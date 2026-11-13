#ifdef TARGET_GX

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macros.h" // for UNUSED

#include "gfx_gx_wm.h"
#include <ogc/machine/processor.h> // _CPU_ISR_Disable/Restore
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h>      // gettime, millisecs_to_ticks
#include "gfx_screen_config.h"
#include "../configfile.h" // for config60Fps, config240p, configAntialias, configHires1280, configVideoMode
#include "../bench.h"
#include "../early_console.h"

static GXRModeObj *rmode;
#define MAX_XFB 5
static void *framebuffer[MAX_XFB];
static bool fb;
static bool tiled_active; // 1280x240p or 720p: the frame is rendered in EFB tiles
static bool hd_active;    // 720p modes: VI programmed directly, libogc kept on the safe mode

// Three XFBs when presenting without waiting for vsync (and always in 720p): the one the VI is
// scanning, at most one queued for the next retrace, and one to render into. A newer frame
// replaces a queued one that hasn't reached the screen yet, so the display stays at the field
// rate (no tearing) while the game runs as fast as it can.
static bool uncapped;
static bool triple;
static int draw_idx = -1;
static volatile int shown_idx;
static volatile int pending_idx = -1;
static volatile u32 retrace_count; // counted by retrace_callback (debug log and bounded waits)
static volatile u32 last_flip_retrace; // retrace at which the newest frame went up (frame pacing)

// Field rendering (interlaced modes with interlace_render field): every image is one field,
// rendered at half height into a buffer of its own. The VI reads the top field from one base
// register and the bottom field from the other, so a field buffer is put in its own parity's
// register at a retrace and the VI shows it the next time it scans that field. Five buffers:
// the two in the registers, at most one queued per parity, and one being drawn (or two when a
// 30 fps image is drawn as both of its fields).
static bool field_mode;
static int fm_reg[2];                     // buffer in each field's base register
static volatile int fm_pend[2] = { -1, -1 }; // queued for each field
static int fm_draw[2] = { -1, -1 };       // being drawn this image (pass 0, pass 1)
static int fm_next_par;                   // field the next image's first pass is for (0 = top)

#define VI ((volatile u16 *)0xCC002000)
#define HWREG(o) (*(volatile u32 *)(0xCD800000 + (o)))

// ---------------------------------------------------------------------------------------------
// High resolution modes (720p, 960p, 1020p, 1440i, and their 480p crops)
//
// libogc has no notion of these modes. It stays configured for the console's safe mode (the
// preferred 480p/480i) the whole time; the VI is programmed with a complete register image
// written straight to the hardware, and framebuffer flips write the field base registers from
// the retrace callback. No VIDEO_Flush is issued while 720p is up, so libogc's retrace handler
// never rewrites the registers. Leaving puts the boot clock and the safe mode's own register
// image back, then has libogc re-assert the safe mode so its shadow state matches.
//
// Timing law (Wii-VI-Experiments, docs/VI_PLL.md): VI clock F = 54 MHz x Nfb / V, and the bus
// moves one pixel every two clocks (P = F/2). Horizontal values are in pixel clocks; vertical
// ones in half-lines per field (N = total, 3*EQU + PRB + 2*ACV + PSB). Hardware limits found by
// vilinetest: line <= 2046 pixels (10-bit HLW), field <= ~2047 half-lines, PRB >= 1, EQU >= 1.
//
//   1280x720p   SMPTE 296M: 1650 x 750, P 74.25 MHz (Nfb 22 V 8), 60.000 Hz (vimodetest)
//   960x720p    1200 x 750, P 54 MHz (Nfb 14 V 7), 60.000 Hz, 4:3 square pixels (vimodetest)
//   1280x960p   VESA 1280x960@60: 1800 x 1000, P 108 MHz (Nfb 16 V 4), 60.000 Hz
//   1920x1020p  2026 x 1022, P 124.2 MHz (Nfb 23 V 5), 59.98 Hz. Square pixels; 1020 active
//               lines leave only 2 lines of vertical blanking (EQU 1, PRB 1, PSB 0) inside the
//               2047 half-line field limit, so it needs a receiver that tolerates that
//   1920x1440i  2038 x 1501 lines per frame (720 per field), P 91.8 MHz (Nfb 17 V 5), 60.02 Hz
//               fields, 4:3 square pixels. The frame is rendered progressive; each field
//               reads every other line of the XFB
//
// crop_480p (and 1280x960crop; always on GameCube, which has no programmable VI clock): the
// frame is rendered at full size, every tile drawn and copied out to RAM, but the video signal
// is the console's ordinary libogc 480p/480i. The top left tile is copied into the displayed
// XFB (640 wide, at least one tile tall, so the TV shows the top left 640x480 of the frame);
// every other tile is copied into one tile-sized scratch buffer. The GPU draws the same pixels
// and writes the same number of bytes to RAM as a full-size XFB would get, without needing the
// memory for one (a 1920x1020 XFB is 3.9 MB; the GameCube has 24 MB in all). Identical on Wii
// and GameCube, so the two can be compared.

struct HdTiming {
    const char *name;
    u16 w, h;           // frame (= XFB) size
    u8 tx, ty;          // EFB tile grid (tiles are w/tx x h/ty, at most 640x528)
    bool il;            // interlaced output
    u16 ht, hs, hbp;    // horizontal, in pixel clocks: total, hsync width, back porch
    u16 vn, acv;        // half-lines per field, active lines per field
    u8 equ, psb;        // EQU, PSB of the odd field (PRB is what's left)
    u8 nfb, v;          // PLL
    u32 p_hz;           // pixel rate (F/2)
    float dar;          // display aspect ratio (0: w / h, square pixels)
};
static const struct HdTiming hd_1280  = { "1280x720p",  1280,  720, 2, 2, false, 1650,  40, 220, 1500,  720, 10, 2, 22, 8,  74250000 };
static const struct HdTiming hd_960   = { "960x720p",    960,  720, 2, 2, false, 1200,  30, 160, 1500,  720, 10, 2, 14, 7,  54000000 };
static const struct HdTiming hd_1280x960 = { "1280x960p", 1280, 960, 2, 2, false, 1800, 112, 312, 2000,  960,  6, 2, 16, 4, 108000000 };
static const struct HdTiming hd_1920x1020 = { "1920x1020p", 1920, 1020, 3, 2, false, 2026, 44, 54, 2044, 1020, 1, 0, 23, 5, 124200000 };
static const struct HdTiming hd_1920x1440 = { "1920x1440i", 1920, 1440, 3, 3, true, 2038, 44, 66, 1501, 720, 6, 3, 17, 5, 91800000 };
static const struct HdTiming *hd = &hd_1280;
static struct HdTiming hd_custom;  // generated (video_mode WxH[p|i][rate] or custom)
static char hd_custom_name[40];
static struct HdTiming hd_sel;     // the selected mode, with its tile grid for how it's rendered
static bool sel_custom;            // hd_sel came from the generator
// crop: the mode whose frame size and tile grid are rendered (its timing is unused)
static const struct HdTiming *crop_t;
static char crop_name[24];
static bool crop_active;

