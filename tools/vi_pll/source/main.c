// vi_pll: WRITE-and-measure probe of the Wii's undocumented VI clock / PLL registers.
//
// Goal: find register writes that push the VI pixel clock above its stock 54 MHz, as a
// stepping stone to 720p/1080i-class pixel clocks (74.25 / 148.5 MHz). This tool does NOT
// assume the PLL field layout (it is undocumented). Instead it:
//   1. Snapshots the candidate registers.
//   2. Runs KNOWN-GOOD CONTROL experiments to prove the write+measure path detects a clock
//      change (rewrite with no change; toggle the known 0x1A0 bit31 divide-by-2).
//   3. EMPIRICALLY MAPS the fields of 0x1C4/0x1C8 by perturbing one nibble at a time and
//      measuring whether the clock moves, and in which direction.
//   4. Optionally runs explicit value tries from usb:/vi_pll.cfg (edit on PC, no rebuild).
//
// Safety model (destructive, undocumented hardware probing):
//   * WRITE-AHEAD LOG: each intended write is logged AND the log file is flushed+closed
//     BEFORE the register is poked. If a value hangs the console, the USB log shows which
//     experiment was in flight.
//   * BOUNDED HOLD + AUTO-RESTORE: every experiment restores the snapshot afterwards. All
//     clock registers also reset on power cycle, so a hang is recovered by rebooting.
//   * TIMEOUT-HARDENED MEASUREMENT: all measurement loops are bounded by the CPU timebase,
//     so a stopped VI (no retrace / stuck beam counter) is reported, never an infinite spin.
//
// Measurement does not need a working display: the field rate and line period are timed
// against the CPU timebase (independent of the VI). Implied VI counter clock = 2*hlw/line.
//
// Register access uses the full-access Hollywood mirror at 0xCD800000, same as vi_probe.
// Requires <ahb_access/> in meta.xml.

#include <gccore.h>
#include <fat.h>
#include <stdbool.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#define VI  ((volatile u16 *)0xCC002000)
#define HW(off)  (*(volatile u32 *)(0xCD800000 + (off)))  // Hollywood regs, full-access mirror
#define HWS(off) (*(volatile u32 *)(0xCD000000 + (off)))  // PPC-subset mirror (read 0 for 0x1C4/8)

#define FULL 0xFFFFFFFFu

// -------- reference pixel clocks for the log (kHz) --------
#define CLK_54   54000u
#define CLK_7425 74250u
#define CLK_148  148500u

static void *xfb;
static GXRModeObj *rmode;
static volatile u32 retraces;
static void on_retrace(u32 cnt) { (void)cnt; retraces++; }

// ----------------------------------------------------------------------------
// Logging: write to USB (primary) and SD (if present). Each line is fopen/append/
// fclose'd so it is committed to the FAT before the next step. Also echoed to the
// on-screen console. first=true truncates both files on the first line.
// ----------------------------------------------------------------------------
static void logf_(const char *fmt, ...)
{
    static const char *const paths[] = { "usb:/vi_pll.txt", "sd:/vi_pll.txt" };
    static bool first = true;
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], first ? "w" : "a");
        if (f) {
            fputs(line, f);
            fputc('\n', f);
            fflush(f);
            fclose(f);              // commit to FAT: write-ahead guarantee
        }
    }
    first = false;
    printf("%s\n", line);          // on-screen echo (visible while video is still synced)
}

// ----------------------------------------------------------------------------
// Timebase delay, independent of the VI (safe to use when retrace has stopped).
// ----------------------------------------------------------------------------
static void tb_delay_ms(u32 ms)
{
    u64 to = gettime() + millisecs_to_ticks(ms);
    while (gettime() < to) {}
}

static u32 cur_line(void) { return VI[0x2C / 2] & 0x7FF; } // VI vertical beam position

// ----------------------------------------------------------------------------
// Measurement result.
// ----------------------------------------------------------------------------
typedef struct {
    bool  field_ok;
    u32   field_milli;   // field rate in milli-Hz (e.g. 59940 = 59.940 Hz)
    bool  line_ok;
    u32   line_ns;       // nanoseconds per line
    u32   hlw;           // HTR0 half-line width (VI[0x06] & 0x3FF)
    u32   counter_khz;   // implied VI counter clock = 2*hlw/line  (the "27 MHz" number)
    u32   eff_khz;       // counter_khz*2 = effective pixel clock estimate ("54 MHz" number)
    u32   maxline;       // highest beam line seen (sanity)
} Meas;

