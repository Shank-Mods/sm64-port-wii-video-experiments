#ifdef TARGET_GX

#include "macros.h" // for UNUSED

#include "gfx_gx_wm.h"
#include <ogc/machine/processor.h> // _CPU_ISR_Disable/Restore
#include "gfx_screen_config.h"
#include "../configfile.h" // for config60Fps, config240p, configAntialias, configHires1280

static GXRModeObj *rmode;
static void *framebuffer[2];
static bool fb;
static bool hires_active;

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
    return hires_active;
}

void *gfx_gx_wm_get_back_buffer(void)
{
    return framebuffer[fb ^ 1];
}

// Horizontal timing for 1280 active pixels at 54 MHz (858 clocks per half line), taken from a
// register dump of the mode working on hardware.
static void gfx_gx_wm_apply_hires_timing(void)
{
    volatile u16 *vi = (volatile u16 *)0xCC002000;
    u32 level;

    _CPU_ISR_Disable(level);
    vi[0x6C / 2] = 0x0001; // VI clock: 54 MHz
    vi[0x04 / 2] = 0x4769; // HTR0: hcs/hce
    vi[0x06 / 2] = 0x035A; // HTR0: half line width = 858
    vi[0x08 / 2] = 0x059A; // HTR1: hbs/hbe/hsy
    vi[0x0A / 2] = 0x93FF;
    vi[0x4A / 2] = 0x0100; // horizontal scaler off
    vi[0x48 / 2] = 0x5050; // picture config: 1280 px stride
    vi[0x02 / 2] = 0x0005; // DCR: enabled, non-interlaced, NTSC
    _CPU_ISR_Restore(level);
}

// Pick the video mode to render in
// Currently, options are 240p or 480p (or 576i for PAL)
static GXRModeObj *gfx_gx_wm_select_mode(void)
{
    if (configHires1280) {
        hires_active = true;
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

static void gfx_gx_wm_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen)
{
    VIDEO_Init();
    VIDEO_SetBlack(true);

    rmode = gfx_gx_wm_select_mode();

    // double-buffering
    framebuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    framebuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    VIDEO_Configure(rmode);

    VIDEO_SetNextFramebuffer(framebuffer[fb]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    if (hires_active) {
        gfx_gx_wm_apply_hires_timing();
        VIDEO_WaitVSync();
    }
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
    if (config240p || hires_active) {
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

static void gfx_gx_wm_swap_buffers_begin(void)
{
    fb ^= 1; // flip frame buffer

    // In 1280x240p the renderer has already copied both tiles into this buffer
    if (!hires_active) {
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
