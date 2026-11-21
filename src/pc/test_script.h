#ifndef TEST_SCRIPT_H
#define TEST_SCRIPT_H

// Test scripts (test_level, test_press, test_stick): see test_script.c

#include <stdbool.h>

// A "test_..." config option (true if it was one; the value is the rest of the line)
bool test_script_option(const char *name, const char *value);
bool test_script_active(void);
// test_wing_light 1: show the castle lobby wing cap light beam whatever the save file says
bool test_script_wing_light(void);

// test_level: the title screen asks whether to go straight to a level; the level select takes it
bool test_script_level_wanted(void);
int test_script_take_level(int *act);
// Called when a level has loaded (init_level): starts the script clock for test_level
void test_script_level_loaded(void);
// Between taking test_level and its first load (the star select is skipped)
bool test_script_entering(void);

// The script clock: game ticks (30 a second) since it started
bool test_script_started(void);
unsigned test_script_ticks(void);
unsigned test_script_ms(void);

// Once per game tick, after the controllers were read: adds the scripted input
void test_script_apply(unsigned short *button, signed char *stick_x, signed char *stick_y);

#endif