// Wait, with timeout, until cur_line() first reaches >= target having been < target.
// Returns false on timeout. Uses crossing detection so it cannot miss an exact sample
// at higher clocks.
static bool wait_cross(u32 target, u64 timeout_ticks, u32 *maxline)
{
    u64 to = gettime() + timeout_ticks;
    // ensure we start below target
    u32 l;
    while ((l = cur_line()) >= target) {
        if (l > *maxline) *maxline = l;
        if (gettime() > to) return false;
    }
    while ((l = cur_line()) < target) {
        if (l > *maxline) *maxline = l;
        if (gettime() > to) return false;
    }
    return true;
}

static Meas measure(void)
{
    Meas m;
    memset(&m, 0, sizeof(m));
    m.hlw = VI[0x06 / 2] & 0x3FF;

    // --- field rate: count retraces for up to ~3 s, bounded ---
    u32 r0 = retraces;
    u64 fto = gettime() + millisecs_to_ticks(500);
    while (retraces == r0 && gettime() < fto) {}
    if (retraces != r0) {
        u32 r1 = retraces;
        u64 t1 = gettime();
        u64 win = gettime() + millisecs_to_ticks(3000);
        while ((retraces - r1) < 120 && gettime() < win) {}
        u64 t2 = gettime();
        u32 got = retraces - r1;
        if (got >= 2) {
            u64 us = ticks_to_microsecs(t2 - t1);
            if (us) { m.field_ok = true; m.field_milli = (u32)((u64)got * 1000000000ull / us); }
        }
    }

    // --- line period: best (shortest) of 8 line-40 -> line-200 crossings ---
    u64 best = ~0ull;
    for (int k = 0; k < 8; k++) {
        u32 ml = m.maxline;
        // sync to near top of a field so 40 and 200 fall in order within one field
        u64 sto = gettime() + millisecs_to_ticks(50);
        u32 l;
        while ((l = cur_line()) > 10) { if (l > ml) ml = l; if (gettime() > sto) break; }
        u64 tmo = millisecs_to_ticks(50);
        bool okA = wait_cross(40, tmo, &ml);
        u64 a = gettime();
        bool okB = wait_cross(200, tmo, &ml);
        u64 b = gettime();
        m.maxline = ml;
        if (okA && okB && b > a && (b - a) < best) best = b - a;
    }
    if (best != ~0ull) {
        u64 ns160 = ticks_to_nanosecs(best);
        m.line_ns = (u32)(ns160 / 160);
        if (m.line_ns) {
            m.line_ok = true;
            m.counter_khz = (u32)(2ull * m.hlw * 1000000ull / m.line_ns);
            m.eff_khz = m.counter_khz * 2;
        }
    }
    return m;
}

// log a measurement with a tag, plus ratio vs a baseline (base_eff_khz, 0 = skip ratio)
static void log_meas(const char *tag, Meas *m, u32 base_eff_khz)
{
    if (m->field_ok)
        logf_("    [%s] field=%u.%03u Hz", tag, m->field_milli / 1000, m->field_milli % 1000);
    else
        logf_("    [%s] field=NO RETRACE (VI not generating fields)", tag);

    if (m->line_ok) {
        logf_("    [%s] line=%u ns  hlw=%u  counter~%u kHz  EFF PIXEL CLOCK ~ %u kHz  (maxline %u)",
              tag, m->line_ns, m->hlw, m->counter_khz, m->eff_khz, m->maxline);
        if (base_eff_khz && m->eff_khz) {
            u32 rx100 = (u32)((u64)m->eff_khz * 100ull / base_eff_khz);
            const char *verdict = "no change";
            if (m->eff_khz > base_eff_khz + 1500) verdict = ">>> CLOCK WENT UP <<<";
            else if (m->eff_khz + 1500 < base_eff_khz) verdict = "clock went DOWN";
            logf_("    [%s] ratio vs base = %u.%02ux   -> %s",
                  tag, rx100 / 100, rx100 % 100, verdict);
        }
    } else {
        logf_("    [%s] line=NO LOCK (beam counter not sweeping 40..200: clock stopped/unstable, maxline %u)",
              tag, m->maxline);
    }
}

