// configfile.c - handles loading and saving the configuration options
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#ifdef TARGET_GX
#include <fat.h>
#ifdef __wii__
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <wiiuse/wpad.h>
#include <ogc/conf.h>
#endif
#include <ogc/pad.h>
#endif

#include "configfile.h"
#include "early_console.h"
#include <unistd.h>
#include <stdarg.h>

static bool dbg_fat_ready;

// fatInitDefault() once; calling it a second time re-initialises the USB driver
static void fat_init_once(void)
{
#ifdef TARGET_GX
    if (!dbg_fat_ready) {
        early_console_line("mounting SD (fatInitDefault)...");
        bool ok = fatInitDefault();
        dbg_fat_ready = true;
#ifdef EARLYCON
        char msg[96];
        snprintf(msg, sizeof(msg), "fatInitDefault %s; cwd device: %s", ok ? "ok" : "FAILED (no card found)",
                 ok ? (getcwd(msg + 64, 32) != NULL ? msg + 64 : "?") : "none");
        early_console_line(msg);
#else
        (void)ok;
#endif
    }
#endif
}

// Debug log for hardware testing, compiled in only with -DDBGLOG: appends a line to
// sm64_debug.txt in the root of every mounted FAT device (SD and USB), closing the file each
// time so nothing is lost on a hang
void dbglog(const char *fmt, ...)
{
#if defined(TARGET_GX) && defined(DBGLOG)
    static bool header_done;
#ifndef DBGLOG_NAME
#define DBGLOG_NAME "sm64_debug.txt"
#endif
#ifdef __wii__
    static const char *const paths[] = { "sd:/" DBGLOG_NAME, "usb:/" DBGLOG_NAME };
#else
    // GameCube: SD2SP2 ("sd") or an SD Gecko in either memory card slot
    static const char *const paths[] = { "sd:/" DBGLOG_NAME, "carda:/" DBGLOG_NAME, "cardb:/" DBGLOG_NAME };
#endif
    char line[256];
    va_list ap;

    fat_init_once();

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    early_console_line(line);

    for (unsigned int i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], header_done ? "a" : "w");
        if (f != NULL) {
            fputs(line, f);
            fputs("\n", f);
            fclose(f);
        }
    }
    header_done = true;
#else
    (void)fmt;
#endif
}

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(arr[0]))

enum ConfigOptionType {
    CONFIG_TYPE_BOOL,
    CONFIG_TYPE_UINT,
    CONFIG_TYPE_FLOAT,
    CONFIG_TYPE_WIDESCREEN, // off / on / auto / pillarbox
    CONFIG_TYPE_VIDEOMODE,  // 720p / default
    CONFIG_TYPE_ASPECT,     // auto / 4:3 / 16:9 / square
    CONFIG_TYPE_SCAN,       // p / i
    CONFIG_TYPE_FIELDMODE,  // field / frame
    CONFIG_TYPE_PACING,     // vsync / realtime
    CONFIG_TYPE_CROPIL,     // 960i / 480p
};

struct ConfigOption {
    const char *name;
    enum ConfigOptionType type;
    union {
        bool *boolValue;
        unsigned int *uintValue;
        float *floatValue;
    };
};

/*
 *Config options and default values
 */
