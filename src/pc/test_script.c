// Test scripts: start straight in a level and play scripted inputs, so a test bench run (or a
// manual test) can reach and exercise a specific place without anyone holding a controller.
//
//   test_level LEVEL [ACT]     start in this level (its folder name, e.g. bob, wf, castle_grounds,
//                              or its number), in act 1-6 (default 1), on save file 4; skips the
//                              title screen and file select
//   test_press T BUTTONS [N]   at T seconds, hold BUTTONS (A B Z START L R CU CD CL CR DU DD DL DR,
//                              joined with +) for N game frames (default 3)
//   test_stick T X Y [S]       at T seconds, hold the stick at X,Y (-80..80) for S seconds
//                              (default 0.5)
//
// T is in game seconds (30 game ticks a second, the same on any console speed or frame rate),
// counted from the moment the test level has loaded (or from boot without test_level).
// Up to 256 entries. The option lines may repeat; each adds an entry.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ultra64.h>
#include "level_table.h"
#include "test_script.h"
#include "configfile.h"
#include "wtb.h"

#define MAX_EVENTS 256

struct Event {
    unsigned start, end; // game ticks [start, end)
    u16 buttons;
    s8 x, y;
    bool stick;
};

static struct Event events[MAX_EVENTS];
static int nevents;
static int test_level = -1;   // -1: none
static int test_act = 1;
static bool level_pending;    // test_level set and not entered yet
static bool started;          // the clock runs (level loaded, or no test_level)
static unsigned ticks;        // game ticks since the clock started
static bool any;              // any test_ option given

static const struct { const char *name; int level; } levels[] = {
#define STUB_LEVEL(_0, _1, _2, _3, _4, _5, _6, _7, _8)
#define DEFINE_LEVEL(_0, levelenum, _2, folder, _4, _5, _6, _7, _8, _9, _10) { #folder, levelenum },
#include "levels/level_defines.h"
#undef STUB_LEVEL
#undef DEFINE_LEVEL
};

static const struct { const char *name; u16 bit; } buttons[] = {
    { "A", A_BUTTON }, { "B", B_BUTTON }, { "Z", Z_TRIG }, { "START", START_BUTTON },
    { "L", L_TRIG }, { "R", R_TRIG }, { "CU", U_CBUTTONS }, { "CD", D_CBUTTONS },
    { "CL", L_CBUTTONS }, { "CR", R_CBUTTONS }, { "DU", U_JPAD }, { "DD", D_JPAD },
    { "DL", L_JPAD }, { "DR", R_JPAD },
};

static u16 parse_buttons(const char *s) {
    u16 bits = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *t = strtok(buf, "+|,"); t != NULL; t = strtok(NULL, "+|,")) {
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (strcasecmp(t, buttons[i].name) == 0) {
                bits |= buttons[i].bit;
                break;
            }
        }
    }
    return bits;
}

static unsigned secs_to_ticks(double s) {
    return s <= 0 ? 0 : (unsigned)(s * 30.0 + 0.5);
}

bool test_script_option(const char *name, const char *value) {
    if (strncmp(name, "test_", 5) != 0)
        return false;
    any = true;
    if (strcmp(name, "test_level") == 0) {
        test_level = -1;
        char lv[32] = "";
        int act = 1;
        sscanf(value, "%31s %d", lv, &act);
        test_act = act >= 1 && act <= 6 ? act : 1;
        char *end;
        long n = strtol(lv, &end, 10);
        if (end != lv && *end == '\0') {
            test_level = (int)n;
        } else {
            for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++)
                if (strcasecmp(lv, levels[i].name) == 0)
                    test_level = levels[i].level;
        }
        if (test_level <= 0 || test_level > LEVEL_MAX) {
            printf("test_level: unknown level '%s'\n", value);
            test_level = -1;
        } else {
            level_pending = true;
            configBenchmark = false; // the attract demos would take over the input
        }
        return true;
    }
    if (nevents >= MAX_EVENTS)
        return true;
    struct Event *e = &events[nevents];
    memset(e, 0, sizeof(*e));
    if (strcmp(name, "test_press") == 0) {
        double t = 0;
        char b[64] = "";
        unsigned frames = 3;
        if (sscanf(value, "%lf %63s %u", &t, b, &frames) >= 2) {
            e->start = secs_to_ticks(t);
            e->end = e->start + (frames ? frames : 1);
            e->buttons = parse_buttons(b);
            nevents++;
        }
    } else if (strcmp(name, "test_stick") == 0) {
        double t = 0, s = 0.5;
        int x = 0, y = 0;
        if (sscanf(value, "%lf %d %d %lf", &t, &x, &y, &s) >= 3) {
            e->start = secs_to_ticks(t);
            e->end = e->start + (secs_to_ticks(s) ? secs_to_ticks(s) : 1);
            e->stick = true;
            e->x = (s8)(x < -80 ? -80 : x > 80 ? 80 : x);
            e->y = (s8)(y < -80 ? -80 : y > 80 ? 80 : y);
            nevents++;
        }
    } else {
        printf("unknown test option '%s'\n", name);
    }
    return true;
}

bool test_script_active(void) {
    return any;
}

int test_script_take_level(int *act) {
    if (!level_pending)
        return -1;
    level_pending = false;
    dbglog("test_level: entering level %d act %d", test_level, test_act);
    *act = test_act;
    return test_level;
}

bool test_script_level_wanted(void) {
    return level_pending;
}

bool test_script_entering(void) {
    return test_level > 0 && !level_pending && !started;
}

void test_script_level_loaded(void) {
    if (test_level > 0 && !started) {
        started = true;
        ticks = 0;
        wtb_mark("test level %d loaded: script clock starts", test_level);
        dbglog("test_level: level %d loaded, script clock starts", test_level);
    }
}

bool test_script_started(void) {
    return test_level <= 0 || started;
}

unsigned test_script_ticks(void) {
    return test_script_started() ? ticks : 0;
}

unsigned test_script_ms(void) {
    return test_script_ticks() * 1000u / 30u;
}

extern signed char gDebugLevelSelect;

void test_script_apply(unsigned short *button, signed char *stick_x, signed char *stick_y) {
    if (!any)
        return;
    if (!test_script_started())
        return;
    if (ticks == 0 && test_level > 0)
        gDebugLevelSelect = 0; // normal play from here (damage, deaths), as without the level select
    for (int i = 0; i < nevents; i++) {
        const struct Event *e = &events[i];
        if (ticks < e->start || ticks >= e->end)
            continue;
        *button |= e->buttons;
        if (e->stick) {
            *stick_x = e->x;
            *stick_y = e->y;
        }
    }
    ticks++;
}
