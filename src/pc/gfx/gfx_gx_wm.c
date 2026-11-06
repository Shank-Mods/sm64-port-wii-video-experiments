#ifdef TARGET_GX

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

static GXRModeObj *rmode;
static void *framebuffer[3];
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

#define VI ((volatile u16 *)0xCC002000)
#define HWREG(o) (*(volatile u32 *)(0xCD800000 + (o)))

// ---------------------------------------------------------------------------------------------
// 720p modes (1280x720p and 960x720p)
//
// libogc has no notion of this mode. It stays configured for the console's safe mode (the
// preferred 480p/480i) the whole time; the VI is programmed with a complete register image
// written straight to the hardware, and framebuffer flips write the field base registers from
// the retrace callback. No VIDEO_Flush is issued while 720p is up, so libogc's retrace handler
// never rewrites the registers. Leaving puts the boot clock and the safe mode's own register
// image back, then has libogc re-assert the safe mode so its shadow state matches.
//
// Both timings are the ones in tools/vimodetest in Wii-VI-Experiments, with the VI PLL law
// F = 54 MHz x Nfb / V from docs/VI_PLL.md there; the bus moves one pixel every two clocks
// (P = F/2). Both have 750 lines per frame (SMPTE 296M vertical) and a 45.00 kHz line rate,
// so both run at exactly 60.000 Hz:
//   1280x720p: SMPTE 296M, 1650-pixel lines, Nfb 22 V 8 -> F 148.5 MHz, P 74.25 MHz
//   960x720p:  the same line time with 1200-pixel lines (4:3, square pixels),
//              Nfb 14 V 7 -> F 108 MHz, P 54 MHz
//
// A third entry uses the same machinery without changing the video signal at all:
//   1280x960 crop: the frame is rendered at 1280x960 (2x2 tiles of 640x480, every tile copied
//              into a full 1280x960 XFB), and the VI stays on the console's own 480p (or 480i)
//              timing and clock. Only the picture configuration changes: the VI fetches 640
//              pixels per line with a 1280-pixel stride, so it scans out the top left 640x480
//              quadrant. The GPU and CPU do the full 1280x960 work; only the VI's memory reads
//              are those of 640x480.

struct HdTiming {
    const char *name;
    u16 w, h;        // frame (= XFB) size
    u16 tile_h;      // EFB tile height (tiles are w/2 wide, 2x2)
    bool crop;       // VI keeps the safe mode, shows the top left 640 x (safe height)
    u16 ht, hs, hbp; // horizontal, in pixel clocks: total, hsync width, back porch
    u8 nfb, v;       // PLL
    u32 p_hz;        // pixel rate (F/2)
};
static const struct HdTiming hd_1280 = { "1280x720p", 1280, 720, 360, false, 1650, 40, 220, 22, 8, 74250000 };
static const struct HdTiming hd_960  = { "960x720p",   960, 720, 360, false, 1200, 30, 160, 14, 7, 54000000 };
static const struct HdTiming hd_crop = { "1280x960>480", 1280, 960, 480, true, 0, 0, 0, 0, 0, 0 };
static const struct HdTiming *hd = &hd_1280;
static bool crop_il;  // crop mode on an interlaced safe mode (480i/576i)

// Vertical, in half-lines per (progressive) field: total, active lines, EQU, PSB
#define HD_VN 1500
#define HD_VACV 720
#define HD_VEQU 10
#define HD_VPSB 2

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

// Field base registers for a progressive framebuffer (both fields read the same picture)
static void fbb_values(void *xfb, u16 *tfbl_hi, u16 *tfbl_lo) {
    u32 t = MEM_VIRTUAL_TO_PHYSICAL(xfb);
    u32 flag = 0;
    if (t >= 0x01000000) {
        flag = 1;
        t >>= 5;
    }
    *tfbl_hi = (u16)((flag << 12) | ((t >> 16) & 0xFF));
    *tfbl_lo = (u16)(t & 0xFFFF);
}