static const u8 img_regs[] = {
    0x00, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A,
    0x1C, 0x1E, 0x20, 0x22, 0x24, 0x26, 0x28, 0x2A,
    0x30, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E,
    0x48, 0x4A, 0x4C, 0x4E, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x5C, 0x5E, 0x60, 0x62,
    0x64, 0x66, 0x68, 0x6A, 0x6C, 0x70 };
#define NIMG (sizeof(img_regs))
struct VIImage { u16 v[NIMG]; u16 dcr; };

static GXRModeObj *safe_mode;
static struct VIImage safe_img, hd_img;
static u32 boot1c4, boot1c8;
static bool hd_programmed;

static u16 img_mask(u8 off) {
    // display-interrupt registers: bit 15 is the interrupt status (write 0 = clear)
    if (off == 0x30 || off == 0x34 || off == 0x38 || off == 0x3C)
        return 0x7FFF;
    return 0xFFFF;
}

static void img_read(struct VIImage *im) {
    for (u32 i = 0; i < NIMG; i++)
        im->v[i] = VI[img_regs[i] / 2] & img_mask(img_regs[i]);
    im->dcr = VI[0x02 / 2];
}

static void img_set(struct VIImage *im, u8 off, u16 v) {
    for (u32 i = 0; i < NIMG; i++) {
        if (img_regs[i] == off) {
            im->v[i] = v;
            return;
        }
    }
}

// Field base register values for an address
static void fbb_values(void *xfb, u16 *tfbl_hi, u16 *tfbl_lo) {
    u32 t = MEM_VIRTUAL_TO_PHYSICAL(xfb);
    u32 flag = 0;
    if (t >= 0x01000000) {
        flag = 1; // page offset: address in 32-byte units (upper MEM1 and MEM2)
        t >>= 5;
    }
    *tfbl_hi = (u16)((flag << 12) | ((t >> 16) & 0xFF));
    *tfbl_lo = (u16)(t & 0xFFFF);
}

// Interlaced scanout: the bottom field starts one XFB line down, and each field reads every
// other line
static bool scan_il(void) {
    return hd->il && !field_mode; // field rendering: each field has a buffer of its own
}

static void fbb_pair(void *xfb, u16 *hi, u16 *lo, u16 *bhi, u16 *blo) {
    fbb_values(xfb, hi, lo);
    fbb_values((u8 *)xfb + (scan_il() ? hd->w * 2 : 0), bhi, blo);
}

static void hd_set_fbb(void *xfb) {
    u16 hi, lo, bhi, blo;
    fbb_pair(xfb, &hi, &lo, &bhi, &blo);
    VI[0x1C / 2] = hi;
    VI[0x1E / 2] = lo;
    VI[0x24 / 2] = bhi;
    VI[0x26 / 2] = blo;
}

// Field rendering: one field's base register
static void fm_set_field(int parity, void *xfb) {
    u16 hi, lo;
    fbb_values(xfb, &hi, &lo);
    if (parity == 0) {
        VI[0x1C / 2] = hi;
        VI[0x1E / 2] = lo;
    } else {
        VI[0x24 / 2] = hi;
        VI[0x26 / 2] = lo;
    }
}

static void img_set_fbb(void *xfb) {
    u16 hi, lo, bhi, blo;
    fbb_pair(xfb, &hi, &lo, &bhi, &blo);
    img_set(&hd_img, 0x1C, hi);
    img_set(&hd_img, 0x1E, lo);
    img_set(&hd_img, 0x24, bhi);
    img_set(&hd_img, 0x26, blo);
}

// Complete register image for the mode (the formulas of vimodetest's calc_mode/build_image)
static void hd_build_image(void *xfb) {
    const u16 il = hd->il ? 1 : 0;
    const u16 hlw = hd->ht / 2;
    const u16 hsy = hd->hs;
    const u16 hbe = hd->hs + hd->hbp;
    const u16 hbs = hbe + hd->w - hlw;
    // colour-burst window (analog encoder only), at the same point in time as libogc's 71..105
    // of an 858-clock line, kept inside the blanking
    u16 hcs = (u16)((71u * hd->ht + 429) / 858);
    u16 hce = (u16)((105u * hd->ht + 429) / 858);
    if (hce >= hbe)
        hce = hbe - 1;
    if (hcs >= hce)
        hcs = hce - 1;
    const u16 psbO = hd->psb;
    const u16 prbO = hd->vn - 3 * hd->equ - 2 * hd->acv - hd->psb;
    const u16 prbE = prbO + il;
    const u16 psbE = psbO - il;
    const u16 bsO = 2 * hd->equ, bsE = bsO + il;
    const u16 beO = hd->vn - hd->equ + il, beE = beO - il;
    const u16 wpl = hd->w / 16;
    const u16 std = scan_il() ? wpl * 2 : wpl;

    hd_img = safe_img; // filters, DI2/DI3, scaler width etc.
    img_set(&hd_img, 0x6C, 1); // VICLK: F passed through
    img_set(&hd_img, 0x00, (u16)((hd->acv << 4) | hd->equ));
    img_set(&hd_img, 0x04, (u16)((hcs << 8) | hce));
    img_set(&hd_img, 0x06, hlw);
    img_set(&hd_img, 0x08, (u16)(((hbe >> 9) & 1) | (hbs << 1)));
    img_set(&hd_img, 0x0A, (u16)(((hbe & 0x1FF) << 7) | hsy));
    img_set(&hd_img, 0x0C, psbO);
    img_set(&hd_img, 0x0E, prbO);
    img_set(&hd_img, 0x10, psbE);
    img_set(&hd_img, 0x12, prbE);
    img_set(&hd_img, 0x14, (u16)((beO << 5) | bsO));
    img_set(&hd_img, 0x16, (u16)((beO << 5) | bsO));
    img_set(&hd_img, 0x18, (u16)((beE << 5) | bsE));
    img_set(&hd_img, 0x1A, (u16)((beE << 5) | bsE));
    img_set_fbb(xfb);
    img_set(&hd_img, 0x30, (u16)(0x1000 | (hd->vn / 2 + 1)));                 // retrace interrupt
    img_set(&hd_img, 0x32, (u16)(((hd->vn & 1) ? hlw : 0) + 1));               // once per field
    img_set(&hd_img, 0x34, 0x1001);
    img_set(&hd_img, 0x36, 0x0001);
    img_set(&hd_img, 0x48, (u16)((wpl << 8) | std)); // fetch width and stride, in 16-pixel units
    img_set(&hd_img, 0x4A, 0x0100);                  // horizontal scaler off
    hd_img.dcr = (safe_img.dcr & 0x0300) | 0x0001 | (il ? 0 : 0x0004); // keep FMT; enable, NIN
}

