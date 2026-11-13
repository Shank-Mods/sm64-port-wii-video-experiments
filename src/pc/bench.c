#ifdef TARGET_GX

// Uncapped timedemo benchmark: frame statistics, per-task CPU/GPU breakdown, clock
// calibration, results file and the overlay text. See bench.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm64.h"
#include "game/game_init.h"
#include "game/area.h"
#include "level_table.h"
#include "course_table.h"

#include "gfx/gfx_gx_wm.h" // gccore.h, with its Mtx/Vtx renamed away from the game's
#include <ogc/irq.h>

#include "bench.h"
#include "configfile.h"

bool gBenchOn;
uint32_t gBenchMark;
int gBenchSp;
uint8_t gBenchStack[16];
uint32_t gBenchAcc[BENCH_CPU_COUNT];

static const char *const cpu_names[BENCH_CPU_COUNT] = {
    "Other", "Game logic", "Scene graph", "Audio synth", "DL+tri setup", "Vertex xform",
    "Tex decode", "GX submit", "GPU wait", "Present+vsync", "Overlay",
};
static const char *const gpu_names[BENCH_GPU_COUNT] = { "3D world", "2D/HUD/sky", "XFB copy" };

uint64_t bench_tb64(void) {
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------------------------------
// Clock calibration
//
// The VI retrace interrupt fires once per field from the video clock, which an overclock of the
// CPU/GPU/bus PLL doesn't touch. Counting timebase ticks between retraces gives the real
// timebase rate, so every time below is in real milliseconds whatever the overclock.

#ifdef __wii__
#define STOCK_TB_HZ 60750000.0 // 243 MHz bus / 4
#define STOCK_CPU_MHZ 729.0
#define CONSOLE_NAME "Wii"
#else
#define STOCK_TB_HZ 40500000.0 // GameCube: 162 MHz bus / 4
#define STOCK_CPU_MHZ 486.0
#define CONSOLE_NAME "GameCube"
#endif

static volatile uint32_t vi_count;
static volatile uint64_t vi_tb_first, vi_tb_last;
static volatile uint32_t vi_shown;
static double field_hz = 60000.0 / 1001.0;
static double tb_hz = STOCK_TB_HZ;
static bool tb_calibrated;
static double cpu_mhz;

void bench_on_retrace(bool new_frame_shown) {
    uint64_t t = bench_tb64();
    if (vi_count == 0)
        vi_tb_first = t;
    vi_tb_last = t;
    vi_count++;
    if (new_frame_shown)
        vi_shown++;
}

static void update_calibration(void) {
    u32 level = IRQ_Disable();
    uint32_t n = vi_count;
    uint64_t first = vi_tb_first, last = vi_tb_last;
    IRQ_Restore(level);

    if (n > 30) {
        tb_hz = (double)(last - first) / (double)(n - 1) * field_hz;
        tb_calibrated = n >= 120;
    }
}

// A chain of 32 dependent adds per iteration: one add per cycle on Broadway, with the loop
// branch folded away, so the CPU clock is about 32 * iterations / elapsed time.
static uint32_t add_chain(uint32_t iterations) {
    uint32_t r = 0;
    __asm__ volatile(
        "mtctr %1\n"
        "1:\n"
        ".rept 32\n"
        "addi %0,%0,1\n"
        ".endr\n"
        "bdnz 1b\n"
        : "+r"(r)
        : "r"(iterations)
        : "ctr");
    return r;
}

static bool restart_frame_clock; // set after a slow measurement so no frame is charged for it

static void measure_cpu_clock(void) {
    restart_frame_clock = true;
    const uint32_t iters = 400000; // ~17.6 ms at 729 MHz
    uint32_t best = 0xFFFFFFFF;
    for (int i = 0; i < 3; i++) {
        uint32_t t0 = bench_tb();
        add_chain(iters);
        uint32_t dt = bench_tb() - t0;
        if (dt < best)
            best = dt;
    }
    if (best > 0)
        cpu_mhz = 32.0 * iters / ((double)best / tb_hz) / 1e6;
}

static double ms(uint64_t ticks) {
    return (double)ticks * 1000.0 / tb_hz;
}

// ---------------------------------------------------------------------------------------------
// State

static uint32_t frame_start;
static bool have_frame;
static bool frame_split;
static bool frame_demo;
static bool frame_gpu_valid;
static bool frame_spilled;
static bool frame_ticked; // a game logic tick ran in this frame
static uint32_t frame_gpu_total;
static uint32_t frame_gpu[BENCH_GPU_COUNT];

// 0.5 s window shown live on the overlay
static uint32_t win_start;
static uint32_t win_frames;
static uint64_t win_sum;
static uint32_t win_max;
static uint64_t win_cat[BENCH_CPU_COUNT];
static uint32_t win_gpu_frames;
static uint64_t win_gpu_total;
static uint64_t win_gpu[BENCH_GPU_COUNT];
static uint32_t win_vi_start, win_shown_start;
static uint32_t win_ticks;

struct Shown {
    bool valid;
    double fps, frame_ms, max_ms, vi_hz, shown_fps;
    double cat_ms[BENCH_CPU_COUNT];
    bool gpu_valid;      // from split frames in this window
    double gpu_ms, gpu_task_ms[BENCH_GPU_COUNT];
    double tick_hz;      // game logic ticks per second
};
static struct Shown shown;
static struct Shown last_split; // last window that had split frames
static bool ov_dirty = true;

// Timedemo passes. A pass is one run through every attract-mode demo.
#define MAX_DEMOS 16
#define MAX_PASS_FRAMES 32768
#define MAX_HISTORY 3

struct DemoStat {
    int level;
    uint32_t frames;
    uint64_t sum;
    uint32_t worst;
    uint64_t cat[BENCH_CPU_COUNT];
    uint32_t gpu_frames;
    uint64_t gpu_total;
    uint64_t gpu[BENCH_GPU_COUNT];
    uint32_t spilled;
    uint32_t ft_first; // index into pass_ft
    uint32_t ticks;    // game logic ticks run during these frames
};

static bool demo_active;
static bool pass_running;
static bool pass_split;
static int pass_no;
static int pass_demos_done;
static int pass_ndemo;
static struct DemoStat pass_demo[MAX_DEMOS];
static struct DemoStat *cur_demo;
static uint32_t pass_ft[MAX_PASS_FRAMES];
static uint32_t pass_nft;
static uint32_t sort_buf[MAX_PASS_FRAMES];

struct History {
    int pass;
    bool split;
    double fps, low1;
};
static struct History history[MAX_HISTORY];
static int nhistory;
static char result_note[48];

// ---------------------------------------------------------------------------------------------

bool bench_wanted(void) {
    return configBenchmark || configBenchOverlay;
}

static uint32_t win_no; // 0.5 s overlay windows closed so far

static int split_setting(void) {
    // 0: never, 1: always, 2: alternate passes (odd passes overlapped, even passes split)
    return configBenchSplit > 2 ? 2 : (int)configBenchSplit;
}

static bool split_for_pass(int n) {
    switch (split_setting()) {
        case 0: return false;
        case 1: return true;
        default: return (n % 2) == 0;
    }
}

// Normal play has no passes: with the alternating setting, one overlay window in four is split
// so the GPU breakdown stays fresh without costing the overlap most of the time
static bool split_for_play_window(uint32_t n) {
    switch (split_setting()) {
        case 0: return false;
        case 1: return true;
        default: return (n % 4) == 3;
    }
}

bool bench_split_this_frame(void) {
    return gBenchOn && frame_split;
}

bool bench_overlay_enabled(void) {
    return gBenchOn && configBenchOverlay;
}

void bench_gpu_frame(bool valid, uint32_t total, const uint32_t task[BENCH_GPU_COUNT], bool spilled) {
    frame_gpu_valid = valid;
    frame_gpu_total = total;
    frame_spilled = spilled;
    for (int i = 0; i < BENCH_GPU_COUNT; i++)
        frame_gpu[i] = valid ? task[i] : 0;
}

#define STUB_LEVEL(_0, levelenum, _2, _3, _4, _5, _6, _7, _8)
#define DEFINE_LEVEL(_0, levelenum, _2, folder, _4, _5, _6, _7, _8, _9, _10) [levelenum] = #folder,
static const char *const level_folders[LEVEL_COUNT] = {
#include "levels/level_defines.h"
};
#undef STUB_LEVEL
#undef DEFINE_LEVEL

// Short upper-case level name ("WF", "CCM"), or NULL
static const char *level_name(int level) {
    static char names[LEVEL_COUNT][12];
    if (level <= LEVEL_NONE || level >= LEVEL_COUNT || level_folders[level] == NULL)
        return NULL;
    if (names[level][0] == '\0') {
        const char *f = level_folders[level];
        int i = 0;
        for (; f[i] != '\0' && i < 11; i++)
            names[level][i] = (f[i] >= 'a' && f[i] <= 'z') ? f[i] - 32 : f[i];
        names[level][i] = '\0';
    }
    return names[level];
}

static int demo_count(void) {
    if (gDemo.animDmaTable != NULL && gDemo.animDmaTable->count > 0 && gDemo.animDmaTable->count <= MAX_DEMOS)
        return (int)gDemo.animDmaTable->count;
    return 7;
}

static const char *video_desc(void) {
    static char buf[24];
    GXRModeObj *r = gfx_gx_wm_get_rmode();
    if (r == NULL)
        return "?";
    if (gfx_gx_wm_hd_name() != NULL)
        return gfx_gx_wm_hd_name(); // 720p etc., or "1920x1020>480" for a crop
    if (gfx_gx_wm_hires())
        return "1280x240p";
    char scan = (r->viTVMode & 3) == VI_INTERLACE ? 'i' : 'p';
    snprintf(buf, sizeof(buf), "%ux%u%c", r->fbWidth, r->xfbHeight, scan);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Results

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// FPS of the slowest 1% of frames (99th percentile frame time)
static double low_fps(const uint32_t *ft, uint32_t n) {
    if (n == 0)
        return 0.0;
    memcpy(sort_buf, ft, n * sizeof(uint32_t));
    qsort(sort_buf, n, sizeof(uint32_t), cmp_u32);
    uint32_t idx = (uint32_t)((double)n * 0.99);
    if (idx >= n)
        idx = n - 1;
    return 1000.0 / ms(sort_buf[idx]);
}

static void sorted_order(const double *v, int n, int *order) {
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && v[order[j]] < v[k]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
}

static void finish_pass(void) {
    pass_running = false;
    measure_cpu_clock(); // ~50 ms; only ever done between passes so no measured frame pays for it

    struct DemoStat all;
    memset(&all, 0, sizeof(all));
    for (int d = 0; d < pass_ndemo; d++) {
        struct DemoStat *s = &pass_demo[d];
        all.frames += s->frames;
        all.sum += s->sum;
        all.gpu_frames += s->gpu_frames;
        all.gpu_total += s->gpu_total;
        all.spilled += s->spilled;
        all.ticks += s->ticks;
        if (s->worst > all.worst)
            all.worst = s->worst;
        for (int i = 0; i < BENCH_CPU_COUNT; i++)
            all.cat[i] += s->cat[i];
        for (int i = 0; i < BENCH_GPU_COUNT; i++)
            all.gpu[i] += s->gpu[i];
    }
    if (all.frames == 0)
        return;

    double fps = all.frames / (ms(all.sum) / 1000.0);
    double low = low_fps(pass_ft, pass_nft);

    if (nhistory == MAX_HISTORY) {
        memmove(&history[0], &history[1], sizeof(history[0]) * (MAX_HISTORY - 1));
        nhistory--;
    }
    history[nhistory++] = (struct History){ pass_no, pass_split, fps, low };

#ifdef __wii__
    const char *path = get_storage_path("sm64_bench.txt");
#else
    const char *path = "sm64_bench.txt"; // next to the config, as the GameCube port keeps it
#endif
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        snprintf(result_note, sizeof(result_note), "could not write results file");
        return;
    }

    fprintf(f, "\n=== SM64 Wii benchmark, pass %d (%s) ===\n", pass_no,
            pass_split ? "split: GPU runs after CPU" : "overlapped: normal pipelining");
    fprintf(f, "build:  %s %s (%s)\n", __DATE__, __TIME__, CONSOLE_NAME);
    fprintf(f, "video:  %s, %.3f Hz field rate\n", video_desc(), field_hz);
    fprintf(f, "mode:   %s\n", configBenchUncapped ? "uncapped (one game tick per frame, no vsync)"
                                : configBenchRender ? "render uncapped (30 Hz game logic, frames interpolated, no vsync)"
                                : configFramePacingRealtime ? "capped, realtime pacing (30 Hz game logic, frames interpolated, at vsync)"
                                                    : "capped (presented at vsync)");
    fprintf(f, "clock:  timebase %.3f MHz -> bus %.1f MHz (x%.3f of stock)%s; CPU est. %.0f MHz\n",
            tb_hz / 1e6, tb_hz * 4.0 / 1e6, tb_hz / STOCK_TB_HZ,
            tb_calibrated ? "" : " [not yet calibrated]", cpu_mhz);
    fprintf(f, "\n%-9s %7s %9s %8s %9s %9s", "demo", "frames", "avg fps", "1% low", "ms/frame", "worst ms");
    if (config_rt_pacing())
        fprintf(f, " %8s %8s", "logic Hz", "fr/tick");
    if (pass_split)
        fprintf(f, " %8s %8s", "cpu ms", "gpu ms");
    fprintf(f, "\n");

    for (int d = 0; d <= pass_ndemo; d++) {
        struct DemoStat *s = d < pass_ndemo ? &pass_demo[d] : &all;
        if (s->frames == 0)
            continue;
        char name[16];
        if (d == pass_ndemo)
            snprintf(name, sizeof(name), "ALL");
        else if (level_name(s->level) != NULL)
            snprintf(name, sizeof(name), "%s", level_name(s->level));
        else
            snprintf(name, sizeof(name), "demo%d", d + 1);
        double dfps = s->frames / (ms(s->sum) / 1000.0);
        double dlow = d < pass_ndemo ? low_fps(pass_ft + s->ft_first, s->frames) : low;
        fprintf(f, "%-9s %7u %9.2f %8.2f %9.3f %9.2f", name, (unsigned)s->frames, dfps, dlow,
                ms(s->sum) / s->frames, ms(s->worst));
        if (config_rt_pacing())
            fprintf(f, " %8.2f %8.2f", s->ticks / (ms(s->sum) / 1000.0),
                    s->ticks > 0 ? (double)s->frames / s->ticks : 0.0);
        if (pass_split && s->gpu_frames > 0) {
            double cpu = (ms(s->sum) - ms(s->cat[BENCH_GPUWAIT])) / s->frames;
            fprintf(f, " %8.3f %8.3f", cpu, ms(s->gpu_total) / s->gpu_frames);
        }
        fprintf(f, "\n");
    }

    double cat_ms[BENCH_CPU_COUNT];
    int order[BENCH_CPU_COUNT];
    for (int i = 0; i < BENCH_CPU_COUNT; i++)
        cat_ms[i] = ms(all.cat[i]) / all.frames;
    sorted_order(cat_ms, BENCH_CPU_COUNT, order);
    fprintf(f, "\nCPU time per frame, all demos (ms, %% of frame):\n");
    double frame_ms = ms(all.sum) / all.frames;
    for (int i = 0; i < BENCH_CPU_COUNT; i++) {
        int c = order[i];
        fprintf(f, "  %-13s %7.3f  %5.1f%%\n", cpu_names[c], cat_ms[c], 100.0 * cat_ms[c] / frame_ms);
    }
    if (pass_split && all.gpu_frames > 0) {
        fprintf(f, "GPU time per frame, all demos (ms):\n");
        for (int i = 0; i < BENCH_GPU_COUNT; i++)
            fprintf(f, "  %-13s %7.3f\n", gpu_names[i], ms(all.gpu[i]) / all.gpu_frames);
        fprintf(f, "  %-13s %7.3f\n", "total", ms(all.gpu_total) / all.gpu_frames);
        if (all.spilled)
            fprintf(f, "  (%u frames were too large to hold back and ran overlapped)\n", (unsigned)all.spilled);
    }
    fprintf(f, "\nSCORE pass %d %s: %.2f avg fps, %.2f 1%% low\n", pass_no, pass_split ? "split" : "overlapped",
            fps, low);
    fclose(f);
    snprintf(result_note, sizeof(result_note), "saved to %s", path);
}

static void demo_start(void) {
    if (!pass_running) {
        pass_running = true;
        pass_no++;
        pass_split = split_for_pass(pass_no);
        pass_demos_done = 0;
        pass_ndemo = 0;
        pass_nft = 0;
        memset(pass_demo, 0, sizeof(pass_demo));
    }
    if (pass_ndemo < MAX_DEMOS) {
        cur_demo = &pass_demo[pass_ndemo++];
        cur_demo->ft_first = pass_nft;
    } else {
        cur_demo = NULL;
    }
}

static void demo_end(void) {
    cur_demo = NULL;
    if (!pass_running)
        return;
    if (++pass_demos_done >= demo_count())
        finish_pass();
}

void bench_tick_done(void) {
    if (!gBenchOn)
        return;
    // Passes are only timed in demo mode (an idle title screen also plays demos)
    bool active = configBenchmark && gCurrDemoInput != NULL;
    if (active && !demo_active)
        demo_start();
    else if (!active && demo_active)
        demo_end();
    demo_active = active;
    frame_ticked = true;
    frame_demo = active && pass_running && cur_demo != NULL;
    if (frame_demo && gCurrCourseNum != COURSE_NONE)
        cur_demo->level = gCurrLevelNum;
}

// Render-uncapped mode: a frame drawn between game ticks still belongs to the running demo
void bench_frame_continue(void) {
    if (!gBenchOn)
        return;
    frame_demo = demo_active && pass_running && cur_demo != NULL;
}

double bench_tb_hz(void) {
    return tb_hz;
}

// ---------------------------------------------------------------------------------------------
// Overlay text

#define OV_COLS 40
#define OV_ROWS ((BENCH_OV_H - 4) / GLYPH_H)
#define GLYPH_W 8
#define GLYPH_H 13
#define GLYPH_ROW0 2 // rows 2..14 of the 8x16 console font

extern u8 console_font_8x16[];

enum { C_WHITE = 1, C_YELLOW, C_CYAN, C_GRAY, C_GREEN, C_ORANGE, C_RED };
static const uint16_t colors[] = {
    0x0000,
    0xFFFF,                                  // white
    0x8000 | (31 << 10) | (31 << 5) | 6,     // yellow
    0x8000 | (8 << 10) | (28 << 5) | 31,     // cyan
    0x8000 | (19 << 10) | (19 << 5) | 19,    // gray
    0x8000 | (10 << 10) | (31 << 5) | 10,    // green
    0x8000 | (31 << 10) | (19 << 5) | 4,     // orange
    0x8000 | (31 << 10) | (8 << 5) | 8,      // red
};
#define OV_BG 0x5000 // black at 5/7 alpha

static char ov_text[OV_ROWS][OV_COLS * 2 + 1];
static int ov_nrows;

// Lines may contain \1..\7 to change colour
static void ov_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ov_line(const char *fmt, ...) {
    if (ov_nrows >= OV_ROWS)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ov_text[ov_nrows], sizeof(ov_text[0]), fmt, ap);
    va_end(ap);
    ov_nrows++;
}