// ----------------------------------------------------------------------------
// Candidate registers we touch. We snapshot and restore exactly these.
// ----------------------------------------------------------------------------
static const u32 CAND[] = { 0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x1C4, 0x1C8 };
#define NCAND (int)(sizeof(CAND)/sizeof(CAND[0]))
static u32 snap[NCAND];

static void snapshot(void)
{
    for (int i = 0; i < NCAND; i++) snap[i] = HW(CAND[i]);
}
static void restore_candidates(void)
{
    u32 lvl;
    _CPU_ISR_Disable(lvl);
    for (int i = 0; i < NCAND; i++) HW(CAND[i]) = snap[i];
    _CPU_ISR_Restore(lvl);
}

static void hw_write(u32 off, u32 val, u32 mask)
{
    if (mask == FULL) HW(off) = val;
    else { u32 c = HW(off); HW(off) = (c & ~mask) | (val & mask); }
}

// ----------------------------------------------------------------------------
// Experiment model.
// ----------------------------------------------------------------------------
typedef struct { u32 off, val, mask; } RegW;
typedef struct {
    char  name[96];
    bool  anchor_hires;   // set 1280x240p/54MHz fixed timing before writing
    RegW  w[8];
    int   nw;
    u32   hold_ms;        // extra hold after measure (0 = none)
    int   nfb;            // if >0, log the model-predicted clock for this feedback multiplier
    u32   predict_khz;    // if >0, log this expected clock (kHz) for the experiment
} Experiment;

static Experiment exps[96];
static int nexp = 0;

static Experiment *new_exp(const char *name, bool anchor)
{
    if (nexp >= (int)(sizeof(exps)/sizeof(exps[0]))) return &exps[0];
    Experiment *e = &exps[nexp++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->anchor_hires = anchor;
    return e;
}
static void exp_add_w(Experiment *e, u32 off, u32 val, u32 mask)
{
    if (e->nw < 8) { e->w[e->nw].off = off; e->w[e->nw].val = val; e->w[e->nw].mask = mask; e->nw++; }
}

// ----------------------------------------------------------------------------
// 1280x240p/54 MHz anchor (same register pokes as the SM64 build and vi_probe).
// ----------------------------------------------------------------------------
static GXRModeObj hires = {
    VI_TVMODE_NTSC_DS, 1280, 240, 240, (VI_MAX_WIDTH_NTSC - 640) / 2, (VI_MAX_HEIGHT_NTSC / 2 - 480 / 2) / 2,
    640, 480, VI_XFBMODE_SF, GX_FALSE, GX_FALSE,
    {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}},
    {0, 0, 21, 22, 21, 0, 0}
};

static void set_mode(GXRModeObj *m)
{
    rmode = m;
    VIDEO_Configure(m);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    for (int i = 0; i < 20; i++) VIDEO_WaitVSync();
}

static void apply_hires(void)
{
    u32 lvl;
    _CPU_ISR_Disable(lvl);
    VI[0x6C / 2] = 0x0001;
    VI[0x04 / 2] = 0x4769;
    VI[0x06 / 2] = 0x035A;
    VI[0x08 / 2] = 0x059A;
    VI[0x0A / 2] = 0x93FF;
    VI[0x4A / 2] = 0x0100;
    VI[0x48 / 2] = 0x5050;
    VI[0x02 / 2] = 0x0005;
    _CPU_ISR_Restore(lvl);
    // settle against the timebase, not VSync (VSync may stop if timing is off)
    tb_delay_ms(300);
}

// Compose a 0x1C4 value: byte3=0x00 (HW re-sets the lock bit itself), byte2 FORCED to
// 0x20 -- writing 0x21 there hard-hangs the console, so the default sweep never does -- ,
// byte1 = feedback multiplier field (Nfb-1), byte0 = output divider field.
static u32 mk1c4(u32 mbyte, u32 dbyte)
{
    return 0x00200000u | ((mbyte & 0xFF) << 8) | (dbyte & 0xFF);
}

// Compose a 0x1C8 value with a given V (output-divider nibble, bits 20-23), keeping the
// rest of the boot value. Boot 0x1C8 = 0xD870700E, V baseline = 7.
static u32 mk1c8(u32 v)
{
    return (0xD870700Eu & ~0x00F00000u) | ((v & 0xF) << 20);
}

