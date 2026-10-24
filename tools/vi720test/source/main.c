// vi720test: attempt a 720p-rate VI raster at the confirmed 74.25 MHz clock, draw a bold
// test pattern, measure what the VI actually produces, and log it. EXPERIMENTAL.
//
// Confirmed so far (vi_pll): F = 54 MHz * Nfb / V, Nfb = 0x1C4[15:8]+1, V = 0x1C8[23:20].
// 74.25 MHz = Nfb 11, V 8  ->  0x1C4 = 0x00200AC0, 0x1C8 = 0xD880700E.
//
// Grounded: the horizontal line rate. line_ns = 4 * HLW * 1e6 / F_kHz (anchored to measured
//   1280x240p data: HLW 858, F 54000 -> 63553 ns). For ~45 kHz (22222 ns) at 74250 kHz:
//   HLW = 22222 * 74250 / 4e6 ~= 413.
// HYPOTHESES (to be corrected from the measured results): the HTR0 hsync position, the HTR1
//   blanking window, and all of the vertical timing (VTR/VTO/VTE). These are scaled from the
//   proven 1280x240p register set or best-effort; wrong values shift/roll/crop the image but
//   do not hang, and everything resets on reboot.
//
// Flow: MODE A = proven 1280x240p @ 54 MHz with the pattern (guaranteed-visible control),
//       MODE B = 720p attempt @ 74.25 MHz. Each is held on screen (HOME/Start = next/exit),
//       measured, and logged to vi720.txt on USB (and SD). PLL/VI restored at the end.

#include <gccore.h>
#include <fat.h>
#include <stdbool.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#define VI  ((volatile u16 *)0xCC002000)
#define HW(off)  (*(volatile u32 *)(0xCD800000 + (off)))

#define W 1280          // framebuffer width (pixels)
#define H 720           // framebuffer height (lines)
#define WORDS_PER_ROW (W/2)   // YUYV: 2 pixels per 32-bit word

// 720p target register values (see header). byte2 of 0x1C4 stays 0x20 (0x21 hard-hangs).
#define PLL_1C4_7425 0x00200AC0u
#define PLL_1C8_7425 0xD880700Eu
#define HLW_240 858
#define HLW_720 413

static u32 *xfb;        // uncached YUYV framebuffer, W*H
static volatile u32 retraces;
static void on_retrace(u32 c) { (void)c; retraces++; }

// ---------------- logging (USB primary, SD mirror, committed per line) ----------------
static void logf_(const char *fmt, ...)
{
    static const char *const paths[] = { "usb:/vi720.txt", "sd:/vi720.txt" };
    static bool first = true;
    char line[320];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], first ? "w" : "a");
        if (f) { fputs(line, f); fputc('\n', f); fflush(f); fclose(f); }
    }
    first = false;
    printf("%s\n", line);
}

static void tb_delay_ms(u32 ms) { u64 to = gettime() + millisecs_to_ticks(ms); while (gettime() < to) {} }
static u32 cur_line(void) { return VI[0x2C / 2] & 0x7FF; }

// ---------------- measurement (timebase-based, timeout-hardened) ----------------
typedef struct { bool field_ok; u32 field_milli; bool line_ok; u32 line_ns; u32 hlw;
                 u32 counter_khz; u32 eff_khz; u32 maxline; } Meas;

static bool wait_cross(u32 target, u64 to_ticks, u32 *ml)
{
    u64 to = gettime() + to_ticks; u32 l;
    while ((l = cur_line()) >= target) { if (l > *ml) *ml = l; if (gettime() > to) return false; }
    while ((l = cur_line()) <  target) { if (l > *ml) *ml = l; if (gettime() > to) return false; }
    return true;
}