static void bar(char *buf, double frac) {
    int n = (int)(frac * 10.0 + 0.5);
    if (n > 10)
        n = 10;
    buf[0] = C_ORANGE;
    for (int i = 0; i < n; i++)
        buf[1 + i] = (char)0xDB; // CP437 full block
    buf[1 + n] = '\0';
}

static void build_overlay_text(void) {
    ov_nrows = 0;
    const struct Shown *s = &shown;

    char head[64];
    if (!configBenchmark) {
        snprintf(head, sizeof(head), "PLAY %s%s", configBenchUncapped ? "UNCAPPED" : configBenchRender ? "RENDER"
                                                  : configFramePacingRealtime ? (config60Fps ? "60FPS RT" : "30FPS RT")
                                                  : (config60Fps ? "60FPS" : "30FPS"),
                 frame_split ? " \4SPLIT" : "");
    } else if (pass_running) {
        const char *name = cur_demo != NULL ? level_name(cur_demo->level) : NULL;
        snprintf(head, sizeof(head), "P%d %s  DEMO %d/%d %s", pass_no, pass_split ? "SPLIT" : "OVERLAP",
                 pass_ndemo, demo_count(), name != NULL ? name : "");
    } else {
        snprintf(head, sizeof(head), "\4%s (P%d %s)", pass_no == 0 ? "waiting for demo" : "between passes",
                 pass_no + 1, split_for_pass(pass_no + 1) ? "split" : "overlap");
    }
    ov_line("\2SM64 BENCH \1%s", head);

    if (!s->valid) {
        ov_line("\4measuring...");
        return;
    }

    ov_line("FPS \2%6.1f\1  frame %5.2fms  max %5.1f", s->fps, s->frame_ms, s->max_ms);
    if (!configBenchUncapped) // game ticks per second: 30 at full speed, lower when slowed down
        ov_line("\4logic %5.2f Hz  %5.2f frames/tick", s->tick_hz,
                s->tick_hz > 0 ? s->fps / s->tick_hz : 0.0);
    if (!configBenchUncapped && !configBenchRender) {
        // At vsync most of the frame can be waiting; what's left is the real load
        double busy = s->frame_ms - s->cat_ms[BENCH_PRESENT];
        ov_line("\4busy %5.2fms  %3.0f%% of frame  (~%.0f fps)", busy,
                s->frame_ms > 0 ? 100.0 * busy / s->frame_ms : 0.0, busy > 0 ? 1000.0 / busy : 0.0);
    }
    ov_line("\4TV %4.1f fps  VI %5.2f Hz  %s", s->shown_fps, s->vi_hz, video_desc());
    if (!tb_calibrated)
        ov_line("\4bus %.1f MHz (calibrating...)", tb_hz * 4.0 / 1e6);
    else if (cpu_mhz > 0.0)
        ov_line("\4bus %.1f MHz x%.3f  CPU~%.0f MHz", tb_hz * 4.0 / 1e6, tb_hz / STOCK_TB_HZ, cpu_mhz);
    else
        ov_line("\4bus %.1f MHz x%.3f", tb_hz * 4.0 / 1e6, tb_hz / STOCK_TB_HZ);

    ov_line("\3CPU TASK          ms     %%");
    int order[BENCH_CPU_COUNT];
    sorted_order(s->cat_ms, BENCH_CPU_COUNT, order);
    for (int i = 0; i < BENCH_CPU_COUNT; i++) {
        int c = order[i];
        if (s->cat_ms[c] < 0.05)
            continue;
        char b[16];
        double frac = s->frame_ms > 0 ? s->cat_ms[c] / s->frame_ms : 0;
        bar(b, frac);
        ov_line(" %-13s%6.2f %5.1f %s", cpu_names[c], s->cat_ms[c], 100.0 * frac, b);
    }

    const struct Shown *g = s->gpu_valid ? s : (last_split.gpu_valid ? &last_split : NULL);
    if (g != NULL) {
        double cpu_only = g->frame_ms - g->cat_ms[BENCH_GPUWAIT];
        ov_line("\3GPU TASK          ms%s", s->gpu_valid ? "" : "  \4(last split)");
        for (int i = 0; i < BENCH_GPU_COUNT; i++) {
            char b[16];
            double frac = g->frame_ms > 0 ? g->gpu_task_ms[i] / g->frame_ms : 0;
            bar(b, frac);
            ov_line(" %-13s%6.2f %5.1f %s", gpu_names[i], g->gpu_task_ms[i], 100.0 * frac, b);
        }
        ov_line(" CPU %.2fms %.0ffps  GPU %.2fms %.0ffps", cpu_only, cpu_only > 0 ? 1000.0 / cpu_only : 0.0,
                g->gpu_ms, g->gpu_ms > 0 ? 1000.0 / g->gpu_ms : 0.0);
    } else {
        ov_line("\3GPU \4per-task times on split passes");
    }

    for (int i = 0; i < nhistory; i++) {
        const struct History *h = &history[i];
        ov_line("\5P%d %-7s %6.2f fps  1%% %6.2f", h->pass, h->split ? "split" : "overlap", h->fps, h->low1);
    }
    if (result_note[0] != '\0' && nhistory > 0)
        ov_line("\4%s", result_note);
}