// ----------------------------------------------------------------------------
// Build the default experiment list (v3 -- ROUND 3 TARGET CONFIRMATION).
//
// Clock law decoded in runs 1-2:  F = 54 MHz * Nfb / V
//   Nfb = 0x1C4 bits 8-15 (byte1) + 1   (baseline 7)
//   V   = 0x1C8 bits 20-23              (baseline 7)
// byte2 of 0x1C4 forced to 0x20 (0x21 hard-hangs); byte0 = 0xC0 (post-divide 1).
//
// This default now CONFIRMS the exact HD pixel clocks and verifies the V divider.
// Targets run first so a hang later cannot cost them. (A full field sweep is still
// available by dropping a vi_pll.cfg on the USB root; that overrides this list.)
// ----------------------------------------------------------------------------
static void build_default_experiments(void)
{
    Experiment *e;

    // CONTROL: rewrite 0x1C4 with its boot value (no change) -- writes land, stable.
    e = new_exp("CONTROL rewrite 0x1C4 boot value (no change)", true);
    exp_add_w(e, 0x1C4, 0x002006C0u, FULL);

    // TARGET 1: 74.25 MHz = 54 * 11/8  (Nfb=11 byte1=0x0A, V=8)
    e = new_exp("TARGET 74.25 MHz  (Nfb=11, V=8)", true);
    exp_add_w(e, 0x1C4, mk1c4(0x0A, 0xC0), FULL);
    exp_add_w(e, 0x1C8, mk1c8(8), FULL);
    e->predict_khz = 74250;

    // TARGET 2: 148.5 MHz = 54 * 22/8  (Nfb=22 byte1=0x15, V=8)
    e = new_exp("TARGET 148.5 MHz  (Nfb=22, V=8)", true);
    exp_add_w(e, 0x1C4, mk1c4(0x15, 0xC0), FULL);
    exp_add_w(e, 0x1C8, mk1c8(8), FULL);
    e->predict_khz = 148500;

    // TARGET 2 (alt): 148.5 MHz = 54 * 11/4  (Nfb=11, V=4)  -- tests V below baseline
    e = new_exp("TARGET 148.5 MHz alt  (Nfb=11, V=4)", true);
    exp_add_w(e, 0x1C4, mk1c4(0x0A, 0xC0), FULL);
    exp_add_w(e, 0x1C8, mk1c8(4), FULL);
    e->predict_khz = 148500;

    // V linearity check at base Nfb=7:  F = 54 * 7 / V = 378 / V MHz
    static const u32 vv[] = { 6, 5, 9, 10 };
    for (unsigned i = 0; i < sizeof(vv)/sizeof(vv[0]); i++) {
        char nm[96];
        snprintf(nm, sizeof(nm), "V-check 0x1C8 V=%u (base Nfb=7)", (unsigned)vv[i]);
        e = new_exp(nm, true);
        exp_add_w(e, 0x1C8, mk1c8(vv[i]), FULL);
        e->predict_khz = (u32)(54000ull * 7 / vv[i]);
    }

    // Isolation sanity: each target knob alone.
    e = new_exp("SANITY Nfb=11 alone (V=7)", true);
    exp_add_w(e, 0x1C4, mk1c4(0x0A, 0xC0), FULL);
    e->predict_khz = 84857;
    e = new_exp("SANITY V=8 alone (Nfb=7)", true);
    exp_add_w(e, 0x1C8, mk1c8(8), FULL);
    e->predict_khz = 47250;
}

// ----------------------------------------------------------------------------
// Optional config override: usb:/vi_pll.cfg (or sd:). If present, the built-in
// sweep is REPLACED by explicit tries. Data only, parsed defensively.
//   Lines:
//     # comment / blank                 ignored
//     TRY OFF=VAL [OFF2=VAL2 ...]        one experiment, anchored at 1280x240p/54MHz
//     TRYRAW OFF=VAL [...]               one experiment, NO anchor (current mode kept)
//   OFF is a Hollywood offset in hex (e.g. 1C4); VAL is a 32-bit hex value.
//   Only offsets in CAND[] are accepted (so restore always covers them).
// ----------------------------------------------------------------------------
static bool off_is_candidate(u32 off)
{
    for (int i = 0; i < NCAND; i++) if (CAND[i] == off) return true;
    return false;
}