static void delay_ms(u32 ms) {
    u64 to = gettime() + millisecs_to_ticks(ms);
    while (gettime() < to) {
    }
}

// VIDEO_WaitVSync with a timeout: returns false (and logs) if no retrace interrupt arrives,
// instead of hanging the game when the VI isn't interrupting in a custom mode
static bool wait_vsync_ms(u32 ms, const char *what) {
    u32 r = retrace_count;
    u64 to = gettime() + millisecs_to_ticks(ms);
    while (retrace_count == r) {
        if (gettime() > to) {
            dbglog("TIMEOUT: no retrace in %u ms (%s); DCR=%04X beam V=%u H=%u", (unsigned)ms, what,
                   VI[0x02 / 2], VI[0x2C / 2] & 0x7FF, VI[0x2E / 2] & 0x7FF);
            return false;
        }
    }
    return true;
}

// Retrace rate and beam counter range over ~0.5 s: tells whether the VI is running and at
// what field rate, with no display needed
static void log_vi_activity(const char *what) {
    u32 r0 = retrace_count;
    u64 t0 = gettime(), to = t0 + millisecs_to_ticks(500);
    u32 vmax = 0, hmax = 0;
    while (gettime() < to) {
        u32 v = VI[0x2C / 2] & 0x7FF, h = VI[0x2E / 2] & 0x7FF;
        if (v > vmax) vmax = v;
        if (h > hmax) hmax = h;
    }
    u32 n = retrace_count - r0;
    u32 us = ticks_to_microsecs(gettime() - t0);
    dbglog("VI activity (%s): %u retraces in %u us = %u.%02u Hz, beam V max %u, H max %u, DCR=%04X",
           what, (unsigned)n, (unsigned)us, us ? (unsigned)(n * 1000000ull / us) : 0,
           us ? (unsigned)((n * 100000000ull / us) % 100) : 0, (unsigned)vmax, (unsigned)hmax, VI[0x02 / 2]);
}

static u32 hd_pll_1c4(void) {
    return (boot1c4 & ~0x0000FF00u) | ((u32)(hd->nfb - 1) << 8); // byte 2 is never touched
}

static u32 hd_pll_1c8(void) {
    return (boot1c8 & ~0x00F00000u) | ((u32)hd->v << 20);
}

// VI off -> reset -> clock -> settle -> complete register image -> VI on
static void apply_image(const struct VIImage *im, u32 p4, u32 p8) {
    u32 l;
    _CPU_ISR_Disable(l);
    VI[0x02 / 2] = VI[0x02 / 2] & ~0x0001; // stop scanout and fetches
    _CPU_ISR_Restore(l);
    delay_ms(2);
    _CPU_ISR_Disable(l);
    VI[0x02 / 2] = 0x0002; // reset counters / field state
    _CPU_ISR_Restore(l);
    delay_ms(1);
    _CPU_ISR_Disable(l);
    VI[0x02 / 2] = 0x0000;
    bool pll_change = (HWREG(0x1C4) & 0x7FFFFFFFu) != (p4 & 0x7FFFFFFFu) || HWREG(0x1C8) != p8;
    HWREG(0x1C4) = p4;
    HWREG(0x1C8) = p8;
    _CPU_ISR_Restore(l);
    if (pll_change)
        delay_ms(50); // PLL relock
    _CPU_ISR_Disable(l);
    for (u32 i = 0; i < NIMG; i++)
        VI[img_regs[i] / 2] = im->v[i];
    VI[0x02 / 2] = im->dcr; // enable last
    _CPU_ISR_Restore(l);
}

static void log_image(const char *what, const struct VIImage *im) {
    for (u32 i = 0; i < NIMG; i += 8) {
        char line[128];
        int n = snprintf(line, sizeof(line), "VI %s:", what);
        for (u32 j = i; j < i + 8 && j < NIMG; j++)
            n += snprintf(line + n, sizeof(line) - n, " %02X=%04X", img_regs[j], im->v[j]);
        dbglog("%s", line);
    }
    dbglog("VI %s: DCR=%04X", what, im->dcr);
}

// Put the console back on its stock clock and the safe mode (also called on the way out)
void gfx_gx_wm_shutdown(void) {
#ifdef __wii__
    *(volatile u32 *)0xCD0000C0 &= ~0x20u; // heartbeat LED off
#endif
    if (!hd_programmed)
        return;
    hd_programmed = false;
    VIDEO_SetPostRetraceCallback(NULL);
    VIDEO_SetBlack(true);
    apply_image(&safe_img, boot1c4, boot1c8);
    VIDEO_Configure(safe_mode);
    VIDEO_SetNextFramebuffer(framebuffer[0]);
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    dbglog("wm: %s left, PLL restored (0x1C4=%08X 0x1C8=%08X)", hd->name, (unsigned)HWREG(0x1C4), (unsigned)HWREG(0x1C8));
}

// GX view of the 720p modes. Only the fields GX and the renderer use matter: libogc never
// configures this object (VIDEO_Configure always gets the safe mode). fbWidth is set from the
// selected timing.
static GXRModeObj TVHd720 =
{
    VI_TVMODE_NTSC_PROG,
    1280,       // fbWidth (XFB stride)
    360,        // efbHeight: one EFB tile
    720,        // xfbHeight
    0, 0,
    640, 480,   // viWidth/viHeight: unused
    VI_XFBMODE_SF,
    GX_FALSE,
    GX_FALSE,
    {
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6}
    },
    { 0, 0, 21, 22, 21, 0, 0 }
};

// ---------------------------------------------------------------------------------------------

static void retrace_callback(UNUSED u32 count)
{
    bool new_frame = false;
    retrace_count++;
    if (hd_active && field_mode) {
        // Each queued field goes into its own field's register; the VI picks it up the next
        // time it scans that field
        for (int p = 0; p < 2; p++) {
            if (fm_pend[p] >= 0) {
                fm_set_field(p, framebuffer[fm_pend[p]]);
                fm_reg[p] = fm_pend[p];
                fm_pend[p] = -1;
                new_frame = true;
            }
        }
        if (new_frame)
            last_flip_retrace = retrace_count;
    } else if (hd_active) {
        // The retrace interrupt comes at the end of the field's active picture, so the queued
        // buffer is read from the next field on and the one that was showing is free again
        if (pending_idx >= 0) {
            hd_set_fbb(framebuffer[pending_idx]);
            shown_idx = pending_idx;
            pending_idx = -1;
            new_frame = true;
            last_flip_retrace = retrace_count;
        }
    } else if (pending_idx >= 0 && VIDEO_GetCurrentFramebuffer() == framebuffer[pending_idx]) {
        shown_idx = pending_idx;
        pending_idx = -1;
        new_frame = true;
        last_flip_retrace = retrace_count;
    }
    bench_on_retrace(new_frame);
}

