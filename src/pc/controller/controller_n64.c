#ifdef TARGET_GX

// Real N64 controllers on the GameCube ports, through a passive 3-wire adapter (data, ground,
// 3.3 V; never the 5 V pin 1). The console's serial interface speaks the N64's Joybus protocol
// itself: 0x00 asks a device what it is (an N64 controller answers 05 00 xx), 0x01 reads an N64
// controller's buttons and stick. Proven on hardware with the N64 controller tester
// (Shank-Mods/n64_pad_tester_wii); this is its protocol code, made synchronous so the game
// reads the controller at the moment it asks for input (no added frame of lag).
//
// The N64 controller's reply is the N64's own pad data, so it is passed through as is: every
// button lands where the game expects it and the stick is 1:1, exactly as on the N64.
//
// Ports holding a GameCube controller belong to libogc's PAD library (controller_gamecube.c),
// which only auto-polls ports where it found a GameCube controller. This backend only probes
// the ports PAD reports as empty, so the two never fight over a port. A GameCube controller
// plugged into an empty port later is handed back to PAD.

#include <stdbool.h>
#include <string.h>

#include "../gfx/gfx_gx_wm.h" // gccore.h (which newer libogc's lwp_watchdog.h pulls in), with its Mtx/Vtx renamed away from the game's
#include <ogc/si.h>
#include <ogc/pad.h>
#include <ogc/lwp_watchdog.h>

#include <ultra64.h>

#include "controller_api.h"
#include "../configfile.h"

#define N64_PORTS 4
#define CMD_INFO 0x00
#define CMD_N64_POLL 0x01
#define XFER_TIMEOUT_US 3000  // an N64 poll takes ~0.2 ms; no response is reported well within this
#define PROBE_INTERVAL 30     // reads between probes of an empty port (~1 s at 30 reads a second)
#define MAX_POLL_ERRORS 3     // consecutive failed polls before the controller counts as unplugged
#define STICK_OVERRIDE 2      // the N64 stick overrides other controllers' sticks when off centre

enum PortKind { PORT_EMPTY, PORT_N64, PORT_OTHER };

struct N64Port {
    u8 out[32] __attribute__((aligned(32))); // SI copies these as 32-bit words
    u8 in[32] __attribute__((aligned(32)));
    volatile int done;
    volatile u32 status;
    enum PortKind kind;
    int probe_wait;
    int poll_errors;
};

static struct N64Port ports[N64_PORTS] __attribute__((aligned(32)));
static bool n64_enabled;

static void xfer_done(s32 chan, u32 status) {
    if (chan < 0 || chan >= N64_PORTS) {
        return;
    }
    ports[chan].status = status;
    ports[chan].done = 1;
}

// One Joybus transfer, waited for (true = the device answered without errors)
static bool xfer(int ch, u32 out_len, u32 in_len) {
    struct N64Port *p = &ports[ch];
    memset(p->in, 0, sizeof(p->in));
    p->done = 0;
    p->status = 0;
    if (!SI_Transfer(ch, p->out, out_len, p->in, in_len, xfer_done, 0)) {
        return false; // the channel is busy (another transfer queued on it)
    }
    const u64 start = gettime();
    while (!p->done) {
        if (ticks_to_microsecs(gettime() - start) > XFER_TIMEOUT_US) {
            return false; // the callback still lands in this port's own buffers later
        }
    }
    return p->status == 0;
}

static void probe(int ch) {
    struct N64Port *p = &ports[ch];
    p->out[0] = CMD_INFO;
    if (!xfer(ch, 1, 3)) {
        p->kind = PORT_EMPTY;
        return;
    }
    if (p->in[0] == 0x05 && p->in[1] == 0x00) {
        if (p->kind != PORT_N64) {
            dbglog("n64 controller: found on port %d (id %02X %02X %02X)", ch + 1, p->in[0], p->in[1],
                   p->in[2]);
        }
        p->kind = PORT_N64;
        p->poll_errors = 0;
    } else {
        if (p->in[0] & 0x08) {
            // A GameCube controller plugged in after start: let the PAD library take the port
            PAD_Reset(PAD_CHAN0_BIT >> ch);
        }
        p->kind = PORT_OTHER;
    }
}

// Reads an N64 controller (false = no answer)
static bool poll(int ch, u16 *buttons, s8 *x, s8 *y) {
    struct N64Port *p = &ports[ch];
    p->out[0] = CMD_N64_POLL;
    if (!xfer(ch, 1, 4)) {
        if (++p->poll_errors >= MAX_POLL_ERRORS) {
            dbglog("n64 controller: port %d unplugged", ch + 1);
            p->kind = PORT_EMPTY;
            p->probe_wait = PROBE_INTERVAL;
        }
        return false;
    }
    p->poll_errors = 0;
    *buttons = (u16)((p->in[0] << 8) | p->in[1]);
    *x = (s8)p->in[2];
    *y = (s8)p->in[3];
    return true;
}

static void controller_n64_init(void) {
    n64_enabled = configN64Controller;
    memset(ports, 0, sizeof(ports));
    if (!n64_enabled) {
        return;
    }
    // Look at every port PAD didn't claim once at start, so a controller plugged in before
    // booting works from the first frame
    PADStatus pads[PAD_CHANMAX];
    PAD_Read(pads);
    for (int ch = 0; ch < N64_PORTS; ch++) {
        if (pads[ch].err == PAD_ERR_NO_CONTROLLER) {
            probe(ch);
        }
    }
}

static void controller_n64_read(OSContPad *pad) {
    if (!n64_enabled) {
        return;
    }
    PADStatus pads[PAD_CHANMAX];
    PAD_Read(pads);

    bool used = false;
    for (int ch = 0; ch < N64_PORTS; ch++) {
        struct N64Port *p = &ports[ch];
        if (p->kind != PORT_N64) {
            // Only ports PAD has no controller on; one probe a second
            if (pads[ch].err != PAD_ERR_NO_CONTROLLER) {
                p->kind = PORT_EMPTY;
                continue;
            }
            if (p->probe_wait > 0) {
                p->probe_wait--;
                continue;
            }
            p->probe_wait = PROBE_INTERVAL;
            probe(ch);
            if (p->kind != PORT_N64) {
                continue;
            }
        }
        u16 buttons;
        s8 x, y;
        if (!poll(ch, &buttons, &x, &y) || used) {
            continue; // keep polling every N64 controller (hot-plug), but player 1 is the first
        }
        used = true;
        // The N64's own button layout: A B Z Start, D-pad, L R, C buttons (0x0080, set by
        // L+R+Start, and 0x0040 aren't buttons)
        pad->button |= buttons & ~0x00C0;
        if (x > STICK_OVERRIDE || x < -STICK_OVERRIDE || y > STICK_OVERRIDE || y < -STICK_OVERRIDE) {
            pad->stick_x = x;
            pad->stick_y = y;
        }
    }
}

struct ControllerAPI controller_n64 = {
    controller_n64_init,
    controller_n64_read
};

#endif