static bool load_cfg(void)
{
    const char *paths[] = { "usb:/vi_pll.cfg", "sd:/vi_pll.cfg" };
    FILE *f = NULL;
    for (int i = 0; i < 2 && !f; i++) f = fopen(paths[i], "r");
    if (!f) return false;

    char ln[256];
    int added = 0;
    while (fgets(ln, sizeof(ln), f)) {
        char *p = ln;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;

        bool anchor = true;
        char *tok = strtok(p, " \t\r\n");
        if (!tok) continue;
        if (strcasecmp(tok, "TRY") == 0)         anchor = true;
        else if (strcasecmp(tok, "TRYRAW") == 0) anchor = false;
        else continue;

        char name[96];
        snprintf(name, sizeof(name), "CFG %s", anchor ? "TRY" : "TRYRAW");
        Experiment *e = new_exp(name, anchor);
        int pairs = 0;
        char *q;
        while ((q = strtok(NULL, " \t\r\n")) != NULL) {
            char *eq = strchr(q, '=');
            if (!eq) continue;
            *eq = '\0';
            u32 off = (u32)strtoul(q, NULL, 16);
            u32 val = (u32)strtoul(eq + 1, NULL, 16);
            if (!off_is_candidate(off)) continue;   // refuse offsets we do not snapshot
            exp_add_w(e, off, val, FULL);
            size_t L = strlen(e->name);
            snprintf(e->name + L, sizeof(e->name) - L, " %03X=%08X", (unsigned)off, (unsigned)val);
            pairs++;
        }
        if (pairs == 0) { nexp--; }  // drop empty TRY line
        else added++;
    }
    fclose(f);
    return added > 0;
}

// ----------------------------------------------------------------------------
// Run one experiment with full write-ahead logging and auto-restore.
// ----------------------------------------------------------------------------
static void run_experiment(int idx, Experiment *e, u32 base_eff)
{
    logf_("");
    logf_("### EXPERIMENT %d: %s", idx, e->name);

    if (e->anchor_hires) {
        set_mode(&hires);
        apply_hires();
    }

    // WRITE-AHEAD: log every intended write (with the value we will read-modify from)
    // and the file is committed before the poke happens.
    for (int i = 0; i < e->nw; i++) {
        logf_("  WILL WRITE HW %03X = %08X (mask %08X)  before=%08X",
              (unsigned)e->w[i].off, (unsigned)e->w[i].val, (unsigned)e->w[i].mask,
              (unsigned)HW(e->w[i].off));
    }
    logf_("  >>> poking now <<<");

    // Apply all writes atomically with interrupts disabled (just the poke).
    u32 lvl;
    _CPU_ISR_Disable(lvl);
    for (int i = 0; i < e->nw; i++)
        hw_write(e->w[i].off, e->w[i].val, e->w[i].mask);
    _CPU_ISR_Restore(lvl);

    // Read back what actually stuck (PLLs may ignore or massage written bits).
    for (int i = 0; i < e->nw; i++)
        logf_("  readback HW %03X = %08X", (unsigned)e->w[i].off, (unsigned)HW(e->w[i].off));

    if (e->nfb > 0)
        logf_("  model predicts ~ %u kHz for Nfb=%d  ( (base/7)*Nfb )",
              (u32)((u64)base_eff * (u32)e->nfb / 7u), e->nfb);
    if (e->predict_khz > 0)
        logf_("  EXPECTED ~ %u kHz", e->predict_khz);

    tb_delay_ms(250);            // let the PLL attempt to relock
    Meas m = measure();
    log_meas("after-write", &m, base_eff);

    if (e->hold_ms) tb_delay_ms(e->hold_ms);

    // Restore the snapshot for all candidates and confirm recovery.
    restore_candidates();
    tb_delay_ms(250);
    Meas r = measure();
    log_meas("after-restore", &r, base_eff);
    logf_("  --- end experiment %d ---", idx);
}

// ----------------------------------------------------------------------------
static void dump_vi(void)
{
    for (int r = 0; r < 0x78; r += 16)
        logf_("  VI %02X: %04X %04X %04X %04X %04X %04X %04X %04X", r,
              VI[r / 2], VI[r / 2 + 1], VI[r / 2 + 2], VI[r / 2 + 3],
              VI[r / 2 + 4], VI[r / 2 + 5], VI[r / 2 + 6], VI[r / 2 + 7]);
}
static void dump_candidates(const char *tag)
{
    logf_("  [%s] HW_AHBPROT(064)=%08X HW_VI1CFG(018)=%08X", tag, HW(0x064), HW(0x018));
    for (u32 off = 0x190; off < 0x200; off += 16)
        logf_("  [%s] HW %03X: %08X %08X %08X %08X", tag, off,
              HW(off), HW(off + 4), HW(off + 8), HW(off + 12));
}

