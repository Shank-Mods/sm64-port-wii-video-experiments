#ifndef WTB_H
#define WTB_H

// Wii test bench client (protocol WTB/1, see Shank-Mods/PrivateTestWii PROTOCOL.md).
//
// A run is requested by the runner on the command line (wiiload passes arguments to the DOL),
// or for manual tests by a text file of the same lines on the SD card:
//
//   wtb=HOST:PORT:JOBID     report to the runner listening at HOST:PORT (IPv4)
//   cfg=NAME=VALUE          a config override for this run only (repeatable)
//
// The game reports over TCP: HELLO when it has started, then FILEs (results, logs) and DONE
// when it's finished, then exits back to the loader. Nothing here touches the SD card except
// reading the optional request file. Wii only (needs the network).

#include <stdbool.h>
#include <stddef.h>

// Reads the request from argv (argc/argv as main got them) or, failing that, from file_path
// (NULL: none). Brings the network up and sends HELLO. Returns true if a test run is active.
bool wtb_init(int argc, char **argv, const char *file_path, const char *game, const char *build);
bool wtb_active(void);

// Config overrides of this run, in order
int wtb_cfg_count(void);
const char *wtb_cfg_name(int i);
const char *wtb_cfg_value(int i);

// Progress text for the runner's log (best effort)
void wtb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Sends a file's contents (name: what the runner saves it as)
bool wtb_send_file(const char *name, const void *data, size_t len);
// Sends a file from the SD card if it exists
bool wtb_send_path(const char *name, const char *path);
// Final report ("ok", or a short reason); the caller then exits
void wtb_done(const char *status);

// Screenshots. The runner schedules them with cfg=wtb_shots=LIST (handled here; not listed by
// wtb_cfg_*): seconds since wtb_init, "5,20.5,60", and/or "every:N" (every N seconds), at most
// WTB_MAX_SHOTS. The game calls wtb_shot_due() once per image it puts on screen; when it returns
// true, it hands the image that's about to be shown to wtb_shot_add, copied straight from the
// XFB (YUY2, Y0 U Y1 V, the console's framebuffer format). Shots are kept in memory and sent by
// wtb_shots_send (call it before wtb_done), so sending doesn't disturb the run's timing.
#define WTB_MAX_SHOTS 32
// Times screenshots by the game's own clock (milliseconds; e.g. game ticks, so a shot lands on
// the same moment on any console speed) instead of real time since wtb_init
void wtb_set_clock(unsigned (*ms)(void));
bool wtb_shot_due(void);
// Adds to the last shot again (its other field, from the next image): true if there was one
bool wtb_shot_again(void);
// kind: "frame" (a whole image), or "top" / "bottom" (one field of an interlaced image; the
// runner weaves a top and a bottom added for the same shot into one frame)
bool wtb_shot_add(const char *kind, int w, int h, const void *yuy2, int stride_bytes);
void wtb_shots_send(void);

// Crashes and freezes. Called by the game once per image it puts on screen (and from any long
// loop that should count as progress): no call for wtb_hang_s seconds (cfg=wtb_hang_s=N, default
// 20; 60 before the first call) counts as a freeze. A crash (DSI, ISI, program, alignment...)
// is caught in test runs: the crashing thread is parked and a watchdog thread sends crash.txt
// (exception, registers, call chain) or hang.txt, then DONE "crash: ..." / "hang: ...", and
// returns to the Homebrew Channel, so the next run can start without anyone touching the Wii.
// cfg=wtb_test=crash@T|dsi@T|hang@T makes the game crash or freeze T seconds in (to test this).
void wtb_alive(void);
// A short note of what the game is doing (scene, phase); the last 8 go into crash/hang reports
void wtb_mark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