bool configFullscreen            = false;
bool config60Fps                 = true; // 60fps interpolation mode
bool configWidescreen            = false; // resolved at startup from configWidescreenMode
unsigned int configWidescreenMode = WIDESCREEN_AUTO; // anamorphic 16:9
bool configPillarbox             = false; // render 4:3 into the centre 3/4 so a 16:9 display's stretch restores correct 4:3
bool config240p                  = false; // Output a true 240p signal instead of 480i/480p
bool configHires1280             = false; // 1280x240p (4:3) output, rendered as two 640x240 tiles
#ifdef __wii__
unsigned int configVideoMode     = VIDEO_MODE_720P; // 1280x720p output (takes priority over 240p/1280x240p)
#else
unsigned int configVideoMode     = VIDEO_MODE_1920X1020P; // GameCube: rendered at 1920x1020, top left shown as 480p
#endif
struct VideoCustom configVideoCustom = { 1280, 720, 60.0f, false };
static unsigned int cfgVideoWidth = 1280, cfgVideoHeight = 720; // video_width / video_height
static float cfgVideoRefresh = 60.0f;                            // video_refresh
static bool cfgVideoInterlaced = false;                          // video_scan i
static bool cfgVideoFromString = false; // video_mode gave the size itself ("1280x720p120")
unsigned int configVideoAspect   = VIDEO_ASPECT_AUTO;
float configVideoAspectRatio     = 16.0f / 10.0f; // video_aspect W:H
static unsigned int cfgAspectW = 16, cfgAspectH = 10; // as written, for saving
bool configInterlaceField        = true;  // interlaced modes: field rendering (false: whole frames)
float configVideoMaxPclk         = 125.0f; // MHz (124.2 is the fastest proven on hardware)
unsigned int configCropInterlaced = CROP_IL_BOB; // crop of an interlaced mode: bob 480p / 960i / plain 480p
bool configCrop480               = false; // render at video_mode's size, output the top left 640x480 as normal 480p
bool configOptPsVertex           = true;
bool configOptNativeTex          = true;
bool configOptTexHash            = true;
bool configOptStateCache         = true;
bool configOptTileCull           = true;
bool configN64Controller         = true;  // N64 controllers on the GameCube ports (passive adapter)
unsigned int configDebugFrameDelayMs = 0; // DBGLOG builds: extra CPU time per frame (simulated load)
bool configFramePacingRealtime   = false; // frame_pacing: vsync (game speed follows the display) / realtime
bool configBenchRender           = false; // bench_uncapped render: 30 Hz logic, frames drawn as fast as possible
bool configOptSkyLast            = true;
bool configOptHwFog              = true;
bool configOptTriBin             = true;
bool configJrbMist               = true;
bool configOptMem2Gpu            = true;
bool configOptGpuTnl             = false; // slower on hardware in the tiled modes (user benchmarks)
bool configAntialias             = false; // (NOTE: Only used in 240p mode) Selects the antialiased video mode variant
bool configInvertCamera          = false; // Invert camera controls
bool configRumble                = true;  // Shindou controller rumble
bool configFog                   = true;  // Distance fog (turning it off also extends the level draw distance)
bool configForceNearest          = false; // Force GX_NEAR (nearest-neighbour) texture filtering instead of bilinear
bool configViDeflicker           = true;  // Apply the VI deflicker filter
bool configPuppycam              = false; // Use the Puppycam analogue camera instead of the original camera
bool configBenchmark             = true;  // bench_demo: timedemo, plays the attract-mode demos in a loop, input ignored
bool configBenchOverlay          = true;  // Frame time overlay (in demo mode and in normal play)
bool configBenchUncapped         = true;  // One game tick per rendered frame, no vsync wait: game logic runs fast too
unsigned int configBenchSplit    = 2;     // GPU timing passes: 0 never, 1 always, 2 alternate passes
unsigned int puppycam_sensitivityX = 75;
unsigned int puppycam_sensitivityY = 75;
unsigned int puppycam_invertX      = 0;
unsigned int puppycam_invertY      = 0;
unsigned int puppycam_degrade      = 10;  // How quickly the camera slows down after letting go
unsigned int puppycam_aggression   = 0;   // How aggressively the camera re-centres behind Mario
unsigned int puppycam_panlevel     = 75;
#ifdef __wii__
unsigned int configStorageDevice   = STORAGE_DEVICE_SD; // SD or USB but prefer SD
#endif
#ifndef TARGET_GX
// Keyboard mappings (scancode values)
unsigned int configKeyA          = 0x26;
unsigned int configKeyB          = 0x33;
unsigned int configKeyStart      = 0x39;
unsigned int configKeyR          = 0x36;
unsigned int configKeyZ          = 0x25;
unsigned int configKeyCUp        = 0x148;
unsigned int configKeyCDown      = 0x150;
unsigned int configKeyCLeft      = 0x14B;
unsigned int configKeyCRight     = 0x14D;
unsigned int configKeyStickUp    = 0x11;
unsigned int configKeyStickDown  = 0x1F;
unsigned int configKeyStickLeft  = 0x1E;
unsigned int configKeyStickRight = 0x20;
#else
#ifdef __wii__
unsigned int configKeyA          = WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A;
unsigned int configKeyB          = WPAD_BUTTON_B | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_B;
unsigned int configKeyStart      = WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_MINUS;
unsigned int configKeyR          = WPAD_NUNCHUK_BUTTON_C | WPAD_CLASSIC_BUTTON_FULL_R;
unsigned int configKeyZ          = WPAD_BUTTON_1 | WPAD_NUNCHUK_BUTTON_Z | WPAD_CLASSIC_BUTTON_FULL_L;
unsigned int configKeyCUp        = WPAD_BUTTON_UP;    /* cannot use WPAD_CLASSIC_BUTTON_UP as this clashes with WPAD_NUNCHUK_BUTTON_Z */
unsigned int configKeyCDown      = WPAD_BUTTON_DOWN;
unsigned int configKeyCLeft      = WPAD_BUTTON_LEFT;  /* cannot use WPAD_CLASSIC_BUTTON_LEFT as this clashes with WPAD_NUNCHUK_BUTTON_C */
unsigned int configKeyCRight     = WPAD_BUTTON_RIGHT;
unsigned int configKeyStickUp    = 0;
unsigned int configKeyStickDown  = 0;
unsigned int configKeyStickLeft  = 0;
unsigned int configKeyStickRight = 0;
#else
unsigned int configKeyA          = PAD_BUTTON_A;
unsigned int configKeyB          = PAD_BUTTON_B;
unsigned int configKeyStart      = PAD_BUTTON_START;
unsigned int configKeyL          = PAD_TRIGGER_L;
unsigned int configKeyR          = PAD_TRIGGER_R;
unsigned int configKeyZ          = PAD_TRIGGER_Z;
unsigned int configKeyCUp        = PAD_BUTTON_UP;
unsigned int configKeyCDown      = PAD_BUTTON_DOWN;
unsigned int configKeyCLeft      = PAD_BUTTON_LEFT;
unsigned int configKeyCRight     = PAD_BUTTON_RIGHT;
unsigned int configKeyStickUp    = 0;
unsigned int configKeyStickDown  = 0;
unsigned int configKeyStickLeft  = 0;
unsigned int configKeyStickRight = 0;
#endif
#endif