static Meas measure(void)
{
    Meas m; memset(&m, 0, sizeof(m));
    m.hlw = VI[0x06 / 2] & 0x3FF;

    u32 r0 = retraces; u64 fto = gettime() + millisecs_to_ticks(500);
    while (retraces == r0 && gettime() < fto) {}
    if (retraces != r0) {
        u32 r1 = retraces; u64 t1 = gettime(); u64 win = gettime() + millisecs_to_ticks(3000);
        while ((retraces - r1) < 120 && gettime() < win) {}
        u64 t2 = gettime(); u32 got = retraces - r1;
        if (got >= 2) { u64 us = ticks_to_microsecs(t2 - t1);
            if (us) { m.field_ok = true; m.field_milli = (u32)((u64)got * 1000000000ull / us); } }
    }

    u64 best = ~0ull;
    for (int k = 0; k < 8; k++) {
        u32 ml = m.maxline; u64 sto = gettime() + millisecs_to_ticks(50); u32 l;
        while ((l = cur_line()) > 10) { if (l > ml) ml = l; if (gettime() > sto) break; }
        bool a = wait_cross(40, millisecs_to_ticks(50), &ml); u64 ta = gettime();
        bool b = wait_cross(200, millisecs_to_ticks(50), &ml); u64 tb = gettime();
        m.maxline = ml;
        if (a && b && tb > ta && (tb - ta) < best) best = tb - ta;
    }
    if (best != ~0ull) {
        u64 ns160 = ticks_to_nanosecs(best); m.line_ns = (u32)(ns160 / 160);
        if (m.line_ns) { m.line_ok = true;
            m.counter_khz = (u32)(2ull * m.hlw * 1000000ull / m.line_ns); m.eff_khz = m.counter_khz * 2; }
    }
    return m;
}

static void log_meas(const char *tag, Meas *m)
{
    if (m->field_ok) logf_("    [%s] field=%u.%03u Hz", tag, m->field_milli/1000, m->field_milli%1000);
    else             logf_("    [%s] field=NO RETRACE", tag);
    if (m->line_ok) {
        u32 line_khz = m->line_ns ? (u32)(1000000000ull / m->line_ns) : 0; // lines per 1000 s -> kHz*...
        logf_("    [%s] line=%u ns (~%u.%02u kHz)  hlw=%u  eff pixel clock ~ %u kHz  maxline=%u",
              tag, m->line_ns, line_khz/1000, (line_khz%1000)/10, m->hlw, m->eff_khz, m->maxline);
    } else {
        logf_("    [%s] line=NO LOCK (beam not sweeping 40..200), maxline=%u", tag, m->maxline);
    }
}

// ---------------- test pattern ----------------
static inline void putpair(u32 x, u32 y, u8 Y, u8 Cb, u8 Cr)
{
    if (x + 1 >= W || y >= H) return;
    xfb[y * WORDS_PER_ROW + (x >> 1)] = ((u32)Y << 24) | ((u32)Cb << 16) | ((u32)Y << 8) | Cr;
}
static void fill(u32 x0, u32 y0, u32 x1, u32 y1, u8 Y, u8 Cb, u8 Cr)
{
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    for (u32 y = y0; y < y1; y++)
        for (u32 x = x0 & ~1u; x < x1; x += 2) putpair(x, y, Y, Cb, Cr);
}

static void draw_pattern(void)
{
    // 8 SMPTE-ish 75% colour bars (Y,Cb,Cr)
    static const u8 bar[8][3] = {
        {200,128,128},{190, 16,148},{170,166, 16},{145, 54, 34},
        {106,202,222},{ 81, 90,240},{ 51,240,110},{ 16,128,128} };
    u32 bw = W / 8;
    for (int i = 0; i < 8; i++) fill(i*bw, 0, (i+1)*bw, H, bar[i][0], bar[i][1], bar[i][2]);

    // white grid every 80 px / 80 lines (2px thick)
    for (u32 x = 0; x < W; x += 80) fill(x, 0, x+2, H, 235,128,128);
    for (u32 y = 0; y < H; y += 80) fill(0, y, W, y+2, 235,128,128);

    // bright 4px border
    fill(0,0,W,4,235,128,128); fill(0,H-4,W,H,235,128,128);
    fill(0,0,4,H,235,128,128); fill(W-4,0,W,H,235,128,128);

    // corner markers 56x56: TL red, TR green, BL blue, BR white (orientation / cropping)
    fill(0,0,56,56, 81,90,240); fill(W-56,0,W,56, 51,240,110);
    fill(0,H-56,56,H, 51,240,110); fill(W-56,H-56,W,H, 235,128,128);
    fill(0,0,56,56, 81,90,240);

    // centre crosshair
    fill(W/2-1,0,W/2+1,H,235,128,128); fill(0,H/2-1,W,H/2+1,235,128,128);
}

// ---------------- register helpers / snapshot ----------------
static const u32 CAND[] = { 0x1A0, 0x1C4, 0x1C8 };
#define NCAND (int)(sizeof(CAND)/sizeof(CAND[0]))
static u32 snap[NCAND];
static void snapshot(void){ for (int i=0;i<NCAND;i++) snap[i]=HW(CAND[i]); }
static void restore_pll(void){ u32 l; _CPU_ISR_Disable(l); for(int i=0;i<NCAND;i++) HW(CAND[i])=snap[i]; _CPU_ISR_Restore(l); }

