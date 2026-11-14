#ifndef WTB_H
#define WTB_H

// Wii test bench client (protocol WTB/1, see Shank-Mods/PrivateTestWii PROTOCOL.md).
//
// A run is requested by the runner on the command line (wiiload passes arguments to the DOL),
// or for manual tests by a text file of the same lines on the SD card:
//
//   wtb=HOST:PORT:JOBID     report to the runner listening at HOST:PORT (IPv4)
//   cfg=NAME=VALUE          a config override for this run only (repeatable)
//
// The game reports over TCP: HELLO when it has started, then FILEs (results, logs) and DONE
// when it's finished, then exits back to the loader. Nothing here touches the SD card except
// reading the optional request file. Wii only (needs the network).

#include <stdbool.h>
#include <stddef.h>

// Reads the request from argv (argc/argv as main got them) or, failing that, from file_path
// (NULL: none). Brings the network up and sends HELLO. Returns true if a test run is active.
bool wtb_init(int argc, char **argv, const char *file_path, const char *game, const char *build);
bool wtb_active(void);

// Config overrides of this run, in order
int wtb_cfg_count(void);
const char *wtb_cfg_name(int i);
const char *wtb_cfg_value(int i);

// Progress text for the runner's log (best effort)
void wtb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Sends a file's contents (name: what the runner saves it as)
bool wtb_send_file(const char *name, const void *data, size_t len);
// Sends a file from the SD card if it exists
bool wtb_send_path(const char *name, const char *path);
// Final report ("ok", or a short reason); the caller then exits
void wtb_done(const char *status);

#endif
