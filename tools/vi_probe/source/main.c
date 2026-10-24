// vi_probe: read-only probe of the Wii's VI timing and Hollywood clock (PLL) registers.
// Switches through known video modes, measures the real VI timing against the CPU timebase
// (which runs from a separate clock), and logs everything to vi_probe.txt on SD and USB.
// It writes no PLL or clock registers.
#include <gccore.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#define VI ((volatile u16 *)0xCC002000)
#define HW(off) (*(volatile u32 *)(0xCD800000 + (off)))  // Hollywood regs, full-access mirror
#define HWS(off) (*(volatile u32 *)(0xCD000000 + (off))) // PPC-subset mirror

static void *xfb;
static volatile u32 retraces;
static void on_retrace(u32 cnt) { (void)cnt; retraces++; }
#define VIDEO_GetRetraceCount() (retraces)
static GXRModeObj *cur;

static void logf_(const char *fmt, ...)
{
    static const char *const paths[] = { "sd:/vi_probe.txt", "usb:/vi_probe.txt" };
    static bool first = true;
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], first ? "w" : "a");
        if (f) {
            fputs(line, f);
            fputc('\n', f);
            fclose(f);
        }
    }
    first = false;
}

static void dump_vi(void)
{
    for (int r = 0; r < 0x78; r += 16)
        logf_("  VI %02X: %04X %04X %04X %04X %04X %04X %04X %04X", r,
              VI[r / 2], VI[r / 2 + 1], VI[r / 2 + 2], VI[r / 2 + 3],
              VI[r / 2 + 4], VI[r / 2 + 5], VI[r / 2 + 6], VI[r / 2 + 7]);
}

static void dump_hw(void)
{
    logf_("  HW_AHBPROT(064)=%08X HW_VI1CFG(018)=%08X", HW(0x064), HW(0x018));
    for (u32 off = 0x190; off < 0x200; off += 16)
        logf_("  HW %03X: %08X %08X %08X %08X", off, HW(off), HW(off + 4), HW(off + 8), HW(off + 12));
    logf_("  (subset mirror) PLLVI=%08X PLLVIEXT=%08X", HWS(0x1C4), HWS(0x1C8));
}

static u32 cur_line(void)
{
    return VI[0x2C / 2] & 0x7FF; // vertical beam position
}

// Measures the field (retrace) rate and the time for a fixed number of lines, using the
// CPU timebase, which is clocked independently of the VI.
static void measure(void)
{
    // Field rate over ~2 seconds
    u32 r0 = VIDEO_GetRetraceCount();
    while (VIDEO_GetRetraceCount() == r0) {}
    u32 r1 = VIDEO_GetRetraceCount();
    u64 t1 = gettime();
    while (VIDEO_GetRetraceCount() - r1 < 120) {}
    u64 t2 = gettime();
    u64 us = ticks_to_microsecs(t2 - t1);
    u32 mhz_milli = (u32)(120ull * 1000000000ull / us); // fields per 1000 s

    // Line period: time for the beam to go from line 40 to line 200, best of several
    u64 best = ~0ull;
    u32 maxline = 0;
    for (int k = 0; k < 8; k++) {
        u64 a, b;
        u32 l;
        u64 timeout = gettime() + millisecs_to_ticks(200);
        while ((l = cur_line()) != 40) { if (l > maxline) maxline = l; if (gettime() > timeout) break; }
        a = gettime();
        timeout = a + millisecs_to_ticks(200);
        while ((l = cur_line()) != 200) { if (l > maxline) maxline = l; if (gettime() > timeout) break; }
        b = gettime();
        if (b - a < best) best = b - a;
    }
    u64 ns160 = ticks_to_nanosecs(best);
    u32 line_ns = (u32)(ns160 / 160);
    u32 hlw = VI[0x06 / 2] & 0x3FF;
    // VI counter clock implied by the line period: (2 * hlw) counts per line
    u32 clk_khz = line_ns ? (u32)(2ull * hlw * 1000000ull / line_ns) : 0;

    logf_("  field rate = %u.%03u Hz over 120 fields (%llu us)", mhz_milli / 1000, mhz_milli % 1000, us);
    logf_("  line period = %u ns (160 lines in %llu ns), max line seen %u", line_ns, ns160, maxline);
    logf_("  hlw=%u -> VI counter clock ~ %u kHz (x2 = %u kHz)", hlw, clk_khz, clk_khz * 2);
}

static void set_mode(GXRModeObj *m, const char *name)
{
    logf_("=== mode %s (viTVMode=%u fb=%ux%u)", name, m->viTVMode, m->fbWidth, m->xfbHeight);
    cur = m;
    VIDEO_Configure(m);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    for (int i = 0; i < 30; i++)
        VIDEO_WaitVSync();
}

// Same 1280x240p register pokes as the SM64 build
static void apply_hires(void)
{
    u32 level;
    _CPU_ISR_Disable(level);
    VI[0x6C / 2] = 0x0001;
    VI[0x04 / 2] = 0x4769;
    VI[0x06 / 2] = 0x035A;
    VI[0x08 / 2] = 0x059A;
    VI[0x0A / 2] = 0x93FF;
    VI[0x4A / 2] = 0x0100;
    VI[0x48 / 2] = 0x5050;
    VI[0x02 / 2] = 0x0005;
    _CPU_ISR_Restore(level);
    for (int i = 0; i < 30; i++)
        VIDEO_WaitVSync();
}

static GXRModeObj hires = {
    VI_TVMODE_NTSC_DS, 1280, 240, 240, (VI_MAX_WIDTH_NTSC - 640) / 2, (VI_MAX_HEIGHT_NTSC / 2 - 480 / 2) / 2,
    640, 480, VI_XFBMODE_SF, GX_FALSE, GX_FALSE,
    {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}},
    {0, 0, 21, 22, 21, 0, 0}
};

int main(void)
{
    VIDEO_Init();
    VIDEO_SetPostRetraceCallback(on_retrace);
    fatInitDefault();

    logf_("vi_probe (read-only) build " __DATE__ " " __TIME__);
    logf_("component cable=%d tvmode=%d", VIDEO_HaveComponentCable(), VIDEO_GetCurrentTvMode());
    logf_("=== boot state (before any mode change)");
    dump_vi();
    dump_hw();

    // One buffer big enough for every mode, filled with mid grey (YUV 0x80 0x80)
    xfb = MEM_K0_TO_K1(memalign(32, 1280 * 576 * 2));
    for (u32 i = 0; i < 1280 * 576 / 2; i++)
        ((u32 *)xfb)[i] = 0x80808080;

    set_mode(&TVNtsc480IntDf, "480i (27 MHz)");
    dump_vi(); dump_hw(); measure();

    if (VIDEO_HaveComponentCable()) {
        set_mode(&TVNtsc480Prog, "480p (54 MHz)");
        dump_vi(); dump_hw(); measure();
    } else {
        logf_("(no component cable: skipping 480p)");
    }

    set_mode(&TVNtsc240Ds, "240p (27 MHz)");
    dump_vi(); dump_hw(); measure();

    set_mode(&hires, "1280x240p base");
    apply_hires();
    logf_("=== 1280x240p (54 MHz, SM64 timing)");
    dump_vi(); dump_hw(); measure();

    set_mode(VIDEO_GetPreferredMode(NULL), "preferred (restore)");
    dump_vi(); dump_hw(); measure();

    logf_("DONE");
    return 0; // back to the Homebrew Channel
}
