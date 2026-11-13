#ifndef CONFIGFILE_H
#define CONFIGFILE_H

#include <stdbool.h>

#define CONFIG_FILE "sm64config.txt"

#ifdef __wii__
// Save is always located under /apps/sm64 now on Wii
#define SAVE_FILE "sm64_save_file.bin"

enum StorageDevice {
    STORAGE_DEVICE_SD  = 0,
    STORAGE_DEVICE_USB = 1,
};

extern unsigned int configStorageDevice;

const char *get_storage_path(const char *filename);

void configfile_switch_storage_device(unsigned int newDevice);
#endif

// Stored preference for configWidescreenMode
#define WIDESCREEN_OFF       0u
#define WIDESCREEN_ON        1u
#define WIDESCREEN_AUTO      2u // follow the console's system 16:9 setting on Wii only
#define WIDESCREEN_PILLARBOX 3u // Proper 4:3 on a 16:9 display. Doesn't work on some displays

// configVideoMode
#define VIDEO_MODE_DEFAULT 0u // the console's own mode (480p/480i), or 240p / 1280x240p if enabled
#define VIDEO_MODE_720P    1u // 1280x720p60, VI pixel clock reprogrammed to 148.5 MHz
#define VIDEO_MODE_960X720P 2u // 960x720p60 (4:3), VI pixel clock reprogrammed to 108 MHz
#define VIDEO_MODE_960P_CROP 3u // rendered at 1280x960, top left 640x480 shown in the normal 480p
#define VIDEO_MODE_1280X960P  4u // 1280x960p60 (4:3), VESA timing, 108 MHz pixels
#define VIDEO_MODE_1920X1020P 5u // 1920x1020p60 (square pixels), minimal vertical blanking
#define VIDEO_MODE_1920X1440I 6u // 1920x1440i60 (4:3), 720 lines per field
#define VIDEO_MODE_CUSTOM     7u // timing generated from a size, scan and refresh (see below)

// configVideoAspect: display aspect ratio of the high resolution modes
#define VIDEO_ASPECT_AUTO   0u // square pixels when the size is a sensible picture shape, else 4:3
#define VIDEO_ASPECT_4_3    1u
#define VIDEO_ASPECT_16_9   2u
#define VIDEO_ASPECT_SQUARE 3u // width / height
#define VIDEO_ASPECT_RATIO  4u // any W:H ("16:10", "21:9", "5:4"): configVideoAspectRatio
extern float configVideoAspectRatio;

// Custom video mode ("video_mode 1280x720p120", or "video_mode custom" with the video_* options)
struct VideoCustom {
    unsigned int width, height; // the whole frame (both fields when interlaced)
    float refresh;              // fields per second
    bool interlaced;
};
extern struct VideoCustom configVideoCustom; // resolved: what the custom mode uses
extern unsigned int configVideoAspect;
extern bool         configInterlaceField; // interlaced modes: render each field on its own
extern float        configVideoMaxPclk;   // MHz: the custom timing generator stays below this
float configfile_video_dar(unsigned int w, unsigned int h); // display aspect of a w x h mode

extern bool         configFullscreen;
extern bool         config60Fps;
extern bool         configWidescreen;
extern unsigned int configWidescreenMode;
extern bool         configPillarbox;
extern bool         config240p;
extern bool         configAntialias;
extern bool         configHires1280;
extern unsigned int configVideoMode;
extern bool         configCrop480;
// Performance options (wii-optimize branch); each can be turned off to compare or work around
extern bool         configOptPsVertex;   // paired-single vertex transform and lighting
extern bool         configOptNativeTex;  // N64 textures converted straight to native GX formats
extern bool         configOptTexHash;    // texture hashes computed once per frame, not per switch
extern bool         configOptStateCache; // skip reloading textures that are already loaded
extern bool         configOptTileCull;   // tiled modes: skip draws that miss a tile
extern bool         configN64Controller; // N64 controllers on the GameCube ports
extern bool         configBenchRender;   // bench_uncapped render: 30 Hz logic, frames uncapped and interpolated
extern bool         configOptSkyLast;    // skybox drawn after the opaque level, only where it shows
extern bool         configOptHwFog;      // CPU-path fog in GX's fog unit (no extra TEV stage)
extern bool         configOptTriBin;     // tiled modes: split draws into per-tile runs of triangles
extern bool         configJrbMist;       // Jolly Roger Bay's mist layer (visual; false skips it)
extern bool         configOptMem2Gpu;    // Wii: GPU-only buffers (textures, display lists) in MEM2
extern bool         configOptGpuTnl;     // GPU vertex transform, lighting and fog

void dbglog(const char *fmt, ...);
extern bool         configInvertCamera;
extern bool         configRumble;
extern bool         configFog;
extern bool         configForceNearest;
extern bool         configViDeflicker;
extern bool         configPuppycam;
extern bool         configBenchmark;
extern bool         configBenchOverlay;
extern bool         configBenchUncapped;
extern unsigned int configBenchSplit;
extern unsigned int puppycam_sensitivityX;
extern unsigned int puppycam_sensitivityY;
extern unsigned int puppycam_invertX;
extern unsigned int puppycam_invertY;
extern unsigned int puppycam_degrade;
extern unsigned int puppycam_aggression;
extern unsigned int puppycam_panlevel;
extern unsigned int configKeyA;
extern unsigned int configKeyB;
extern unsigned int configKeyStart;
#ifdef __gamecube__
extern unsigned int configKeyL;
#endif
extern unsigned int configKeyR;
extern unsigned int configKeyZ;
extern unsigned int configKeyCUp;
extern unsigned int configKeyCDown;
extern unsigned int configKeyCLeft;
extern unsigned int configKeyCRight;
extern unsigned int configKeyStickUp;
extern unsigned int configKeyStickDown;
extern unsigned int configKeyStickLeft;
extern unsigned int configKeyStickRight;

void configfile_load(const char *filename);
void configfile_save(const char *filename);
void configfile_resolve_widescreen(void);
bool configfile_console_is_widescreen(void);  // true when the console's system setting is 16:9 (Wii)

#endif