bool bench_overlay_update(uint16_t *tex, int width, int height) {
    if (!ov_dirty)
        return false;
    ov_dirty = false;
    build_overlay_text();

    const int tiles_w = width / 4;
    // Background over the rows in use, transparent below
    int used_h = ov_nrows * GLYPH_H + 4;
    for (int y = 0; y < height; y++) {
        uint16_t v = y < used_h ? OV_BG : 0x0000;
        for (int x = 0; x < width; x++)
            tex[((y >> 2) * tiles_w + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3)] = v;
    }

    for (int r = 0; r < ov_nrows; r++) {
        uint16_t color = colors[C_WHITE];
        int col = 0;
        for (const unsigned char *p = (const unsigned char *)ov_text[r]; *p != '\0' && col < OV_COLS; p++) {
            if (*p < 8) {
                color = colors[*p];
                continue;
            }
            const u8 *glyph = &console_font_8x16[*p * 16 + GLYPH_ROW0];
            int x0 = 4 + col * GLYPH_W, y0 = 2 + r * GLYPH_H;
            for (int gy = 0; gy < GLYPH_H; gy++) {
                u8 bits = glyph[gy];
                if (*p == 0xDB) // bar segments: a band in the middle of the row, with no side gap
                    bits = (gy >= 3 && gy <= 9) ? 0xFF : 0x00;
                int y = y0 + gy;
                if (y >= height)
                    break;
                for (int gx = 0; gx < GLYPH_W; gx++) {
                    int x = x0 + gx;
                    if ((bits & (0x80 >> gx)) && x < width)
                        tex[((y >> 2) * tiles_w + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3)] = color;
                }
            }
            col++;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Per-frame bookkeeping

static void close_window(uint32_t now) {
    struct Shown *s = &shown;
    double dt = ms(now - win_start) / 1000.0;

    update_calibration();
    if (tb_calibrated && cpu_mhz == 0.0 && !pass_running)
        measure_cpu_clock();

    u32 level = IRQ_Disable();
    uint32_t vi_n = vi_count, sh_n = vi_shown;
    IRQ_Restore(level);

    s->valid = win_frames > 0;
    if (s->valid) {
        s->fps = win_frames / (ms(win_sum) / 1000.0);
        s->frame_ms = ms(win_sum) / win_frames;
        s->max_ms = ms(win_max);
        for (int i = 0; i < BENCH_CPU_COUNT; i++)
            s->cat_ms[i] = ms(win_cat[i]) / win_frames;
        s->vi_hz = dt > 0 ? (vi_n - win_vi_start) / dt : 0;
        s->tick_hz = dt > 0 ? win_ticks / dt : 0;
        s->shown_fps = dt > 0 ? (sh_n - win_shown_start) / dt : 0;
        s->gpu_valid = win_gpu_frames > win_frames / 2;
        if (s->gpu_valid) {
            s->gpu_ms = ms(win_gpu_total) / win_gpu_frames;
            for (int i = 0; i < BENCH_GPU_COUNT; i++)
                s->gpu_task_ms[i] = ms(win_gpu[i]) / win_gpu_frames;
            last_split = *s;
        }
    }

    win_no++;
    win_start = now;
    win_frames = 0;
    win_ticks = 0;
    win_sum = 0;
    win_max = 0;
    memset(win_cat, 0, sizeof(win_cat));
    win_gpu_frames = 0;
    win_gpu_total = 0;
    memset(win_gpu, 0, sizeof(win_gpu));
    win_vi_start = vi_n;
    win_shown_start = sh_n;
    ov_dirty = true;
}

static void record_frame(uint32_t ft, uint32_t now) {
    win_frames++;
    if (frame_ticked)
        win_ticks++;
    win_sum += ft;
    if (ft > win_max)
        win_max = ft;
    for (int i = 0; i < BENCH_CPU_COUNT; i++)
        win_cat[i] += gBenchAcc[i];
    if (frame_gpu_valid) {
        win_gpu_frames++;
        win_gpu_total += frame_gpu_total;
        for (int i = 0; i < BENCH_GPU_COUNT; i++)
            win_gpu[i] += frame_gpu[i];
    }

    if (frame_demo && cur_demo != NULL) {
        struct DemoStat *d = cur_demo;
        d->frames++;
        if (frame_ticked)
            d->ticks++;
        d->sum += ft;
        if (ft > d->worst)
            d->worst = ft;
        for (int i = 0; i < BENCH_CPU_COUNT; i++)
            d->cat[i] += gBenchAcc[i];
        if (frame_gpu_valid) {
            d->gpu_frames++;
            d->gpu_total += frame_gpu_total;
            for (int i = 0; i < BENCH_GPU_COUNT; i++)
                d->gpu[i] += frame_gpu[i];
        }
        if (frame_spilled)
            d->spilled++;
        if (pass_nft < MAX_PASS_FRAMES)
            pass_ft[pass_nft++] = ft;
    }

    if ((double)(now - win_start) >= tb_hz * 0.5)
        close_window(now);
}

void bench_frame_begin(void) {
    if (!gBenchOn)
        return;
    uint32_t now = bench_tb();
    gBenchAcc[gBenchStack[gBenchSp]] += now - gBenchMark;

    if (have_frame)
        record_frame(now - frame_start, now);
    else
        win_start = now;

    memset(gBenchAcc, 0, sizeof(gBenchAcc));
    gBenchSp = 0;
    gBenchStack[0] = BENCH_OTHER;
    if (restart_frame_clock) {
        restart_frame_clock = false;
        now = bench_tb();
    }
    frame_start = now;
    gBenchMark = bench_tb(); // bookkeeping above counts as "Other"
    gBenchAcc[BENCH_OTHER] += gBenchMark - now;
    have_frame = true;

    frame_gpu_valid = false;
    frame_spilled = false;
    frame_demo = false;
    frame_ticked = false;
    if (configBenchmark)
        frame_split = split_for_pass(pass_running ? pass_no : pass_no + 1);
    else
        frame_split = split_for_play_window(win_no);
}

void bench_init(void) {
    gBenchOn = bench_wanted();
    if (!gBenchOn)
        return;
    field_hz = gfx_gx_wm_get_field_hz();
    gBenchSp = 0;
    gBenchStack[0] = BENCH_OTHER;
    gBenchMark = bench_tb();
}

#endif