// wait for A (GC or Wiimote) with a timeout; returns true if A pressed, false on timeout/HOME.
static bool wait_start(u32 timeout_ms)
{
    u64 to = gettime() + millisecs_to_ticks(timeout_ms);
    while (gettime() < to) {
        WPAD_ScanPads();
        PAD_ScanPads();
        u32 wd = WPAD_ButtonsDown(0);
        u32 gd = PAD_ButtonsDown(0);
        if (wd & WPAD_BUTTON_A) return true;
        if (gd & PAD_BUTTON_A)  return true;
        if ((wd & WPAD_BUTTON_HOME) || (gd & PAD_BUTTON_START)) return false;
        VIDEO_WaitVSync();
    }
    return true; // timed out -> proceed (works unattended / no controller synced)
}

int main(void)
{
    VIDEO_Init();
    WPAD_Init();
    PAD_Init();

    rmode = VIDEO_GetPreferredMode(NULL);
    // one buffer large enough for every mode; grey fill (YUV 0x80)
    xfb = MEM_K0_TO_K1(memalign(32, 1280 * 576 * 2));
    for (u32 i = 0; i < 1280 * 576 / 2; i++) ((u32 *)xfb)[i] = 0x80808080;

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetPostRetraceCallback(on_retrace);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    CON_Init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    printf("\x1b[2;0H");

    bool fat = fatInitDefault();

    logf_("vi_pll (WRITES clock regs) build " __DATE__ " " __TIME__);
    logf_("fatInitDefault=%d  component cable=%d  tvmode=%d",
          fat, VIDEO_HaveComponentCable(), VIDEO_GetCurrentTvMode());

    // Snapshot BEFORE anything, log it as the record of boot-state candidates.
    snapshot();
    logf_("=== boot-state candidate registers (will be restored after each experiment) ===");
    dump_candidates("boot");
    dump_vi();

    // Build the experiment list: cfg override if present, else the default field map.
    bool have_cfg = load_cfg();
    if (!have_cfg) {
        build_default_experiments();
        logf_("no usb:/vi_pll.cfg found -> running built-in control + field-map sweep (%d experiments)", nexp);
    } else {
        logf_("usb:/vi_pll.cfg loaded -> running %d explicit experiment(s)", nexp);
    }

    logf_("reference pixel clocks: stock=%u kHz  720p/1080i=%u kHz  'extrems 148.5'=%u kHz",
          CLK_54, CLK_7425, CLK_148);

    // On-screen prompt (video is synced here, before any PLL write).
    printf("\n  vi_pll: writes undocumented VI clock registers.\n");
    printf("  %d experiment(s) queued. Logs -> vi_pll.txt on USB%s.\n", nexp, fat ? "/SD" : " (NO FAT MOUNTED!)");
    printf("  Each write is held briefly then restored; all reset on reboot.\n");
    printf("  Press A to start (auto-start in 10 s). HOME/Start to exit.\n");
    if (!wait_start(10000)) {
        logf_("user aborted at start prompt");
        logf_("DONE");
        return 0;
    }

    // Establish the 54 MHz baseline (fixed 1280x240p timing) that the sweep compares to.
    logf_("");
    logf_("=== baseline: 1280x240p @ stock 54 MHz (no clock write) ===");
    set_mode(&hires);
    apply_hires();
    Meas base = measure();
    log_meas("baseline", &base, 0);
    u32 base_eff = base.line_ok ? base.eff_khz : CLK_54;
    logf_("baseline effective pixel clock used for ratios = %u kHz", base_eff);

    // Run everything.
    for (int i = 0; i < nexp; i++)
        run_experiment(i, &exps[i], base_eff);

    // Final safety restore + return to a sane display.
    restore_candidates();
    tb_delay_ms(250);
    set_mode(VIDEO_GetPreferredMode(NULL));
    logf_("");
    logf_("all experiments done; candidates restored to boot snapshot.");
    dump_candidates("final");
    logf_("DONE");
    return 0; // back to the Homebrew Channel
}