// 1280x240p (4:3): a 240p mode whose XFB is 1280 pixels wide, scanned out with a 54 MHz VI pixel
// clock so all 1280 pixels fit in the active line. The EFB is only 640 wide, so gfx_gx.c renders
// each frame as two 640x240 tiles and copies them side by side into the XFB.
static GXRModeObj TVNtsc1280x240 =
{
    VI_TVMODE_NTSC_DS,
    HIRES_XFB_WIDTH,  // fbWidth
    240,              // efbHeight
    240,              // xfbHeight
    (VI_MAX_WIDTH_NTSC - 640) / 2,
    (VI_MAX_HEIGHT_NTSC / 2 - 480 / 2) / 2,
    640,              // viWidth (libogc's view; the real horizontal timing is poked below)
    480,              // viHeight
    VI_XFBMODE_SF,
    GX_FALSE,
    GX_FALSE,
    {
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6},
        {6,6},{6,6},{6,6}
    },
    { 0, 0, 21, 22, 21, 0, 0 }
};

bool gfx_gx_wm_hires(void)
{
    return tiled_active;
}

bool gfx_gx_wm_hd(void)
{
    return hd_active;
}

bool gfx_gx_wm_crop(void)
{
    return crop_active;
}

// The high resolution mode being rendered (a direct-VI mode or a crop), or NULL
static const struct HdTiming *frame_mode(void)
{
    return hd_active ? hd : (crop_active ? crop_t : NULL);
}

// Display aspect ratio of the high resolution modes, else 0
float gfx_gx_wm_frame_aspect(void)
{
    const struct HdTiming *t = frame_mode();
    if (t == NULL)
        return 0.0f;
    if (configVideoAspect != VIDEO_ASPECT_AUTO || sel_custom)
        return configfile_video_dar(t->w, t->h);
    return t->dar > 0.0f ? t->dar : (float)t->w / (float)t->h;
}

bool gfx_gx_wm_field_mode(void)
{
    return field_mode;
}

const char *gfx_gx_wm_hd_name(void)
{
    static char buf[48];
    if (hd_active && field_mode) {
        snprintf(buf, sizeof(buf), "%s fields", hd->name);
        return buf;
    }
    return hd_active ? hd->name : (crop_active ? crop_name : NULL);
}

// Height actually rendered per image: the field's in field rendering, else the frame's
static int render_h(const struct HdTiming *t)
{
    return (field_mode && t == hd) ? t->h / 2 : t->h;
}

void gfx_gx_wm_get_tiles(int *tiles_x, int *tiles_y, int *tile_w, int *tile_h)
{
    // Grid worked out for the rendered size (e.g. 1920x1440: 3x3 tiles of 640x480);
    // 1280x240p: 2x1 of 640x240
    const struct HdTiming *t = frame_mode();
    *tiles_x = t != NULL ? t->tx : 2;
    *tiles_y = t != NULL ? t->ty : 1;
    *tile_w = t != NULL ? t->w / t->tx : HIRES_TILE_WIDTH;
    *tile_h = t != NULL ? render_h(t) / t->ty : (rmode != NULL ? rmode->efbHeight : 240);
}

int gfx_gx_wm_get_frame_width(void)
{
    const struct HdTiming *t = frame_mode();
    if (t != NULL)
        return t->w;
    return rmode != NULL ? rmode->fbWidth : DESIRED_SCREEN_WIDTH;
}

int gfx_gx_wm_get_frame_height(void)
{
    if (rmode == NULL)
        return DESIRED_SCREEN_HEIGHT;
    const struct HdTiming *t = frame_mode();
    return t != NULL ? render_h(t) : rmode->efbHeight;
}

// Images a 30 or 60 fps frame stays up for, in fields of the selected mode
static int fields_per_image(void)
{
    const int n = (int)(gfx_gx_wm_get_field_hz() / (config60Fps ? 60.0 : 30.0) + 0.5);
    return n < 1 ? 1 : n;
}

// Field rendering: a free field buffer for render pass k of this image
static void *fm_acquire(int k)
{
    if (fm_draw[k] < 0) {
        u32 level = IRQ_Disable();
        for (int i = 0; i < MAX_XFB; i++) {
            if (i != fm_reg[0] && i != fm_reg[1] && i != fm_pend[0] && i != fm_pend[1]
                && i != fm_draw[0] && i != fm_draw[1]) {
                fm_draw[k] = i;
                break;
            }
        }
        IRQ_Restore(level);
        if (fm_draw[k] < 0)
            fm_draw[k] = 0; // can't happen with five buffers; never leave the renderer without one
    }
    return framebuffer[fm_draw[k]];
}

// Render passes per image: field rendering at a frame rate below the field rate draws a frame's
// two fields (one image held for two fields); otherwise one
int gfx_gx_wm_pass_count(void)
{
    if (!field_mode || uncapped)
        return 1;
    return fields_per_image() >= 2 ? 2 : 1;
}

void *gfx_gx_wm_get_pass_buffer(int k)
{
    if (!field_mode)
        return gfx_gx_wm_get_back_buffer();
    return fm_acquire(k);
}

// Vertical offset of render pass k, in rendered lines: the top field's lines sit a quarter of a
// field line above the middle of the half-height image and the bottom field's a quarter below
float gfx_gx_wm_pass_shift(int k)
{
    if (!field_mode)
        return 0.0f;
    return ((fm_next_par + k) & 1) == 0 ? 0.25f : -0.25f;
}

void *gfx_gx_wm_get_back_buffer(void)
{
    if (field_mode)
        return fm_acquire(0);
    if (!triple)
        return framebuffer[fb ^ 1];

    if (draw_idx < 0) {
        u32 level = IRQ_Disable();
        for (int i = 0; i < 3; i++) {
            if (i != shown_idx && i != pending_idx) {
                draw_idx = i;
                break;
            }
        }
        IRQ_Restore(level);
    }
    return framebuffer[draw_idx];
}

bool gfx_gx_wm_uncapped(void)
{
    return uncapped;
}

