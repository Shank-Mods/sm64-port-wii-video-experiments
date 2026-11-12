#ifndef BENCH_H
#define BENCH_H

// Uncapped timedemo benchmark with a per-task CPU/GPU frame-time overlay.
//
// CPU time is split into tasks with a tiny push/pop stack: each push or pop reads the PowerPC
// timebase once and charges the time since the last mark to the task on top of the stack. The
// timebase is calibrated against the video interface's retrace interrupt, which keeps running
// from the video clock when the CPU/GPU are overclocked (the timebase itself runs at bus/4, so
// it speeds up with an overclock and can't be trusted on its own).

#include <stdbool.h>
#include <stdint.h>

enum BenchCpuTask {
    BENCH_OTHER,    // anything not covered below
    BENCH_LOGIC,    // level script: Mario, objects, collision, camera, sound requests
    BENCH_SCENE,    // geo graph walk and display list building (render_game)
    BENCH_AUDIO,    // audio synthesis
    BENCH_DLIST,    // gfx_pc: display list decode, combiner and triangle setup
    BENCH_VERTEX,   // gfx_pc: software vertex transform, lighting, texgen, fog
    BENCH_TEXTURE,  // texture decode (N64 format -> RGBA32 -> RGB5A3)
    BENCH_SUBMIT,   // GX command emission
    BENCH_GPUWAIT,  // CPU blocked waiting for the GPU to finish the frame
    BENCH_PRESENT,  // XFB flip and end of frame
    BENCH_OVERLAY,  // this overlay
    BENCH_CPU_COUNT
};

enum BenchGpuTask {
    BENCH_GPU_WORLD,  // perspective (3D) draws
    BENCH_GPU_2D,     // orthographic draws: HUD, text, skybox, transitions
    BENCH_GPU_COPY,   // EFB -> XFB copy and clear
    BENCH_GPU_COUNT
};

// Overlay texture size (RGB5A3, 40 columns x 22 rows of 8x13 text)
#define BENCH_OV_W 320
#define BENCH_OV_H 292

#ifdef TARGET_GX

extern bool gBenchOn;
extern uint32_t gBenchMark;
extern int gBenchSp;
extern uint8_t gBenchStack[16];
extern uint32_t gBenchAcc[BENCH_CPU_COUNT];

// Lower 32 bits of the timebase; differences are valid for ~70 s at stock clocks
static inline uint32_t bench_tb(void) {
    uint32_t t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

uint64_t bench_tb64(void);

static inline void bench_push(int task) {
    if (!gBenchOn || gBenchSp >= 15)
        return;
    uint32_t now = bench_tb();
    gBenchAcc[gBenchStack[gBenchSp]] += now - gBenchMark;
    gBenchMark = now;
    gBenchStack[++gBenchSp] = (uint8_t)task;
}

static inline void bench_pop(void) {
    if (!gBenchOn || gBenchSp <= 0)
        return;
    uint32_t now = bench_tb();
    gBenchAcc[gBenchStack[gBenchSp]] += now - gBenchMark;
    gBenchMark = now;
    gBenchSp--;
}

// True when the instrumentation is on: demo mode, or the overlay in normal play
bool bench_wanted(void);

// Called by pc_main around each produced frame
void bench_init(void);
void bench_frame_begin(void);
void bench_tick_done(void);
void bench_frame_continue(void); // render-uncapped: a frame without a game tick
double bench_tb_hz(void);         // calibrated timebase rate

// Called from the GX renderer
bool bench_split_this_frame(void);     // run the GPU after the CPU has finished the frame
void bench_gpu_frame(bool valid, uint32_t total, const uint32_t task[BENCH_GPU_COUNT], bool spilled);
bool bench_overlay_enabled(void);
bool bench_overlay_update(uint16_t *tex, int width, int height); // true if the texture was redrawn

// Called from the VI retrace interrupt
void bench_on_retrace(bool new_frame_shown);

#else

#define bench_push(t) ((void)0)
#define bench_pop() ((void)0)

#endif

#endif
