#if defined(TARGET_GX) && !defined(EARLYCON)

#include <stdbool.h>
#include <gccore.h>
#include "early_console.h"

bool gEarlyVideoUp;

void early_console_init(void) {
    VIDEO_Init();
    gEarlyVideoUp = true;
}

#endif

#if defined(TARGET_GX) && defined(EARLYCON)

// Diagnostic builds (-DEARLYCON): a text console on screen from the first instruction of main,
// showing every dbglog line until the game's own video takes over (held for a few seconds
// first). If nothing at all appears, the program never started (loader problem); otherwise the
// last line on screen is how far startup got, even when no SD card could be written.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "early_console.h"

static bool active;
static void *con_xfb;
bool gEarlyVideoUp;

extern u8 _end[];

void early_console_init(void) {
    VIDEO_Init();
    gEarlyVideoUp = true;
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    con_xfb = MEM_K0_TO_K1(memalign(32, VIDEO_GetFrameBufferSize(rmode)));
    if (con_xfb == MEM_K0_TO_K1(NULL))
        return;
    console_init(con_xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(con_xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    active = true;
    printf("\x1b[2;0H");
    printf("SM64 bench diagnostic build (%s %s)\n", __DATE__, __TIME__);
#ifdef __wii__
    printf("console: Wii\n");
#else
    printf("console: GameCube\n");
#endif
    printf("program ends at %p, arena %p-%p\n", (void *)_end, SYS_GetArenaLo(), SYS_GetArenaHi());
    printf("video: %ux%u %s, component cable %d\n", rmode->fbWidth, rmode->xfbHeight,
           (rmode->viTVMode & VI_NON_INTERLACE) ? "progressive" : "interlaced", VIDEO_HaveComponentCable());
}

void early_console_line(const char *line) {
    if (!active)
        return;
    fputs(line, stdout);
    fputs("\n", stdout);
    fflush(stdout);
}

// Called just before the game switches the display to its own mode
void early_console_handover(void) {
    if (!active)
        return;
    printf("\nstarting the game's video in 5 s...\n");
    fflush(stdout);
    u64 to = gettime() + secs_to_ticks(5);
    while (gettime() < to) {
    }
    active = false;
}

// After the game's framebuffer is on screen: the console's XFB is no longer scanned
void early_console_free(void) {
    if (con_xfb != NULL && con_xfb != MEM_K0_TO_K1(NULL)) {
        free(MEM_K1_TO_K0(con_xfb));
        con_xfb = NULL;
    }
}

#endif