// Top and bottom field bases. Interlaced crop: the bottom field starts one XFB line down.
static void hd_set_fbb(void *xfb) {
    u16 hi, lo, bhi, blo;
    fbb_values(xfb, &hi, &lo);
    fbb_values((u8 *)xfb + (crop_il ? hd->w * 2 : 0), &bhi, &blo);
    VI[0x1C / 2] = hi;
    VI[0x1E / 2] = lo;
    VI[0x24 / 2] = bhi;
    VI[0x26 / 2] = blo;
}

static void crop_build_image(void *xfb) {
    // Everything as the safe mode has it, except what the VI reads: 640 pixels per line from
    // lines hd->w pixels apart (two lines apart when interlaced: each field reads every other line)
    hd_img = safe_img;
    crop_il = (safe_img.dcr & 0x0004) == 0;
    const u16 wpl = 640 / 16;
    const u16 std = (hd->w / 16) * (crop_il ? 2 : 1);
    img_set(&hd_img, 0x48, (u16)((wpl << 8) | std));
    u16 hi, lo, bhi, blo;
    fbb_values(xfb, &hi, &lo);
    fbb_values((u8 *)xfb + (crop_il ? hd->w * 2 : 0), &bhi, &blo);
    img_set(&hd_img, 0x1C, hi);
    img_set(&hd_img, 0x1E, lo);
    img_set(&hd_img, 0x24, bhi);
    img_set(&hd_img, 0x26, blo);
}

static void hd_build_image(void *xfb) {
    if (hd->crop) {
        crop_build_image(xfb);
        return;
    }
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
    const u16 psb = HD_VPSB;
    const u16 prb = HD_VN - 3 * HD_VEQU - 2 * HD_VACV - HD_VPSB;
    const u16 bs = 2 * HD_VEQU;
    const u16 be = HD_VN - HD_VEQU;
    const u16 wpl = hd->w / 16;

    hd_img = safe_img; // filters, DI2/DI3, scaler width etc.
    img_set(&hd_img, 0x6C, 1); // VICLK: F passed through
    img_set(&hd_img, 0x00, (u16)((HD_VACV << 4) | HD_VEQU));
    img_set(&hd_img, 0x04, (u16)((hcs << 8) | hce));
    img_set(&hd_img, 0x06, hlw);
    img_set(&hd_img, 0x08, (u16)(((hbe >> 9) & 1) | (hbs << 1)));
    img_set(&hd_img, 0x0A, (u16)(((hbe & 0x1FF) << 7) | hsy));
    img_set(&hd_img, 0x0C, psb);
    img_set(&hd_img, 0x0E, prb);
    img_set(&hd_img, 0x10, psb);
    img_set(&hd_img, 0x12, prb);
    img_set(&hd_img, 0x14, (u16)((be << 5) | bs));
    img_set(&hd_img, 0x16, (u16)((be << 5) | bs));
    img_set(&hd_img, 0x18, (u16)((be << 5) | bs));
    img_set(&hd_img, 0x1A, (u16)((be << 5) | bs));
    u16 hi, lo;
    fbb_values(xfb, &hi, &lo);
    img_set(&hd_img, 0x1C, hi);
    img_set(&hd_img, 0x1E, lo);
    img_set(&hd_img, 0x24, hi);
    img_set(&hd_img, 0x26, lo);
    img_set(&hd_img, 0x30, (u16)(0x1000 | (HD_VN / 2 + 1))); // retrace interrupt once per field
    img_set(&hd_img, 0x32, 1);
    img_set(&hd_img, 0x34, 0x1001);
    img_set(&hd_img, 0x36, 0x0001);
    img_set(&hd_img, 0x48, (u16)((wpl << 8) | wpl)); // fetch width and stride, in 16-pixel units
    img_set(&hd_img, 0x4A, 0x0100);                  // horizontal scaler off
    hd_img.dcr = (safe_img.dcr & 0x0300) | 0x0001 | 0x0004; // keep FMT; enable, non-interlaced
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
    if (hd->crop)
        return boot1c4; // clock untouched
    return (boot1c4 & ~0x0000FF00u) | ((u32)(hd->nfb - 1) << 8); // byte 2 is never touched
}