// Recompose HTR0/HTR1 from the proven 1280x240p values, scaling the hsync/blanking fields
// by (HLW_720/HLW_240) so the sync/blanking stay proportional to the shorter line.
// 240p: HTR0=0x4769035A (HLW=858,HCE=0x69,HCS=0x47), HTR1=0x059A93FF (HBS,HBE,HSY).
static u32 scale_field(u32 v, u32 num, u32 den){ return (v * num) / den; }

// ---------------- VI mode application ----------------
static GXRModeObj mode240 = {
    VI_TVMODE_NTSC_DS, 1280, 240, 240, (VI_MAX_WIDTH_NTSC-640)/2, (VI_MAX_HEIGHT_NTSC/2-480/2)/2,
    640, 480, VI_XFBMODE_SF, GX_FALSE, GX_FALSE,
    {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}},
    {0,0,21,22,21,0,0}
};

static void vi_present(GXRModeObj *m)
{
    VIDEO_Configure(m);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    tb_delay_ms(60);
}

// MODE A: proven 1280x240p @ 54 MHz (clock untouched = stock 54).
static void apply_240p(void)
{
    u32 l; _CPU_ISR_Disable(l);
    VI[0x6C/2]=0x0001;
    VI[0x04/2]=0x4769; VI[0x06/2]=0x035A;
    VI[0x08/2]=0x059A; VI[0x0A/2]=0x93FF;
    VI[0x4A/2]=0x0100; VI[0x48/2]=0x5050;
    VI[0x02/2]=0x0005;
    _CPU_ISR_Restore(l);
    tb_delay_ms(200);
}

// MODE B: 720p attempt @ 74.25 MHz. Every written value is logged write-ahead.
static void apply_720p(void)
{
    // Horizontal, grounded: HLW for ~45 kHz. hsync/blanking scaled from the 240p set.
    u32 htr0_hi = (scale_field(0x47, HLW_720, HLW_240) << 8) | scale_field(0x69, HLW_720, HLW_240);
    u32 htr1_lo = 0x059A, htr1_hi = 0x93FF; // placeholder; recomposed below
    // Decode 240p HTR1 = 0x059A93FF -> HBS(17-26),HBE(7-16),HSY(0-6); scale; recompose.
    u32 h1 = 0x059A93FF;
    u32 hsy = h1 & 0x7F, hbe = (h1 >> 7) & 0x3FF, hbs = (h1 >> 17) & 0x3FF;
    hsy = scale_field(hsy, HLW_720, HLW_240);
    hbe = scale_field(hbe, HLW_720, HLW_240);
    hbs = scale_field(hbs, HLW_720, HLW_240);
    u32 htr1 = (hbs << 17) | (hbe << 7) | hsy;
    htr1_lo = htr1 >> 16; htr1_hi = htr1 & 0xFFFF;

    // Vertical, HYPOTHESIS: 720 active, ~30 lines blanking (to be corrected from maxline).
    u32 vtr = (720u << 4) | 0x0C;      // ACV=720, EQU=12
    u32 vto_lo = 0x0024, vto_hi = 0x0018;  // PSB=0x24, PRB=0x18 (half-lines) -- guess
    u32 vte_lo = 0x0024, vte_hi = 0x0018;

    logf_("  MODE B writes (74.25 MHz, HLW=%u):", HLW_720);
    logf_("    0x6C VICLK = 0001");
    logf_("    0x1C4 = %08X  0x1C8 = %08X   (PLL 74.25 MHz)", PLL_1C4_7425, PLL_1C8_7425);
    logf_("    0x04/06 HTR0 = %04X %04X   (HLW=%u, hsync scaled)", (unsigned)htr0_hi, (unsigned)HLW_720, HLW_720);
    logf_("    0x08/0A HTR1 = %04X %04X   (HSY=%u HBE=%u HBS=%u)", (unsigned)htr1_lo,(unsigned)htr1_hi,(unsigned)hsy,(unsigned)hbe,(unsigned)hbs);
    logf_("    0x02 DCR = 0005 (progressive,NTSC)   0x48 HSW=5050 0x4A HSR=0100");
    logf_("    0x00 VTR = %04X (ACV=720)   0x0C/0E VTO = %04X %04X   0x10/12 VTE = %04X %04X",
          (unsigned)vtr,(unsigned)vto_lo,(unsigned)vto_hi,(unsigned)vte_lo,(unsigned)vte_hi);
    logf_("  >>> poking now <<<");

    u32 l; _CPU_ISR_Disable(l);
    VI[0x6C/2] = 0x0001;
    HW(0x1C4)  = PLL_1C4_7425;
    HW(0x1C8)  = PLL_1C8_7425;
    VI[0x04/2] = (u16)htr0_hi;  VI[0x06/2] = (u16)HLW_720;
    VI[0x08/2] = (u16)htr1_lo;  VI[0x0A/2] = (u16)htr1_hi;
    VI[0x02/2] = 0x0005;
    VI[0x48/2] = 0x5050;        VI[0x4A/2] = 0x0100;
    VI[0x00/2] = (u16)vtr;
    VI[0x0C/2] = (u16)vto_lo;   VI[0x0E/2] = (u16)vto_hi;
    VI[0x10/2] = (u16)vte_lo;   VI[0x12/2] = (u16)vte_hi;
    _CPU_ISR_Restore(l);
    tb_delay_ms(300);

    logf_("  readback 0x1C4=%08X 0x1C8=%08X VTR=%04X HTR0=%04X%04X",
          (unsigned)HW(0x1C4),(unsigned)HW(0x1C8),(unsigned)VI[0x00/2],(unsigned)VI[0x04/2],(unsigned)VI[0x06/2]);
}