// Horizontal timing for 1280 active pixels at 54 MHz (858 clocks per half line), taken from a
// register dump of the mode working on hardware.
static void gfx_gx_wm_apply_hires_timing(void)
{
    u32 level;

    _CPU_ISR_Disable(level);
    VI[0x6C / 2] = 0x0001; // VI clock: 54 MHz
    VI[0x04 / 2] = 0x4769; // HTR0: hcs/hce
    VI[0x06 / 2] = 0x035A; // HTR0: half line width = 858
    VI[0x08 / 2] = 0x059A; // HTR1: hbs/hbe/hsy
    VI[0x0A / 2] = 0x93FF;
    VI[0x4A / 2] = 0x0100; // horizontal scaler off
    VI[0x48 / 2] = 0x5050; // picture config: 1280 px stride
    VI[0x02 / 2] = 0x0005; // DCR: enabled, non-interlaced, NTSC
    _CPU_ISR_Restore(level);
}

// ---------------------------------------------------------------------------------------------
// Custom modes: a timing generated from a size, scan and refresh rate

#define VI_HT_MAX 2046     // pixel clocks per line (10-bit half-line width)
#define VI_VN_MAX 2047     // half-lines per field
#define VI_HBLANK_MIN 106  // the narrowest horizontal blanking used on hardware (1920x1020p)
#define PLL_NFB_MIN 14     // feedback multipliers proven on hardware: 14..23 (VCO 756..1242 MHz)
#define PLL_NFB_MAX 23

// Tile grid for a rendered size: as few tiles as fit the EFB (640x528), each the same size,
// widths a multiple of 16 and heights even (what the EFB -> XFB copy needs)
static bool tile_grid(unsigned w, unsigned h, u8 *tx, u8 *ty)
{
    *tx = *ty = 0;
    for (unsigned x = 1; x <= 15 && !*tx; x++)
        if (w % x == 0 && (w / x) % 16 == 0 && w / x <= 640)
            *tx = (u8)x;
    for (unsigned y = 1; y <= 15 && !*ty; y++)
        if (h % y == 0 && (h / y) % 2 == 0 && h / y <= 528)
            *ty = (u8)y;
    return *tx && *ty && *tx * *ty <= 16;
}

// Searches the PLL settings and line lengths for the timing closest to the requested refresh,
// preferring blanking close to the usual proportions (720p's 29% of the line and 4% of the
// lines) and never above the pixel clock limit. why: the reason when nothing fits.
static bool gen_timing(struct HdTiming *t, unsigned w, unsigned h, double rate, bool il, char *why, size_t why_len)
{
    if (w % 16 || w < 320 || w > 2032) {
        snprintf(why, why_len, "width %u: must be a multiple of 16 from 320 to 2032", w);
        return false;
    }
    if (h < 120 || h % (il ? 4 : 2)) {
        snprintf(why, why_len, "height %u: must be at least 120 and a multiple of %u", h, il ? 4 : 2);
        return false;
    }
    if (w + VI_HBLANK_MIN > VI_HT_MAX) {
        snprintf(why, why_len, "width %u: a line can't be longer than %u pixels with blanking", w, VI_HT_MAX);
        return false;
    }
    if (rate < 20.0 || rate > 1000.0) {
        snprintf(why, why_len, "refresh %.2f Hz is out of range", rate);
        return false;
    }
    const unsigned acv = il ? h / 2 : h;                  // active lines per field
    const unsigned vn_min = 2 * acv + (il ? 5 : 4);       // EQU 1, PRB 1, PSB 1 (interlaced)
    if (vn_min > VI_VN_MAX) {
        snprintf(why, why_len, "%u active lines per field: the field limit is about %u", acv, (VI_VN_MAX - 5) / 2);
        return false;
    }
    const double pmax = (double)configVideoMaxPclk * 1e6;
    const double hb_t = fmax(VI_HBLANK_MIN, w * 0.29), vb_t = fmax(3.0, acv * 0.042);
    double best = 1e30;
    unsigned b_nfb = 0, b_v = 0, b_ht = 0, b_vn = 0;
    double b_p = 0;
    for (unsigned nfb = PLL_NFB_MIN; nfb <= PLL_NFB_MAX; nfb++) {
        for (unsigned v = 1; v <= 15; v++) {
            const double p = 54e6 * nfb / v / 2.0;
            if (p > pmax)
                continue;
            for (unsigned ht = (w + VI_HBLANK_MIN + 1) & ~1u; ht <= VI_HT_MAX; ht += 2) {
                // The field lengths (odd interlaced, even progressive) either side of the exact one
                const double vnf = 2.0 * p / (ht * rate);
                if (vnf < 3.0)
                    continue;
                const unsigned lo = il ? 2 * (unsigned)floor((vnf - 1.0) / 2.0) + 1 : 2 * (unsigned)floor(vnf / 2.0);
                for (unsigned vn = lo; vn <= lo + 2; vn += 2) {
                    if (vn < vn_min || vn > VI_VN_MAX)
                        continue;
                    const double actual = p / (ht * (vn / 2.0));
                    const double err = fabs(actual - rate) / rate;
                    if (err > 0.005)
                        continue;
                    const double score = err * 1000.0 + fabs((ht - w) - hb_t) / hb_t
                                         + 0.5 * fabs((vn - 2.0 * acv) / 2.0 - vb_t) / vb_t;
                    if (score < best) {
                        best = score;
                        b_nfb = nfb;
                        b_v = v;
                        b_ht = ht;
                        b_vn = vn;
                        b_p = p;
                    }
                }
            }
        }
    }
    if (b_ht == 0) {
        snprintf(why, why_len, "no timing for %ux%u%c at %.2f Hz within the limits (pixel clock <= %.1f MHz, "
                 "line <= %u, field <= %u half-lines)", w, h, il ? 'i' : 'p', rate, (double)configVideoMaxPclk,
                 VI_HT_MAX, VI_VN_MAX);
        return false;
    }

    memset(t, 0, sizeof(*t));
    t->w = (u16)w;
    t->h = (u16)h;
    t->il = il;
    t->ht = (u16)b_ht;
    const unsigned blank = b_ht - w;
    t->hs = (u16)(blank >= 200 ? 40 : 32);
    const unsigned hfp = blank / 4 > 8 ? blank / 4 : 8;
    t->hbp = (u16)(blank - t->hs - hfp);
    t->vn = (u16)b_vn;
    t->acv = (u16)acv;
    // Vertical blanking (half-lines): 3 EQU + PRB + PSB
    const int vbh = (int)b_vn - 2 * (int)acv;
    int psb = il ? (vbh >= 12 ? 3 : 1) : (vbh >= 10 ? 2 : 0);
    int equ = vbh / 10;
    if (equ < 1) equ = 1;
    if (equ > 10) equ = 10;
    while (vbh - 3 * equ - psb < 1 && equ > 1)
        equ--;
    if (vbh - 3 * equ - psb < 1) {
        snprintf(why, why_len, "vertical blanking too short");
        return false;
    }
    t->equ = (u8)equ;
    t->psb = (u8)psb;
    t->nfb = (u8)b_nfb;
    t->v = (u8)b_v;
    t->p_hz = (u32)(b_p + 0.5);
    return true;
}