static const struct ConfigOption options[] = {
    {.name = "fullscreen",     .type = CONFIG_TYPE_BOOL, .boolValue = &configFullscreen},
    {.name = "60fps",          .type = CONFIG_TYPE_BOOL, .boolValue = &config60Fps},
    {.name = "widescreen",     .type = CONFIG_TYPE_WIDESCREEN, .uintValue = &configWidescreenMode},
    {.name = "240p",           .type = CONFIG_TYPE_BOOL, .boolValue = &config240p},
    {.name = "hires_1280x240p", .type = CONFIG_TYPE_BOOL, .boolValue = &configHires1280},
    {.name = "video_mode",     .type = CONFIG_TYPE_VIDEOMODE, .uintValue = &configVideoMode},
    {.name = "video_width",    .type = CONFIG_TYPE_UINT, .uintValue = &cfgVideoWidth},
    {.name = "video_height",   .type = CONFIG_TYPE_UINT, .uintValue = &cfgVideoHeight},
    {.name = "video_refresh",  .type = CONFIG_TYPE_FLOAT, .floatValue = &cfgVideoRefresh},
    {.name = "video_scan",     .type = CONFIG_TYPE_SCAN, .boolValue = &cfgVideoInterlaced},
    {.name = "video_aspect",   .type = CONFIG_TYPE_ASPECT, .uintValue = &configVideoAspect},
    {.name = "interlace_render", .type = CONFIG_TYPE_FIELDMODE, .boolValue = &configInterlaceField},
    {.name = "video_max_pclk", .type = CONFIG_TYPE_FLOAT, .floatValue = &configVideoMaxPclk},
    {.name = "crop_480p",      .type = CONFIG_TYPE_BOOL, .boolValue = &configCrop480},
    {.name = "crop_interlaced", .type = CONFIG_TYPE_CROPIL, .uintValue = &configCropInterlaced},
    {.name = "opt_ps_vertex",  .type = CONFIG_TYPE_BOOL, .boolValue = &configOptPsVertex},
    {.name = "opt_native_tex", .type = CONFIG_TYPE_BOOL, .boolValue = &configOptNativeTex},
    {.name = "opt_tex_hash",   .type = CONFIG_TYPE_BOOL, .boolValue = &configOptTexHash},
    {.name = "opt_state_cache", .type = CONFIG_TYPE_BOOL, .boolValue = &configOptStateCache},
    {.name = "opt_tile_cull",  .type = CONFIG_TYPE_BOOL, .boolValue = &configOptTileCull},
    {.name = "n64_controller", .type = CONFIG_TYPE_BOOL, .boolValue = &configN64Controller},
#ifdef DBGLOG
    {.name = "debug_frame_delay_ms", .type = CONFIG_TYPE_UINT, .uintValue = &configDebugFrameDelayMs},
#endif
    {.name = "frame_pacing",   .type = CONFIG_TYPE_PACING, .boolValue = &configFramePacingRealtime},
    {.name = "opt_sky_last",   .type = CONFIG_TYPE_BOOL, .boolValue = &configOptSkyLast},
    {.name = "opt_hw_fog",     .type = CONFIG_TYPE_BOOL, .boolValue = &configOptHwFog},
    {.name = "opt_tri_bin",    .type = CONFIG_TYPE_BOOL, .boolValue = &configOptTriBin},
    {.name = "jrb_mist",       .type = CONFIG_TYPE_BOOL, .boolValue = &configJrbMist},
    {.name = "opt_mem2_gpu",   .type = CONFIG_TYPE_BOOL, .boolValue = &configOptMem2Gpu},
    {.name = "opt_gpu_tnl",    .type = CONFIG_TYPE_BOOL, .boolValue = &configOptGpuTnl},
    {.name = "antialias",      .type = CONFIG_TYPE_BOOL, .boolValue = &configAntialias},
    {.name = "invert_camera",  .type = CONFIG_TYPE_BOOL, .boolValue = &configInvertCamera},
    {.name = "rumble",         .type = CONFIG_TYPE_BOOL, .boolValue = &configRumble},
    {.name = "fog",            .type = CONFIG_TYPE_BOOL, .boolValue = &configFog},
    {.name = "force_nearest",  .type = CONFIG_TYPE_BOOL, .boolValue = &configForceNearest},
    {.name = "vi_deflicker",   .type = CONFIG_TYPE_BOOL, .boolValue = &configViDeflicker},
    {.name = "puppycam",       .type = CONFIG_TYPE_BOOL, .boolValue = &configPuppycam},
    {.name = "bench_demo",     .type = CONFIG_TYPE_BOOL, .boolValue = &configBenchmark},
    {.name = "bench_uncapped", .type = CONFIG_TYPE_BOOL, .boolValue = &configBenchUncapped},
    {.name = "bench_overlay",  .type = CONFIG_TYPE_BOOL, .boolValue = &configBenchOverlay},
    {.name = "bench_gpu_split", .type = CONFIG_TYPE_UINT, .uintValue = &configBenchSplit},
    {.name = "puppycam_sensitivity_x",  .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_sensitivityX},
    {.name = "puppycam_sensitivity_y",  .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_sensitivityY},
    {.name = "puppycam_invert_x",       .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_invertX},
    {.name = "puppycam_invert_y",       .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_invertY},
    {.name = "puppycam_stopping_speed", .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_degrade},
    {.name = "puppycam_centre_aggression", .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_aggression},
    {.name = "puppycam_pan_amount",     .type = CONFIG_TYPE_UINT, .uintValue = &puppycam_panlevel},
    {.name = "key_a",          .type = CONFIG_TYPE_UINT, .uintValue = &configKeyA},
    {.name = "key_b",          .type = CONFIG_TYPE_UINT, .uintValue = &configKeyB},
    {.name = "key_start",      .type = CONFIG_TYPE_UINT, .uintValue = &configKeyStart},
#ifdef __gamecube__
    {.name = "key_l",          .type = CONFIG_TYPE_UINT, .uintValue = &configKeyL},
#endif
    {.name = "key_z",          .type = CONFIG_TYPE_UINT, .uintValue = &configKeyZ},
    {.name = "key_cup",        .type = CONFIG_TYPE_UINT, .uintValue = &configKeyCUp},
    {.name = "key_cdown",      .type = CONFIG_TYPE_UINT, .uintValue = &configKeyCDown},
    {.name = "key_cleft",      .type = CONFIG_TYPE_UINT, .uintValue = &configKeyCLeft},
    {.name = "key_cright",     .type = CONFIG_TYPE_UINT, .uintValue = &configKeyCRight},
#ifndef TARGET_GX
    {.name = "key_stickup",    .type = CONFIG_TYPE_UINT, .uintValue = &configKeyStickUp},
    {.name = "key_stickdown",  .type = CONFIG_TYPE_UINT, .uintValue = &configKeyStickDown},
    {.name = "key_stickleft",  .type = CONFIG_TYPE_UINT, .uintValue = &configKeyStickLeft},
    {.name = "key_stickright", .type = CONFIG_TYPE_UINT, .uintValue = &configKeyStickRight},
#endif
};