// hold the current mode on screen; HOME (wiimote) or Start (GC) ends early
static void hold(u32 ms)
{
    u64 to = gettime() + millisecs_to_ticks(ms);
    while (gettime() < to) {
        WPAD_ScanPads(); PAD_ScanPads();
        if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || (PAD_ButtonsDown(0) & PAD_BUTTON_START)) break;
        tb_delay_ms(30);
    }
}

int main(void)
{
    VIDEO_Init(); WPAD_Init(); PAD_Init();
    GXRModeObj *pref = VIDEO_GetPreferredMode(NULL);

    xfb = (u32 *)MEM_K0_TO_K1(memalign(32, W * H * 2));
    for (u32 i = 0; i < (u32)(W * H / 2); i++) xfb[i] = 0x10801080; // black (Y=16)
    draw_pattern();

    VIDEO_Configure(pref);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetPostRetraceCallback(on_retrace);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    CON_Init(xfb, 20, 20, 40, 40, W * 2); // tiny console corner; pattern dominates

    bool fat = fatInitDefault();
    logf_("vi720test build " __DATE__ " " __TIME__ "  fat=%d component=%d", fat, VIDEO_HaveComponentCable());
    snapshot();
    logf_("boot 0x1C4=%08X 0x1C8=%08X 0x1A0=%08X", (unsigned)HW(0x1C4),(unsigned)HW(0x1C8),(unsigned)HW(0x1A0));

    // ---- MODE A: proven 1280x240p @ 54 MHz (guaranteed-visible control) ----
    logf_("");
    logf_("=== MODE A: 1280x240p @ 54 MHz (proven) -- pattern should be visible ===");
    vi_present(&mode240);
    apply_240p();
    Meas a = measure(); log_meas("240p", &a);
    logf_("   holding ~20 s (HOME/Start to continue)...");
    hold(20000);

    // ---- MODE B: 720p attempt @ 74.25 MHz ----
    logf_("");
    logf_("=== MODE B: 720p ATTEMPT @ 74.25 MHz (experimental) ===");
    logf_("   expect line ~45 kHz; watch total lines (maxline) and field rate.");
    vi_present(&mode240);          // reuse 240p plumbing, then override timing
    apply_720p();
    Meas b = measure(); log_meas("720p", &b);
    logf_("   interpretation: 720p60 wants ~45.0 kHz line, ~60 Hz field, maxline ~1500 (half-lines).");
    logf_("   holding ~40 s (HOME/Start to continue)... look at the monitor now.");
    hold(40000);

    // ---- restore ----
    restore_pll();
    tb_delay_ms(200);
    vi_present(pref);
    logf_("");
    logf_("restored. final 0x1C4=%08X 0x1C8=%08X", (unsigned)HW(0x1C4),(unsigned)HW(0x1C8));
    logf_("DONE");
    return 0;
}