// Pick the video mode to render in
// Options are 720p, 1280x240p, 240p or the console's own 480p/480i (576i for PAL)
static GXRModeObj *gfx_gx_wm_select_mode(void)
{
    const struct HdTiming *sel = NULL;
    bool force_crop = false;
    switch (configVideoMode) {
        case VIDEO_MODE_720P:       sel = &hd_1280; break;
        case VIDEO_MODE_960X720P:   sel = &hd_960; break;
        case VIDEO_MODE_1280X960P:  sel = &hd_1280x960; break;
        case VIDEO_MODE_1920X1020P: sel = &hd_1920x1020; break;
        case VIDEO_MODE_1920X1440I: sel = &hd_1920x1440; break;
        case VIDEO_MODE_960P_CROP:  sel = &hd_1280x960; force_crop = true; break;
        case VIDEO_MODE_CUSTOM: {
            const struct VideoCustom *c = &configVideoCustom;
            char why[160];
            snprintf(hd_custom_name, sizeof(hd_custom_name), "%ux%u%c%g", c->width, c->height,
                     c->interlaced ? 'i' : 'p', (double)c->refresh);
            if (gen_timing(&hd_custom, c->width, c->height, c->refresh, c->interlaced, why, sizeof(why))) {
                hd_custom.name = hd_custom_name;
                sel = &hd_custom;
                sel_custom = true;
                dbglog("custom mode %s: %u x %u half-lines, active %u, EQU %u PSB %u, P %.3f MHz (Nfb %u V %u), "
                       "%.3f Hz", hd_custom_name, hd_custom.ht, hd_custom.vn, hd_custom.acv, hd_custom.equ,
                       hd_custom.psb, hd_custom.p_hz / 1e6, hd_custom.nfb, hd_custom.v,
                       2.0 * ((double)hd_custom.p_hz / hd_custom.ht) / hd_custom.vn);
            } else {
                dbglog("custom mode %s not possible: %s; using the console's own mode", hd_custom_name, why);
            }
            break;
        }
        default: break;
    }
#ifndef __wii__
    force_crop = true; // the GameCube's VI clock can't be raised: only the crops exist there
#endif
    const bool crop = sel != NULL && (configCrop480 || force_crop);
    if (sel != NULL) {
        // Rendered size and tile grid: per field with field rendering (not for the crops, which
        // show the top left of a whole frame)
        hd_sel = *sel;
        hd = &hd_sel;
        field_mode = !crop && hd_sel.il && configInterlaceField;
        const unsigned rh = field_mode ? hd_sel.h / 2 : hd_sel.h;
        if (!tile_grid(hd_sel.w, rh, &hd_sel.tx, &hd_sel.ty)) {
            dbglog("mode %s: no tile grid for %ux%u; using the console's own mode", hd_sel.name, hd_sel.w, rh);
            sel = NULL;
            field_mode = false;
        } else {
            dbglog("mode %s%s: rendered %ux%u as %ux%u tiles of %ux%u", hd_sel.name,
                   field_mode ? " (field rendering)" : "", hd_sel.w, rh, hd_sel.tx, hd_sel.ty,
                   hd_sel.w / hd_sel.tx, rh / hd_sel.ty);
        }
    }
    if (sel != NULL && crop) {
        // Normal 480p/480i signal; the frame is rendered at sel's size
        crop_t = &hd_sel;
        snprintf(crop_name, sizeof(crop_name), "%ux%u>480", hd_sel.w, hd_sel.h);
        crop_active = true;
        tiled_active = true;
        return VIDEO_GetPreferredMode(NULL);
    }
#ifdef __wii__
    if (sel != NULL) {
        TVHd720.fbWidth = hd->w;
        TVHd720.xfbHeight = render_h(hd);
        TVHd720.efbHeight = render_h(hd) / hd->ty;
        hd_active = true;
        tiled_active = true;
        return &TVHd720;
    }
#endif

    if (configHires1280) {
        tiled_active = true;
        return &TVNtsc1280x240;
    }

    if (!config240p)
        return VIDEO_GetPreferredMode(NULL);

    switch (VIDEO_GetCurrentTvMode())
    {
        case VI_PAL:
            return configAntialias ? &TVPal264DsAa : &TVPal264Ds;
        case VI_MPAL:
            return configAntialias ? &TVMpal240DsAa : &TVMpal240Ds;
        case VI_EURGB60:
            return configAntialias ? &TVEurgb60Hz240DsAa : &TVEurgb60Hz240Ds;
        case VI_NTSC:
        default:
            return configAntialias ? &TVNtsc240DsAa : &TVNtsc240Ds;
    }
}

static void dbg_vi_regs(const char *tag)
{
    for (int r = 0; r < 0x78; r += 16)
        dbglog("VI %s %02X: %04X %04X %04X %04X %04X %04X %04X %04X", tag, r,
               VI[r / 2], VI[r / 2 + 1], VI[r / 2 + 2], VI[r / 2 + 3],
               VI[r / 2 + 4], VI[r / 2 + 5], VI[r / 2 + 6], VI[r / 2 + 7]);
}

static void log_arenas(const char *what)
{
#ifdef __wii__
    dbglog("wm_init: %s: MEM1 arena %p-%p, MEM2 arena %p-%p", what, SYS_GetArenaLo(), SYS_GetArenaHi(),
           SYS_GetArena2Lo(), SYS_GetArena2Hi());
#else
    dbglog("wm_init: %s: arena %p-%p (%u KB free)", what, SYS_GetArenaLo(), SYS_GetArenaHi(),
           (unsigned)(((u8 *)SYS_GetArenaHi() - (u8 *)SYS_GetArenaLo()) / 1024));
#endif
}

static void clear_xfb(void *xfb, u32 bytes)
{
    u32 *p = (u32 *)xfb; // uncached
    for (u32 i = 0; i < bytes / 4; i++)
        p[i] = 0x10801080; // YUV black
}