// Reads an entire line from a file (excluding the newline character) and returns an allocated string
// Returns NULL if no lines could be read from the file
static char *read_file_line(FILE *file) {
    char *buffer;
    size_t bufferSize = 8;
    size_t offset = 0; // offset in buffer to write

    buffer = malloc(bufferSize);
    while (1) {
        // Read a line from the file
        if (fgets(buffer + offset, bufferSize - offset, file) == NULL) {
            free(buffer);
            return NULL; // Nothing could be read.
        }
        offset = strlen(buffer);
        assert(offset > 0);

        // If a newline was found, remove the trailing newline and exit
        if (buffer[offset - 1] == '\n') {
            buffer[offset - 1] = '\0';
            break;
        }

        if (feof(file)) // EOF was reached
            break;

        // If no newline or EOF was reached, then the whole line wasn't read.
        bufferSize *= 2; // Increase buffer size
        buffer = realloc(buffer, bufferSize);
        assert(buffer != NULL);
    }

    return buffer;
}

// Returns the position of the first non-whitespace character
static char *skip_whitespace(char *str) {
    while (isspace(*str))
        str++;
    return str;
}

// NULL-terminates the current whitespace-delimited word, and returns a pointer to the next word
static char *word_split(char *str) {
    // Precondition: str must not point to whitespace
    assert(!isspace(*str));

    // Find either the next whitespace char or end of string
    while (!isspace(*str) && *str != '\0')
        str++;
    if (*str == '\0') // End of string
        return str;

    // Terminate current word
    *(str++) = '\0';

    // Skip whitespace to next word
    return skip_whitespace(str);
}

// Splits a string into words, and stores the words into the 'tokens' array
// 'maxTokens' is the length of the 'tokens' array
// Returns the number of tokens parsed
static unsigned int tokenize_string(char *str, int maxTokens, char **tokens) {
    int count = 0;

    str = skip_whitespace(str);
    while (str[0] != '\0' && count < maxTokens) {
        tokens[count] = str;
        str = word_split(str);
        count++;
    }
    return count;
}