static u32 hd_pll_1c8(void) {
    if (hd->crop)
        return boot1c8;
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
    if (hd_active) {
        // The retrace interrupt comes at the end of the field's active picture, so the queued
        // buffer is read from the next field on and the one that was showing is free again
        if (pending_idx >= 0) {
            hd_set_fbb(framebuffer[pending_idx]);
            shown_idx = pending_idx;
            pending_idx = -1;
            new_frame = true;
        }
    } else if (pending_idx >= 0 && VIDEO_GetCurrentFramebuffer() == framebuffer[pending_idx]) {
        shown_idx = pending_idx;
        pending_idx = -1;
        new_frame = true;
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

const char *gfx_gx_wm_hd_name(void)
{
    return hd_active ? hd->name : NULL;
}

void gfx_gx_wm_get_tiles(int *tiles_x, int *tiles_y, int *tile_w, int *tile_h)
{
    // 1280x720p: 2x2 tiles of 640x360. 960x720p: 2x2 of 480x360. 1280x960 crop: 2x2 of 640x480.
    // 1280x240p: 2x1 of 640x240
    *tiles_x = 2;
    *tiles_y = hd_active ? 2 : 1;
    *tile_w = hd_active ? hd->w / 2 : HIRES_TILE_WIDTH;
    *tile_h = hd_active ? hd->tile_h : (rmode != NULL ? rmode->efbHeight : 240);
}

int gfx_gx_wm_get_frame_height(void)
{
    if (rmode == NULL)
        return DESIRED_SCREEN_HEIGHT;
    return hd_active ? hd->h : rmode->efbHeight;
}

void *gfx_gx_wm_get_back_buffer(void)
{
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

// Pick the video mode to render in
// Options are 720p, 1280x240p, 240p or the console's own 480p/480i (576i for PAL)
static GXRModeObj *gfx_gx_wm_select_mode(void)
{
#ifdef __wii__
    if (configVideoMode == VIDEO_MODE_720P || configVideoMode == VIDEO_MODE_960X720P
        || configVideoMode == VIDEO_MODE_960P_CROP) {
        hd = configVideoMode == VIDEO_MODE_960X720P ? &hd_960
           : configVideoMode == VIDEO_MODE_960P_CROP ? &hd_crop : &hd_1280;
        TVHd720.fbWidth = hd->w;
        TVHd720.efbHeight = hd->tile_h;
        TVHd720.xfbHeight = hd->h;
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

static void clear_xfb(void *xfb, u32 bytes)
{
    u32 *p = (u32 *)xfb; // uncached
    for (u32 i = 0; i < bytes / 4; i++)
        p[i] = 0x10801080; // YUV black
}

static void gfx_gx_wm_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen)
{
    dbglog("wm_init: VIDEO_Init");
    VIDEO_Init();
    VIDEO_SetBlack(true);
    dbglog("wm_init: tvmode=%d component=%d", VIDEO_GetCurrentTvMode(), VIDEO_HaveComponentCable());
    dbg_vi_regs("boot");

    rmode = gfx_gx_wm_select_mode();
    dbglog("wm_init: mode viTVMode=%u fb=%ux%u efb=%u vi=%ux%u tiled=%d hd=%d", rmode->viTVMode,
           rmode->fbWidth, rmode->xfbHeight, rmode->efbHeight, rmode->viWidth, rmode->viHeight,
           tiled_active, hd_active);

    // Double buffering when presenting at vsync, triple when uncapped; 720p always uses the
    // retrace-driven queue, so always three there
    const u32 fb_bytes = VIDEO_GetFrameBufferSize(rmode);
    dbglog("wm_init: before XFBs: MEM1 arena %p-%p, MEM2 arena %p-%p", SYS_GetArenaLo(), SYS_GetArenaHi(),
           SYS_GetArena2Lo(), SYS_GetArena2Hi());
    framebuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    framebuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    if (configBenchUncapped || hd_active) {
        void *third = SYS_AllocateFramebuffer(rmode);
        if (third != NULL) {
            framebuffer[2] = MEM_K0_TO_K1(third);
            triple = true;
            uncapped = configBenchUncapped;
        }
    }
    dbglog("wm_init: fb0=%p fb1=%p fb2=%p (%u bytes each) triple=%d uncapped=%d", framebuffer[0],
           framebuffer[1], framebuffer[2], (unsigned)fb_bytes, triple, uncapped);
    dbglog("wm_init: after XFBs: MEM1 arena %p-%p, MEM2 arena %p-%p", SYS_GetArenaLo(), SYS_GetArenaHi(),
           SYS_GetArena2Lo(), SYS_GetArena2Hi());

    if (hd_active && (!triple || framebuffer[0] == MEM_K0_TO_K1(NULL) || framebuffer[1] == MEM_K0_TO_K1(NULL))) {
        // Not enough memory for 720p XFBs: fall back to the console's own mode
        dbglog("wm_init: not enough memory for 720p, falling back");
        hd_active = false;
        tiled_active = false;
        rmode = VIDEO_GetPreferredMode(NULL);
        triple = uncapped = false;
        framebuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
        framebuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
        if (configBenchUncapped) {
            void *third = SYS_AllocateFramebuffer(rmode);
            if (third != NULL) {
                framebuffer[2] = MEM_K0_TO_K1(third);
                triple = uncapped = true;
            }
        }
    }

    for (int i = 0; i < 3; i++)
        if (framebuffer[i] != MEM_K0_TO_K1(NULL) && framebuffer[i] != NULL)
            clear_xfb(framebuffer[i], VIDEO_GetFrameBufferSize(rmode));

    GXRModeObj *vi_mode = rmode;
    if (hd_active) {
        // libogc runs the safe mode underneath; the 720p registers go on top of it below
        safe_mode = VIDEO_GetPreferredMode(NULL);
        vi_mode = safe_mode;
    }

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

    if (hd_active) {
        VIDEO_WaitVSync();
        img_read(&safe_img);
        safe_img.dcr |= 0x0001; // the safe image re-enables the VI when restored
        boot1c4 = HWREG(0x1C4);
        boot1c8 = HWREG(0x1C8);
        hd_build_image(framebuffer[0]);
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

    if (tiled_active && !hd_active) {
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
    if (hd_active && !hd->crop)
        return (double)hd->p_hz / ((double)hd->ht * (HD_VN / 2)); // exactly 60
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
    while (pending_idx >= 0 && gettime() < to) {
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
    static unsigned int frames;
    frames++;
    heartbeat();
    if (frames <= 10 || frames % 300 == 0)
        dbglog("swap: frame %u retraces %u beam V=%u H=%u draw=%d shown=%d pending=%d DCR=%04X FBB=%04X%04X",
               frames, (unsigned)retrace_count, VI[0x2C / 2] & 0x7FF, VI[0x2E / 2] & 0x7FF,
               draw_idx, shown_idx, pending_idx, VI[0x02 / 2], VI[0x1C / 2], VI[0x1E / 2]);
    if (frames == 120 && tiled_active)
        dbg_vi_regs("frame120");

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

        // Presenting at the field rate (only reached in 720p): wait for this frame to go up,
        // and hold it for a second field at 30 fps
        wait_pending_shown();
        if (!config60Fps)
            wait_vsync_ms(100, "30 fps hold");
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