static void gfx_gx_wm_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen)
{
    dbglog("wm_init: VIDEO_Init");
    if (!gEarlyVideoUp) // the diagnostic console already did it
        VIDEO_Init();
    VIDEO_SetBlack(true);
    dbglog("wm_init: tvmode=%d component=%d", VIDEO_GetCurrentTvMode(), VIDEO_HaveComponentCable());
    dbg_vi_regs("boot");

    rmode = gfx_gx_wm_select_mode();
    dbglog("wm_init: mode viTVMode=%u fb=%ux%u efb=%u vi=%ux%u tiled=%d hd=%d crop=%s", rmode->viTVMode,
           rmode->fbWidth, rmode->xfbHeight, rmode->efbHeight, rmode->viWidth, rmode->viHeight,
           tiled_active, hd_active, crop_active ? crop_name : "no");

    // Double buffering when presenting at vsync, triple when uncapped; 720p always uses the
    // retrace-driven queue, so always three there
    u32 fb_bytes = VIDEO_GetFrameBufferSize(rmode);
    if (crop_active) {
        // The top left tile is copied in whole, so the XFB must be at least a tile tall
        int tx, ty, tw, th;
        gfx_gx_wm_get_tiles(&tx, &ty, &tw, &th);
        u32 lines = rmode->xfbHeight > (u32)th ? rmode->xfbHeight : (u32)th;
        fb_bytes = (u32)rmode->fbWidth * lines * 2;
    }
    log_arenas("before XFBs");
    framebuffer[0] = MEM_K0_TO_K1(memalign(32, fb_bytes));
    framebuffer[1] = MEM_K0_TO_K1(memalign(32, fb_bytes));
    if ((configBenchUncapped || configBenchRender) || hd_active) {
        void *third = memalign(32, fb_bytes);
        if (third != NULL) {
            framebuffer[2] = MEM_K0_TO_K1(third);
            triple = true;
            uncapped = (configBenchUncapped || configBenchRender);
        }
    }
    if (field_mode && hd_active) {
        // Field buffers (each one field tall): five, see fm_*
        for (int i = 3; i < MAX_XFB; i++) {
            void *b = memalign(32, fb_bytes);
            framebuffer[i] = b != NULL ? MEM_K0_TO_K1(b) : NULL;
            if (b == NULL)
                triple = false; // reported as not enough memory below
        }
    }
    dbglog("wm_init: fb0=%p fb1=%p fb2=%p fb3=%p fb4=%p (%u bytes each) triple=%d uncapped=%d field=%d",
           framebuffer[0], framebuffer[1], framebuffer[2], framebuffer[3], framebuffer[4], (unsigned)fb_bytes,
           triple, uncapped, field_mode);
    log_arenas("after XFBs");

    const bool fb_missing = framebuffer[0] == MEM_K0_TO_K1(NULL) || framebuffer[1] == MEM_K0_TO_K1(NULL);
    if ((hd_active && (!triple || fb_missing)) || (crop_active && fb_missing)) {
        // Not enough memory for the XFBs: fall back to the console's own mode
        dbglog("wm_init: not enough memory for the %s XFBs, falling back", hd_active ? hd->name : crop_name);
        hd_active = false;
        crop_active = false;
        tiled_active = false;
        field_mode = false;
        rmode = VIDEO_GetPreferredMode(NULL);
        triple = uncapped = false;
        fb_bytes = VIDEO_GetFrameBufferSize(rmode);
        framebuffer[0] = MEM_K0_TO_K1(memalign(32, fb_bytes));
        framebuffer[1] = MEM_K0_TO_K1(memalign(32, fb_bytes));
        if ((configBenchUncapped || configBenchRender)) {
            void *third = memalign(32, fb_bytes);
            if (third != NULL) {
                framebuffer[2] = MEM_K0_TO_K1(third);
                triple = uncapped = true;
            }
        }
    }

    for (int i = 0; i < MAX_XFB; i++)
        if (framebuffer[i] != MEM_K0_TO_K1(NULL) && framebuffer[i] != NULL)
            clear_xfb(framebuffer[i], fb_bytes);

    GXRModeObj *vi_mode = rmode;
    if (hd_active) {
        // libogc runs the safe mode underneath; the 720p registers go on top of it below
        safe_mode = VIDEO_GetPreferredMode(NULL);
        vi_mode = safe_mode;
    }

    early_console_handover(); // diagnostic builds: hold the startup text on screen a moment
    VIDEO_Configure(vi_mode);
    dbglog("wm_init: configured (libogc mode %ux%u viTVMode %u)", vi_mode->fbWidth, vi_mode->xfbHeight, vi_mode->viTVMode);

    VIDEO_SetNextFramebuffer(framebuffer[fb]);
    // Never blanked here: the XFBs are already cleared to black, and libogc blanks the screen by
    // programming zero active lines, which the 720p/crop modes would copy from the safe image
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    shown_idx = fb;
    VIDEO_SetPostRetraceCallback(retrace_callback); // counts retraces from here on
    VIDEO_WaitVSync();
    if (vi_mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    dbg_vi_regs("libogc");
    early_console_free();

    if (hd_active) {
        VIDEO_WaitVSync();
        img_read(&safe_img);
        safe_img.dcr |= 0x0001; // the safe image re-enables the VI when restored
        boot1c4 = HWREG(0x1C4);
        boot1c8 = HWREG(0x1C8);
        hd_build_image(framebuffer[0]);
        fm_reg[0] = fm_reg[1] = 0; // field rendering: both fields start on the black buffer 0
        log_image("safe", &safe_img);
        log_image(hd->name, &hd_img);
        dbglog("wm_init: boot PLL 0x1C4=%08X 0x1C8=%08X -> 720p 0x1C4=%08X 0x1C8=%08X",
               (unsigned)boot1c4, (unsigned)boot1c8, (unsigned)hd_pll_1c4(), (unsigned)hd_pll_1c8());
        shown_idx = 0;
        hd_programmed = true;
        atexit(gfx_gx_wm_shutdown);
        log_vi_activity("safe mode, before switch");
        dbglog("wm_init: applying %s image", hd->name);
        apply_image(&hd_img, hd_pll_1c4(), hd_pll_1c8());
        dbglog("wm_init: image applied, PLL now 0x1C4=%08X 0x1C8=%08X", (unsigned)HWREG(0x1C4), (unsigned)HWREG(0x1C8));
        VIDEO_SetPostRetraceCallback(retrace_callback);
        wait_vsync_ms(200, "first field after switch");
        wait_vsync_ms(200, "second field after switch");
        dbg_vi_regs(hd->name);
        log_vi_activity(hd->name);
        dbglog("wm_init: %s up, PLL 0x1C4=%08X 0x1C8=%08X", hd->name, (unsigned)HWREG(0x1C4), (unsigned)HWREG(0x1C8));
    } else {
        // Also needed when presenting at vsync: the benchmark calibrates its clock on retraces
        shown_idx = fb;
        VIDEO_SetPostRetraceCallback(retrace_callback);
    }

    if (tiled_active && !hd_active && !crop_active) {
#ifndef HIRES_NO_VI_POKE
        gfx_gx_wm_apply_hires_timing();
        dbglog("wm_init: hires timing applied");
#else
        dbglog("wm_init: hires timing NOT applied (test build)");
#endif
        VIDEO_WaitVSync();
        dbg_vi_regs("hires");
    }
    if (!hd_active)
        log_vi_activity("running mode");
    dbglog("wm_init: done");
}

GXRModeObj *gfx_gx_wm_get_rmode(void)
{
    return rmode;
}

int gfx_gx_wm_get_field_rate(void)
{
    if (rmode == NULL)
        return 60;

    switch (rmode->viTVMode >> 2)
    {
        case VI_PAL:
        case VI_DEBUG_PAL:
            return 50;
        default:
            return 60;
    }
}

double gfx_gx_wm_get_field_hz(void)
{
    if (hd_active)
        return 2.0 * ((double)hd->p_hz / hd->ht) / hd->vn; // field rate: 2 x line rate / half-lines
    return gfx_gx_wm_get_field_rate() == 50 ? 50.0 : 60000.0 / 1001.0;
}

static void gfx_gx_wm_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode), UNUSED bool (*on_key_up)(int scancode), UNUSED void (*on_all_keys_up)(void))
{
}