#ifdef __wii__
static const char *device_root(unsigned int device) {
    return (device == STORAGE_DEVICE_USB) ? "usb:" : "sd:";
}

static void build_storage_path(char *buf, size_t size, unsigned int device, const char *filename) {
    snprintf(buf, size, "%s/apps/sm64/%s", device_root(device), filename);
}

// True if the device ("sd:" or "usb:") was mounted by fatInitDefault()
static bool storage_device_mounted(unsigned int device) {
    return FindDevice(device_root(device)) >= 0;
}

static void ensure_storage_dir(unsigned int device) {
    char dir[64];
    // mkdir() on a device that isn't mounted crashes with some newlib versions
    if (!storage_device_mounted(device))
        return;
    snprintf(dir, sizeof(dir), "%s/apps", device_root(device));
    mkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%s/apps/sm64", device_root(device));
    mkdir(dir, 0777);
}

const char *get_storage_path(const char *filename) {
    static char path[64];
    ensure_storage_dir(configStorageDevice);
    build_storage_path(path, sizeof(path), configStorageDevice, filename);
    return path;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (in == NULL)
        return false;
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

void configfile_switch_storage_device(unsigned int newDevice) {
    if (newDevice == configStorageDevice)
        return;

    unsigned int oldDevice = configStorageDevice;

    // Bail out if the new device isn't writable
    ensure_storage_dir(newDevice);
    char testPath[64];
    build_storage_path(testPath, sizeof(testPath), newDevice, ".sm64_write_test");
    FILE *test = fopen(testPath, "wb");
    if (test == NULL)
        return;
    fclose(test);
    remove(testPath);

    static const char *const files[] = { CONFIG_FILE, SAVE_FILE };
    for (unsigned int i = 0; i < ARRAY_LEN(files); i++) {
        char src[64], dst[64];
        build_storage_path(src, sizeof(src), oldDevice, files[i]);
        build_storage_path(dst, sizeof(dst), newDevice, files[i]);
        if (copy_file(src, dst))
            remove(src);
    }

    configStorageDevice = newDevice;
}
#endif

// Loads the config file specified by 'filename'
// "1280x720p120", "1920x1080i" (60 Hz when no rate is given), "240p360" (640 wide), "maxi"
static bool parse_video_mode_string(const char *str, struct VideoCustom *out) {
    unsigned int w = 0, h = 0;
    float rate = 60.0f;
    char scan = 0;
    int n = 0;
    if (strcmp(str, "maxi") == 0) {
        // The tallest interlaced frame the VI's field length allows, at 60 fields a second
        str = "1920x2040i60";
    }
    if (sscanf(str, "%ux%u%c%n", &w, &h, &scan, &n) >= 3) {
    } else if (sscanf(str, "%u%c%n", &h, &scan, &n) >= 2) {
        w = h <= 480 ? 640 : (h * 16 / 9 + 15) / 16 * 16;
    } else {
        return false;
    }
    if (scan != 'p' && scan != 'i')
        return false;
    if (str[n] != '\0' && sscanf(str + n, "%f", &rate) != 1)
        return false;
    if (w == 0 || h == 0 || rate <= 0.0f)
        return false;
    out->width = w;
    out->height = h;
    out->refresh = rate;
    out->interlaced = scan == 'i';
    return true;
}

static const char *custom_mode_string(void) {
    static char buf[32];
    if (!cfgVideoFromString)
        return "custom";
    snprintf(buf, sizeof(buf), "%ux%u%c%g", configVideoCustom.width, configVideoCustom.height,
             configVideoCustom.interlaced ? 'i' : 'p', (double)configVideoCustom.refresh);
    return buf;
}

// Game ticks on a real-time 30 Hz clock with frames interpolated to the moment they're drawn:
// bench_uncapped render (frames as fast as possible), or frame_pacing realtime (frames at
// most one per refresh; when rendering falls behind, frames drop instead of the game slowing)
bool config_rt_pacing(void) {
    return configBenchRender || (configFramePacingRealtime && !configBenchUncapped);
}

// Display aspect of a w x h mode under video_aspect
float configfile_video_dar(unsigned int w, unsigned int h) {
    const float sq = h > 0 ? (float)w / (float)h : 4.0f / 3.0f;
    switch (configVideoAspect) {
        case VIDEO_ASPECT_4_3: return 4.0f / 3.0f;
        case VIDEO_ASPECT_16_9: return 16.0f / 9.0f;
        case VIDEO_ASPECT_SQUARE: return sq;
        case VIDEO_ASPECT_RATIO: return configVideoAspectRatio;
        default:
            // Square pixels when that makes a normal picture shape (1280x720, 1920x1080,
            // 1440x1080, 1920x1440), else 4:3 (1440x240, 1440x480, 640x240)
            return (sq >= 1.25f && sq <= 1.9f) ? sq : 4.0f / 3.0f;
    }
}

void configfile_load(const char *filename) {
    const char *path = filename;
#ifdef __wii__
    fat_init_once();
    dbglog("config: fat ready");
    // ALways prefer SD
    configStorageDevice = STORAGE_DEVICE_SD;
    char probe[64];
    build_storage_path(probe, sizeof(probe), STORAGE_DEVICE_SD, filename);
    FILE *probeFile = fopen(probe, "r");
    dbglog("config: sd probe %s", probeFile ? "found" : "none");
    if (probeFile != NULL) {
        fclose(probeFile);
    } else {
        build_storage_path(probe, sizeof(probe), STORAGE_DEVICE_USB, filename);
        probeFile = fopen(probe, "r");
        dbglog("config: usb probe %s", probeFile ? "found" : "none");
        if (probeFile != NULL) {
            fclose(probeFile);
            configStorageDevice = STORAGE_DEVICE_USB;
        } else if (!storage_device_mounted(STORAGE_DEVICE_SD)
                   && storage_device_mounted(STORAGE_DEVICE_USB)) {
            // No SD card: keep the config and save on the USB drive
            configStorageDevice = STORAGE_DEVICE_USB;
        }
    }
    dbglog("config: mounted sd=%d usb=%d", storage_device_mounted(STORAGE_DEVICE_SD),
           storage_device_mounted(STORAGE_DEVICE_USB));
    path = get_storage_path(filename);
    dbglog("config: path %s", path);
#elif defined(TARGET_GX)
    fat_init_once();
#endif
    FILE *file;
    char *line;

    printf("Loading configuration from '%s'\n", path);

    file = fopen(path, "r");
    if (file == NULL) {
        // Create a new config file and save defaults
        printf("Config file '%s' not found. Creating it.\n", path);
        dbglog("config: not found, saving defaults");
        configfile_save(filename);
        dbglog("config: defaults saved");
        return;
    }

    // Go through each line in the file
    while ((line = read_file_line(file)) != NULL) {
        char *p = line;
        char *tokens[2];
        int numTokens;

        while (isspace(*p))
            p++;
        numTokens = tokenize_string(p, 2, tokens);
        if (numTokens != 0) {
            if (numTokens == 2) {
                const struct ConfigOption *option = NULL;

                // "benchmark" was the name of bench_demo on the wii-benchmark branch
                if (strcmp(tokens[0], "benchmark") == 0)
                    tokens[0] = "bench_demo";
                for (unsigned int i = 0; i < ARRAY_LEN(options); i++) {
                    if (strcmp(tokens[0], options[i].name) == 0) {
                        option = &options[i];
                        break;
                    }
                }
                if (option == NULL)
                    printf("unknown option '%s'\n", tokens[0]);
                else {
                    switch (option->type) {
                        case CONFIG_TYPE_BOOL:
                            if (option->boolValue == &configBenchUncapped) {
                                // bench_uncapped takes a third value, "render"
                                configBenchRender = strcmp(tokens[1], "render") == 0;
                                if (configBenchRender) {
                                    configBenchUncapped = false;
                                    break;
                                }
                            }
                            if (strcmp(tokens[1], "true") == 0)
                                *option->boolValue = true;
                            else if (strcmp(tokens[1], "false") == 0)
                                *option->boolValue = false;
                            break;
                        case CONFIG_TYPE_UINT:
                            sscanf(tokens[1], "%u", option->uintValue);
                            break;
                        case CONFIG_TYPE_FLOAT:
                            sscanf(tokens[1], "%f", option->floatValue);
                            break;
                        case CONFIG_TYPE_WIDESCREEN:
                            if (strcmp(tokens[1], "auto") == 0)
                                *option->uintValue = WIDESCREEN_AUTO;
                            else if (strcmp(tokens[1], "pillarbox") == 0)
                                *option->uintValue = WIDESCREEN_PILLARBOX;
                            else if (strcmp(tokens[1], "on") == 0 || strcmp(tokens[1], "true") == 0)
                                *option->uintValue = WIDESCREEN_ON;
                            else if (strcmp(tokens[1], "off") == 0 || strcmp(tokens[1], "false") == 0)
                                *option->uintValue = WIDESCREEN_OFF;
                            break;
                        case CONFIG_TYPE_VIDEOMODE:
                            if (strcmp(tokens[1], "720p") == 0 || strcmp(tokens[1], "1280x720p") == 0)
                                *option->uintValue = VIDEO_MODE_720P;
                            else if (strcmp(tokens[1], "960x720p") == 0)
                                *option->uintValue = VIDEO_MODE_960X720P;
                            else if (strcmp(tokens[1], "1280x960crop") == 0)
                                *option->uintValue = VIDEO_MODE_960P_CROP;
                            else if (strcmp(tokens[1], "1280x960p") == 0)
                                *option->uintValue = VIDEO_MODE_1280X960P;
                            else if (strcmp(tokens[1], "1920x1020p") == 0 || strcmp(tokens[1], "maxp") == 0)
                                *option->uintValue = VIDEO_MODE_1920X1020P;
                            else if (strcmp(tokens[1], "1920x1440i") == 0)
                                *option->uintValue = VIDEO_MODE_1920X1440I;
                            else if (strcmp(tokens[1], "default") == 0 || strcmp(tokens[1], "480p") == 0
                                     || strcmp(tokens[1], "480i") == 0 || strcmp(tokens[1], "auto") == 0)
                                *option->uintValue = VIDEO_MODE_DEFAULT;
                            else if (strcmp(tokens[1], "custom") == 0) {
                                *option->uintValue = VIDEO_MODE_CUSTOM;
                                cfgVideoFromString = false;
                            } else if (parse_video_mode_string(tokens[1], &configVideoCustom)) {
                                *option->uintValue = VIDEO_MODE_CUSTOM;
                                cfgVideoFromString = true;
                            } else
                                printf("unknown video_mode '%s'\n", tokens[1]);
                            break;
                        case CONFIG_TYPE_ASPECT:
                            if (strcmp(tokens[1], "4:3") == 0)
                                *option->uintValue = VIDEO_ASPECT_4_3;
                            else if (strcmp(tokens[1], "16:9") == 0)
                                *option->uintValue = VIDEO_ASPECT_16_9;
                            else if (strcmp(tokens[1], "square") == 0)
                                *option->uintValue = VIDEO_ASPECT_SQUARE;
                            else if (strcmp(tokens[1], "auto") == 0)
                                *option->uintValue = VIDEO_ASPECT_AUTO;
                            else {
                                // Any W:H
                                unsigned int aw = 0, ah = 0;
                                if (sscanf(tokens[1], "%u:%u", &aw, &ah) == 2 && aw > 0 && ah > 0
                                    && (float)aw / ah >= 0.5f && (float)aw / ah <= 4.0f) {
                                    *option->uintValue = VIDEO_ASPECT_RATIO;
                                    configVideoAspectRatio = (float)aw / (float)ah;
                                    cfgAspectW = aw;
                                    cfgAspectH = ah;
                                } else
                                    printf("unknown video_aspect '%s'\n", tokens[1]);
                            }
                            break;
                        case CONFIG_TYPE_SCAN:
                            if (strcmp(tokens[1], "i") == 0 || strcmp(tokens[1], "interlaced") == 0)
                                *option->boolValue = true;
                            else if (strcmp(tokens[1], "p") == 0 || strcmp(tokens[1], "progressive") == 0)
                                *option->boolValue = false;
                            break;
                        case CONFIG_TYPE_CROPIL:
                            if (strcmp(tokens[1], "960i") == 0)
                                *option->uintValue = CROP_IL_960I;
                            else if (strcmp(tokens[1], "bob") == 0)
                                *option->uintValue = CROP_IL_BOB;
                            else if (strcmp(tokens[1], "480p") == 0)
                                *option->uintValue = CROP_IL_480P;
                            break;
                        case CONFIG_TYPE_PACING:
                            if (strcmp(tokens[1], "realtime") == 0)
                                *option->boolValue = true;
                            else if (strcmp(tokens[1], "vsync") == 0)
                                *option->boolValue = false;
                            break;
                        case CONFIG_TYPE_FIELDMODE:
                            if (strcmp(tokens[1], "field") == 0)
                                *option->boolValue = true;
                            else if (strcmp(tokens[1], "frame") == 0)
                                *option->boolValue = false;
                            break;
                        default:
                            assert(0); // bad type
                    }
                    printf("option: '%s', value: '%s'\n", tokens[0], tokens[1]);
                }
            } else
                puts("error: expected value");
        }
        free(line);
    }

    fclose(file);
    if (configVideoMode == VIDEO_MODE_CUSTOM && !cfgVideoFromString) {
        configVideoCustom.width = cfgVideoWidth;
        configVideoCustom.height = cfgVideoHeight;
        configVideoCustom.refresh = cfgVideoRefresh;
        configVideoCustom.interlaced = cfgVideoInterlaced;
    }
}

bool configfile_console_is_widescreen(void) {
#if defined(TARGET_GX) && defined(__wii__)
    return CONF_GetAspectRatio() == CONF_ASPECT_16_9;
#else
    return false;
#endif
}

void configfile_resolve_widescreen(void) {
#ifdef TARGET_GX
    configPillarbox = false;
    switch (configWidescreenMode) {
        case WIDESCREEN_ON:
            configWidescreen = true;
            break;
        case WIDESCREEN_PILLARBOX:
            configWidescreen = false;
            configPillarbox = true;
            break;
        case WIDESCREEN_OFF:
            configWidescreen = false;
            break;
        case WIDESCREEN_AUTO:
        default:
            // 1280x720p is a 16:9 mode, 960x720p and 1280x240p are 4:3 ones, so "auto" follows
            // those modes rather than the console setting
            if (configVideoMode == VIDEO_MODE_CUSTOM)
                configWidescreen = configfile_video_dar(configVideoCustom.width, configVideoCustom.height) > 1.5f;
            else if (configVideoAspect != VIDEO_ASPECT_AUTO && configVideoMode != VIDEO_MODE_DEFAULT)
                configWidescreen = configVideoAspect == VIDEO_ASPECT_16_9
                                   || (configVideoAspect == VIDEO_ASPECT_RATIO && configVideoAspectRatio > 1.5f)
                                   || (configVideoAspect == VIDEO_ASPECT_SQUARE
                                       && (configVideoMode == VIDEO_MODE_720P || configVideoMode == VIDEO_MODE_1920X1020P));
            else if (configVideoMode == VIDEO_MODE_720P || configVideoMode == VIDEO_MODE_1920X1020P)
                configWidescreen = true;
            else if (configVideoMode == VIDEO_MODE_960X720P || configVideoMode == VIDEO_MODE_960P_CROP
                     || configVideoMode == VIDEO_MODE_1280X960P || configVideoMode == VIDEO_MODE_1920X1440I)
                configWidescreen = false;
            else
                configWidescreen = configHires1280 ? false : configfile_console_is_widescreen();
            break;
    }
#endif
}

// Writes the config file to 'filename'
void configfile_save(const char *filename) {
    const char *path = filename;
#ifdef __wii__
    fat_init_once();
    path = get_storage_path(filename);
#elif defined(TARGET_GX)
    fat_init_once();
#endif
    FILE *file;

    printf("Saving configuration to '%s'\n", path);

    file = fopen(path, "w");
    if (file == NULL) {
        // error
        return;
    }

    for (unsigned int i = 0; i < ARRAY_LEN(options); i++) {
        const struct ConfigOption *option = &options[i];

        switch (option->type) {
            case CONFIG_TYPE_BOOL:
                if (option->boolValue == &configBenchUncapped && configBenchRender)
                    fprintf(file, "%s render\n", option->name);
                else
                    fprintf(file, "%s %s\n", option->name, *option->boolValue ? "true" : "false");
                break;
            case CONFIG_TYPE_UINT:
                fprintf(file, "%s %u\n", option->name, *option->uintValue);
                break;
            case CONFIG_TYPE_FLOAT:
                fprintf(file, "%s %f\n", option->name, *option->floatValue);
                break;
            case CONFIG_TYPE_WIDESCREEN:
                fprintf(file, "%s %s\n", option->name,
                        *option->uintValue == WIDESCREEN_AUTO ? "auto" :
                        *option->uintValue == WIDESCREEN_PILLARBOX ? "pillarbox" :
                        *option->uintValue == WIDESCREEN_ON ? "on" : "off");
                break;
            case CONFIG_TYPE_VIDEOMODE:
                fprintf(file, "%s %s\n", option->name,
                        *option->uintValue == VIDEO_MODE_720P ? "720p" :
                        *option->uintValue == VIDEO_MODE_960X720P ? "960x720p" :
                        *option->uintValue == VIDEO_MODE_960P_CROP ? "1280x960crop" :
                        *option->uintValue == VIDEO_MODE_1280X960P ? "1280x960p" :
                        *option->uintValue == VIDEO_MODE_1920X1020P ? "1920x1020p" :
                        *option->uintValue == VIDEO_MODE_1920X1440I ? "1920x1440i" :
                        *option->uintValue == VIDEO_MODE_CUSTOM ? custom_mode_string() : "default");
                break;
            case CONFIG_TYPE_ASPECT:
                if (*option->uintValue == VIDEO_ASPECT_RATIO) {
                    fprintf(file, "%s %u:%u\n", option->name, cfgAspectW, cfgAspectH);
                    break;
                }
                fprintf(file, "%s %s\n", option->name,
                        *option->uintValue == VIDEO_ASPECT_4_3 ? "4:3" :
                        *option->uintValue == VIDEO_ASPECT_16_9 ? "16:9" :
                        *option->uintValue == VIDEO_ASPECT_SQUARE ? "square" : "auto");
                break;
            case CONFIG_TYPE_SCAN:
                fprintf(file, "%s %s\n", option->name, *option->boolValue ? "i" : "p");
                break;
            case CONFIG_TYPE_CROPIL:
                fprintf(file, "%s %s\n", option->name, *option->uintValue == CROP_IL_960I ? "960i"
                        : *option->uintValue == CROP_IL_BOB ? "bob" : "480p");
                break;
            case CONFIG_TYPE_PACING:
                fprintf(file, "%s %s\n", option->name, *option->boolValue ? "realtime" : "vsync");
                break;
            case CONFIG_TYPE_FIELDMODE:
                fprintf(file, "%s %s\n", option->name, *option->boolValue ? "field" : "frame");
                break;
            default:
                assert(0); // unknown type
        }
    }

    fclose(file);
}
