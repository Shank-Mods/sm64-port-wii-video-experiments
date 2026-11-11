#ifndef EARLY_CONSOLE_H
#define EARLY_CONSOLE_H

#include <stdbool.h>

#if defined(TARGET_GX) && defined(EARLYCON)
extern bool gEarlyVideoUp; // VIDEO_Init already done by the console
void early_console_init(void);
void early_console_line(const char *line);
void early_console_handover(void);
void early_console_free(void);
#else
#define gEarlyVideoUp false
#define early_console_init() ((void)0)
#define early_console_line(l) ((void)(l))
#define early_console_handover() ((void)0)
#define early_console_free() ((void)0)
#endif

#endif