static void gfx_gx_wm_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen))
{
}

static void gfx_gx_wm_set_fullscreen(UNUSED bool enable)
{
}

static void gfx_gx_wm_main_loop(void (*run_one_game_iter)(void))
{
    run_one_game_iter();
}

static void gfx_gx_wm_get_dimensions(uint32_t *width, uint32_t *height)
{
    if (config240p || tiled_active) {
        // GX handles all scaling, so we just lie here
        *width = DESIRED_SCREEN_WIDTH;
        *height = DESIRED_SCREEN_HEIGHT;
    } else {
        *width = rmode->fbWidth;
        *height = rmode->xfbHeight;
    }
}

static void gfx_gx_wm_handle_events(void)
{
}

static bool gfx_gx_wm_start_frame(void)
{
    return true;
}

// Waits for the queued frame to reach the screen (720p at vsync), with a timeout so a VI that
// stopped interrupting can't hang the game
static void wait_pending_shown(void)
{
    u64 to = gettime() + millisecs_to_ticks(100);
    while ((pending_idx >= 0 || fm_pend[0] >= 0 || fm_pend[1] >= 0) && gettime() < to) {
    }
}

// Sign of life: the disc-slot LED blinks about twice a second while frames are being produced.
// Picture missing but LED blinking = the game runs and the display chain isn't showing the
// signal; LED stopped = the game is stuck. (Wii only; needs ahb_access.)
static void heartbeat(void)
{
#ifdef __wii__
    static u64 last;
    u64 now = gettime();
    if (now - last >= millisecs_to_ticks(250)) {
        *(volatile u32 *)0xCD0000C0 ^= 0x20; // HW_GPIOB_OUT: slot LED
        last = now;
    }
#endif
}

static void gfx_gx_wm_swap_buffers_begin(void)
{
#ifdef DBGLOG
    if (configDebugFrameDelayMs)
        delay_ms(configDebugFrameDelayMs); // simulated rendering load (testing frame pacing)
#endif
    static unsigned int frames;
    frames++;
    heartbeat();
    if (frames <= 10 || frames % 300 == 0)
        dbglog("swap: frame %u retraces %u beam V=%u H=%u draw=%d shown=%d pending=%d DCR=%04X FBB=%04X%04X",
               frames, (unsigned)retrace_count, VI[0x2C / 2] & 0x7FF, VI[0x2E / 2] & 0x7FF,
               draw_idx, shown_idx, pending_idx, VI[0x02 / 2], VI[0x1C / 2], VI[0x1E / 2]);
    if (frames == 120 && tiled_active)
        dbg_vi_regs("frame120");

    if (triple && field_mode) {
        // Queue each drawn field for its own field's register (replacing an older one still
        // waiting there)
        u32 level = IRQ_Disable();
        int n = 0;
        for (int k = 0; k < 2; k++) {
            if (fm_draw[k] >= 0) {
                fm_pend[(fm_next_par + k) & 1] = fm_draw[k];
                fm_draw[k] = -1;
                n++;
            }
        }
        fm_next_par = (fm_next_par + n) & 1;
        IRQ_Restore(level);
        if (uncapped || n == 0)
            return;
        wait_pending_shown();
        while ((u32)(retrace_count - last_flip_retrace) + 1 < (u32)fields_per_image())
            if (!wait_vsync_ms(100, "frame hold"))
                break;
        return;
    }

    if (triple) {
        // The renderer has already copied the frame into the draw buffer and waited for it
        if (draw_idx < 0)
            return;
        u32 level = IRQ_Disable();
        pending_idx = draw_idx;
        if (!hd_active) {
            VIDEO_SetNextFramebuffer(framebuffer[draw_idx]);
            VIDEO_Flush();
        }
        IRQ_Restore(level);
        draw_idx = -1;
        if (uncapped)
            return;

        // Presenting at the field rate (only reached in the high resolution modes): wait for
        // this frame to go up, and hold it until the 30 or 60 fps frame time has passed (at
        // 60 Hz: a second field at 30 fps; at 120 Hz, four fields)
        wait_pending_shown();
        while ((u32)(retrace_count - last_flip_retrace) + 1 < (u32)fields_per_image())
            if (!wait_vsync_ms(100, "frame hold"))
                break;
        return;
    }

    fb ^= 1; // flip frame buffer

    // In 1280x240p the renderer has already copied both tiles into this buffer
    if (!tiled_active) {
        GX_CopyDisp(framebuffer[fb], GX_TRUE);
        GX_Flush();
    }

    VIDEO_SetNextFramebuffer(framebuffer[fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    // Hold each rendered frame for an extra field so the 30Hz game logic maps to a 30Hz display
    // Skipped at 60fps
    if (!config60Fps) {
        VIDEO_WaitVSync();
    }
}

static void gfx_gx_wm_swap_buffers_end(void)
{
}

static double gfx_gx_wm_get_time(void)
{
    return 0.0;
}

struct GfxWindowManagerAPI gfx_gx_wm_api =
{
    gfx_gx_wm_init,
    gfx_gx_wm_set_keyboard_callbacks,
    gfx_gx_wm_set_fullscreen_changed_callback,
    gfx_gx_wm_set_fullscreen,
    gfx_gx_wm_main_loop,
    gfx_gx_wm_get_dimensions,
    gfx_gx_wm_handle_events,
    gfx_gx_wm_start_frame,
    gfx_gx_wm_swap_buffers_begin,
    gfx_gx_wm_swap_buffers_end,
    gfx_gx_wm_get_time
};

#endif
