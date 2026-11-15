// Wii test bench client, protocol WTB/1 (see wtb.h). Self-contained: libogc and the C library
// only, so other ports can copy it as is.

#if defined(TARGET_GX) && defined(__wii__)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <network.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/lwp.h>
#include <ogc/irq.h>
#include <ogc/system.h>
#include <ogc/libversion.h>
#if _V_MAJOR_ >= 3
#include <tuxedo/ppc/exception.h>
#else
#include <ogc/context.h>
#endif

#include "wtb.h"

#define WTB_MAX_CFG 32

static bool active;
static char host[64];
static unsigned short port;
static char job[64];
static char cfg_name[WTB_MAX_CFG][48];
static char cfg_value[WTB_MAX_CFG][96];
static int ncfg;
static bool net_ok;
static u64 t_start;
static unsigned (*game_clock)(void);

// Screenshots: the schedule (milliseconds since wtb_init, sorted), the queue of captured images
static unsigned shot_ms[WTB_MAX_SHOTS];
static int nshot_ms, next_shot;
static unsigned shot_every_ms;
static int shot_no = -1;      // the shot being captured (wtb_shot_due returned true for it)
static int last_shot = -1;
static unsigned shot_t_ms;    // its time
struct Shot { char name[64]; char *data; size_t len; };
static struct Shot shots[WTB_MAX_SHOTS * 2];
static int nshots, shots_taken;
static size_t shots_bytes;
#define WTB_SHOT_MEMORY (40u << 20)  // never hold more than this many bytes of images

bool wtb_active(void) {
    return active;
}

int wtb_cfg_count(void) {
    return ncfg;
}

const char *wtb_cfg_name(int i) {
    return (i >= 0 && i < ncfg) ? cfg_name[i] : "";
}

const char *wtb_cfg_value(int i) {
    return (i >= 0 && i < ncfg) ? cfg_value[i] : "";
}

// Profiler, frame log, memory (see wtb.h)
#define PROF_SLOTS 16384
static u32 prof_pc[PROF_SLOTS];
static u32 prof_cnt[PROF_SLOTS];
static volatile u32 prof_total, prof_lost;
static u32 prof_period;
static unsigned prof_hz = 2000;
static volatile int prof_state;      // 0 off, 1 waiting for T0, 2 sampling, 3 done
static unsigned prof_t0, prof_t1 = ~0u;
#define FRAMES_MAX 32768
struct FrameRec { u32 t_us, retrace; };
static struct FrameRec *fr_end, *fr_show;
static volatile u32 fr_nend, fr_nshow;
static int fr_state;                 // 0 off, 1 waiting, 2 logging, 3 done
static unsigned fr_t0, fr_t1 = ~0u;
static size_t mem_peak, mem_now, mem_arena;
static u32 alive_calls;

// Crash / freeze watchdog state
static unsigned hang_s = 20;
static int test_kind;            // wtb_test: 1 trap, 2 null write (DSI), 3 freeze
static unsigned test_ms;
static volatile u32 alive_count;
static volatile int crashed;     // set by the exception hook
static char crash_text[3072];
static char marks[8][64];
static volatile unsigned nmarks;
static lwp_t watchdog_thread = LWP_THREAD_NULL;

// Streamed log: lines collected here, sent by the watchdog thread
#define STREAM_SIZE (16 * 1024)
static char stream_buf[STREAM_SIZE];
static char stream_out[STREAM_SIZE];
static volatile size_t stream_len;
static volatile unsigned stream_dropped;

static void start_watchdog(void);

// The system clock the PLL is programmed for (overclocked test runs check it): Bollywood =
// 243 MHz x (CLKF + fraction) / the firmware's stock multiplier, Vegas = 54 MHz x (CLKF+1) /
// ((CLKR+1) x CLKO). Needs AHB access (Homebrew Channel apps with <ahb_access/>; wiiload).
static void report_clock(void) {
    volatile u32 *hw = (volatile u32 *)0xCD800000;
    if (hw[0x64 / 4] != 0xFFFFFFFF) {
        wtb_log("clock: unknown (no AHB access)");
        return;
    }
    const u32 pll = hw[0x1B0 / 4], ext = hw[0x1B4 / 4], ver = hw[0x214 / 4];
    const u32 clkr = pll & 0x3f, clkf = (pll >> 6) & 0xfff, clko = (pll >> 18) & 0x1ff;
    u32 khz = 0;
    if ((u8)ver >= 0x20) {
        const u32 stock = (109u << 14) | 1492u;
        if (clkr == 2 && clko == 8)
            khz = (u32)((243000ull * ((clkf << 14) | ((ext >> 9) & 0x3fff)) + stock / 2) / stock);
    } else if (clko != 0) {
        khz = (u32)((54000ull * (clkf + 1)) / ((clkr + 1) * clko));
    }
    if (khz == 0)
        wtb_log("clock: unknown PLL layout, PLLSYS %08X EXT %08X", (unsigned)pll, (unsigned)ext);
    else
        wtb_log("clock: %u.%03u MHz system (CPU %u MHz), %s, PLLSYS %08X EXT %08X", khz / 1000, khz % 1000,
                (khz * 3 + 500) / 1000, (u8)ver >= 0x20 ? "Bollywood" : "Vegas", (unsigned)pll, (unsigned)ext);
}

// "on" (the whole run) or "T0-T1" in seconds
static void parse_window(const char *v, unsigned *t0, unsigned *t1) {
    *t0 = 0;
    *t1 = ~0u;
    if (strncmp(v, "on", 2) == 0 || strncmp(v, "true", 4) == 0)
        return;
    const char *dash = strchr(v, '-');
    *t0 = (unsigned)(atof(v) * 1000.0);
    if (dash != NULL)
        *t1 = (unsigned)(atof(dash + 1) * 1000.0);
}

static int cmp_uint(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

// "5,20.5,60" and/or "every:N" (seconds)
static void parse_shots(const char *v) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", v);
    for (char *tok = strtok(buf, ", \r\n"); tok != NULL; tok = strtok(NULL, ", \r\n")) {
        if (strncmp(tok, "every:", 6) == 0) {
            double e = atof(tok + 6);
            shot_every_ms = e > 0.1 ? (unsigned)(e * 1000.0) : 0;
        } else if (nshot_ms < WTB_MAX_SHOTS) {
            double t = atof(tok);
            if (t >= 0)
                shot_ms[nshot_ms++] = (unsigned)(t * 1000.0);
        }
    }
    qsort(shot_ms, nshot_ms, sizeof(shot_ms[0]), cmp_uint);
}

// One request item: "wtb=HOST:PORT:JOB" or "cfg=NAME=VALUE" (also "wtb HOST:PORT:JOB" and
// "cfg NAME VALUE", the file form)
static void parse_item(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '-')
        s++;
    if (strncmp(s, "wtb", 3) == 0 && (s[3] == '=' || s[3] == ' ')) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s", s + 4);
        char *a = strchr(buf, ':');
        char *b = a ? strchr(a + 1, ':') : NULL;
        if (a == NULL || b == NULL)
            return;
        *a = *b = '\0';
        snprintf(host, sizeof(host), "%s", buf);
        port = (unsigned short)atoi(a + 1);
        snprintf(job, sizeof(job), "%s", b + 1);
        for (char *e = job + strlen(job); e > job && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '); e--)
            e[-1] = '\0';
        active = host[0] && port && job[0];
    } else if (strncmp(s, "cfg", 3) == 0 && (s[3] == '=' || s[3] == ' ') && ncfg < WTB_MAX_CFG) {
        const char *n = s + 4;
        const char *sep = strpbrk(n, "= ");
        if (sep == NULL)
            return;
        if ((size_t)(sep - n) == 9 && strncmp(n, "wtb_shots", 9) == 0) {
            parse_shots(sep + 1);
            return;
        }
        if ((size_t)(sep - n) == 10 && strncmp(n, "wtb_hang_s", 10) == 0) {
            hang_s = (unsigned)atoi(sep + 1);
            return;
        }
        if ((size_t)(sep - n) == 8 && strncmp(n, "wtb_prof", 8) == 0) {
            parse_window(sep + 1, &prof_t0, &prof_t1);
            const char *sp = strchr(sep + 1, ' ');
            if (sp != NULL && atoi(sp + 1) >= 50 && atoi(sp + 1) <= 20000)
                prof_hz = (unsigned)atoi(sp + 1);
            prof_state = 1;
            return;
        }
        if ((size_t)(sep - n) == 10 && strncmp(n, "wtb_frames", 10) == 0) {
            parse_window(sep + 1, &fr_t0, &fr_t1);
            fr_state = 1;
            return;
        }
        if ((size_t)(sep - n) == 8 && strncmp(n, "wtb_test", 8) == 0) {
            const char *v = sep + 1, *at = strchr(v, '@');
            test_kind = strncmp(v, "crash", 5) == 0 ? 1 : strncmp(v, "dsi", 3) == 0 ? 2 : strncmp(v, "hang", 4) == 0 ? 3 : 0;
            test_ms = at ? (unsigned)(atof(at + 1) * 1000.0) : 10000;
            return;
        }
        snprintf(cfg_name[ncfg], sizeof(cfg_name[0]), "%.*s", (int)(sep - n), n);
        snprintf(cfg_value[ncfg], sizeof(cfg_value[0]), "%s", sep + 1);
        char *v = cfg_value[ncfg];
        for (char *e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '); e--)
            e[-1] = '\0';
        ncfg++;
    }
}

static int connect_runner(void) {
    if (!net_ok)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = 8;          // BSD-style 8-byte sockaddr, as IOS expects
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!inet_aton(host, &addr.sin_addr))
        return -1;
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0)
        return -1;
    if (net_connect(s, (struct sockaddr *)&addr, 8) < 0) {   // IOS wants addrlen 8 (16 -> -22 on hardware)
        net_close(s);
        return -1;
    }
    return s;
}

static bool send_all(int s, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        s32 n = net_send(s, p, len > 4096 ? 4096 : len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// One message on its own connection: a header line, then an optional payload
static bool send_msg(const char *header, const void *payload, size_t len) {
    int s = connect_runner();
    if (s < 0)
        return false;
    bool ok = send_all(s, header, strlen(header)) && (len == 0 || send_all(s, payload, len));
    net_close(s);
    return ok;
}

bool wtb_init(int argc, char **argv, const char *file_path, const char *game, const char *build) {
    for (int i = 1; i < argc; i++)
        if (argv[i] != NULL)
            parse_item(argv[i]);
    if (!active && file_path != NULL) {
        FILE *f = fopen(file_path, "r");
        if (f != NULL) {
            char line[256];
            while (fgets(line, sizeof(line), f) != NULL)
                parse_item(line);
            fclose(f);
        }
    }
    if (!active)
        return false;
    t_start = gettime();

    // DHCP; a few seconds
    char ip[16] = { 0 };
    // On real hardware, right after the Homebrew Channel starts us, IOS needs several seconds
    // before the network is usable: if_config's own retries run out first (Dolphin hides this).
    // Loop on net_init while it says -EAGAIN, then wait for DHCP to give us an address.
    {
        u64 t0 = gettime();
        s32 r = -1;
        while (ticks_to_secs(gettime() - t0) < 30 && (r = net_init()) < 0)
            usleep(250 * 1000);
        u32 hip = 0;
        while (r >= 0 && (hip = net_gethostip()) == 0 && ticks_to_secs(gettime() - t0) < 40)
            usleep(100 * 1000);
        net_ok = r >= 0 && hip != 0;
        if (net_ok)
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", hip >> 24, (hip >> 16) & 255, (hip >> 8) & 255, hip & 255);
    }
    start_watchdog();
    char hello[256];
    snprintf(hello, sizeof(hello), "WTB/1 HELLO %s %s %s %s\n", job, game, net_ok ? ip : "-", build);
    send_msg(hello, NULL, 0);
    report_clock();
    return true;
}

void wtb_log(const char *fmt, ...) {
    if (!active)
        return;
    char text[200], line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    for (char *c = text; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    snprintf(line, sizeof(line), "WTB/1 LOG %s %s\n", job, text);
    send_msg(line, NULL, 0);
}

bool wtb_send_file(const char *name, const void *data, size_t len) {
    if (!active)
        return false;
    char header[200];
    snprintf(header, sizeof(header), "WTB/1 FILE %s %s %u\n", job, name, (unsigned)len);
    return send_msg(header, data, len);
}

bool wtb_send_path(const char *name, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    void *buf = len > 0 ? malloc((size_t)len) : NULL;
    if (buf != NULL && fread(buf, 1, (size_t)len, f) == (size_t)len)
        ok = wtb_send_file(name, buf, (size_t)len);
    free(buf);
    fclose(f);
    return ok;
}

void wtb_set_clock(unsigned (*ms)(void)) {
    game_clock = ms;
}

bool wtb_shot_due(void) {
    shot_no = -1;
    if (!active || shots_taken >= WTB_MAX_SHOTS)
        return false;
    const unsigned now = game_clock ? game_clock() : (unsigned)ticks_to_millisecs(gettime() - t_start);
    unsigned due = ~0u;
    if (next_shot < nshot_ms && shot_ms[next_shot] <= now) {
        due = shot_ms[next_shot++];
        // several times passed at once (a long load): take one now, the next ones on later images
    } else if (shot_every_ms && now >= shot_every_ms * (unsigned)(shots_taken + 1)) {
        due = now;
    } else {
        return false;
    }
    shot_no = last_shot = shots_taken++;
    shot_t_ms = due;
    return true;
}

bool wtb_shot_again(void) {
    if (!active || last_shot < 0)
        return false;
    shot_no = last_shot;
    return true;
}

bool wtb_shot_add(const char *kind, int w, int h, const void *yuy2, int stride_bytes) {
    if (!active || shot_no < 0 || w <= 0 || h <= 0 || nshots >= (int)(sizeof(shots) / sizeof(shots[0])))
        return false;
    char header[64];
    const int hl = snprintf(header, sizeof(header), "WTBIMG1 yuy2 %d %d\n", w, h);
    const size_t row = (size_t)w * 2, len = (size_t)hl + row * (size_t)h;
    if (shots_bytes + len > WTB_SHOT_MEMORY) {
        wtb_log("screenshot %d skipped: over the memory limit", shot_no);
        return false;
    }
    char *d = malloc(len);
    if (d == NULL) {
        wtb_log("screenshot %d skipped: out of memory (%u bytes)", shot_no, (unsigned)len);
        return false;
    }
    memcpy(d, header, (size_t)hl);
    for (int y = 0; y < h; y++)
        memcpy(d + hl + row * (size_t)y, (const char *)yuy2 + (size_t)stride_bytes * (size_t)y, row);
    struct Shot *s = &shots[nshots++];
    snprintf(s->name, sizeof(s->name), "shot_%02d_%u.%03us_%s.wtbimg", shot_no, shot_t_ms / 1000,
             shot_t_ms % 1000, kind);
    s->data = d;
    s->len = len;
    shots_bytes += len;
    return true;
}

void wtb_shots_send(void) {
    for (int i = 0; i < nshots; i++) {
        if (!wtb_send_file(shots[i].name, shots[i].data, shots[i].len))
            wtb_log("sending %s failed", shots[i].name);
        free(shots[i].data);
    }
    if (nshot_ms || shot_every_ms)
        wtb_log("screenshots: %d taken of %d scheduled, %d images sent", shots_taken,
                shot_every_ms ? shots_taken : nshot_ms, nshots);
    nshots = 0;
    shots_bytes = 0;
}

// ---- crashes and freezes ---------------------------------------------------------------------

static unsigned now_ms(void) {
    return (unsigned)ticks_to_millisecs(gettime() - t_start);
}

static unsigned clock_ms(void) {
    return game_clock ? game_clock() : now_ms();
}

// ---- profiler: the performance monitor interrupts every prof_period CPU cycles ---------------
// MMCR0 (SPR 952): ENINT (enable the interrupt), PMC1INTCONTROL (PMC1 going negative signals
// it), PMC1SELECT = 1 (count processor cycles). The CPU clears ENINT when it signals, so each
// sample re-arms. PMC1 (SPR 953) counts up from 0x80000000 - period.
#define MMCR0_SAMPLE 0x04008040u
#define MMCR0_FREEZE 0x80000000u

static void prof_arm(void) {
    const u32 start = 0x80000000u - prof_period;
    const u32 mmcr0 = MMCR0_SAMPLE;
    __asm__ volatile("mtspr 953, %0" : : "r"(start));
    __asm__ volatile("mtspr 952, %0" : : "r"(mmcr0));
}

static void prof_stop(void) {
    const u32 mmcr0 = MMCR0_FREEZE;
    __asm__ volatile("mtspr 952, %0" : : "r"(mmcr0));
}

// Exception context: count the sample, re-arm (no calls into anything that locks)
static void prof_sample(u32 pc) {
    if (prof_state != 2) {
        prof_stop();
        return;
    }
    prof_total++;
    u32 h = ((pc >> 2) * 2654435761u) >> (32 - 14);
    for (int i = 0; i < 32; i++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (prof_pc[h] == pc) {
            prof_cnt[h]++;
            break;
        }
        if (prof_pc[h] == 0) {
            prof_pc[h] = pc;
            prof_cnt[h] = 1;
            break;
        }
        if (i == 31)
            prof_lost++;
    }
    prof_arm();
}

static void prof_send(void) {
    if (prof_state == 0)
        return;
    prof_stop();
    const int was = prof_state;
    prof_state = 3;
    size_t cap = 64 + (size_t)PROF_SLOTS * 20, len = 0;
    char *t = malloc(cap);
    if (t == NULL)
        return;
    len += (size_t)snprintf(t, cap, "WTBPROF1 hz %u samples %u lost %u%s\n", prof_hz, (unsigned)prof_total,
                            (unsigned)prof_lost, was == 1 ? " (window never started)" : "");
    for (int i = 0; i < PROF_SLOTS && len + 24 < cap; i++)
        if (prof_pc[i] != 0)
            len += (size_t)snprintf(t + len, cap - len, "%08X %u\n", (unsigned)prof_pc[i], (unsigned)prof_cnt[i]);
    wtb_send_file("prof.txt", t, len);
    free(t);
}

// ---- frame log ------------------------------------------------------------------------------
static u32 now_us(void) {
    return (u32)ticks_to_microsecs(gettime() - t_start);
}

void wtb_frame_shown(unsigned retrace) {
    if (fr_state != 2 || fr_show == NULL || fr_nshow >= FRAMES_MAX)
        return;
    fr_show[fr_nshow].t_us = now_us();
    fr_show[fr_nshow].retrace = retrace;
    fr_nshow++;
}

static void frames_send(void) {
    if (fr_state == 0)
        return;
    fr_state = 3;
    if (fr_end == NULL)
        return;
    char header[96];
    const int hl = snprintf(header, sizeof(header), "WTBFRAMES1 %u %u\n", (unsigned)fr_nend, (unsigned)fr_nshow);
    const size_t len = (size_t)hl + (fr_nend + fr_nshow) * sizeof(struct FrameRec);
    char *b = malloc(len);
    if (b == NULL)
        return;
    memcpy(b, header, (size_t)hl);
    memcpy(b + hl, fr_end, fr_nend * sizeof(struct FrameRec));
    memcpy(b + hl + fr_nend * sizeof(struct FrameRec), fr_show, fr_nshow * sizeof(struct FrameRec));
    wtb_send_file("frames.bin", b, len); // big-endian u32 pairs: finished (t_us, frame no), shown (t_us, retrace)
    free(b);
}

// ---- memory ---------------------------------------------------------------------------------
static void mem_sample(void) {
    struct mallinfo mi = mallinfo();
    mem_now = (size_t)mi.uordblks;
    mem_arena = (size_t)mi.arena;
    if (mem_now > mem_peak)
        mem_peak = mem_now;
}

static void mem_send(void) {
    char t[512];
    const u32 a1 = (u32)SYS_GetArena1Hi() - (u32)SYS_GetArena1Lo();
    const u32 a2 = (u32)SYS_GetArena2Hi() - (u32)SYS_GetArena2Lo();
    const int n = snprintf(t, sizeof(t),
                           "heap in use      %8u KB (last sample)\n"
                           "heap peak        %8u KB (sampled every 30 frames)\n"
                           "heap obtained    %8u KB (from sbrk)\n"
                           "MEM1 arena free  %8u KB\n"
                           "MEM2 arena free  %8u KB\n",
                           (unsigned)(mem_now >> 10), (unsigned)(mem_peak >> 10), (unsigned)(mem_arena >> 10),
                           (unsigned)(a1 >> 10), (unsigned)(a2 >> 10));
    wtb_send_file("mem.txt", t, (size_t)n);
}

// Windows open and close on the game's own frames
static void windows_update(void) {
    const unsigned c = clock_ms();
    if (prof_state == 1 && c >= prof_t0) {
        prof_period = 729000000u / prof_hz; // Broadway clock
        prof_state = 2;
        wtb_mark("profiler on (%u Hz)", prof_hz);
        prof_arm();
    } else if (prof_state == 2 && c >= prof_t1) {
        prof_state = 3;
        prof_stop();
        wtb_mark("profiler off: %u samples", (unsigned)prof_total);
    }
    if (fr_state == 1 && c >= fr_t0) {
        fr_end = malloc(FRAMES_MAX * sizeof(struct FrameRec));
        fr_show = malloc(FRAMES_MAX * sizeof(struct FrameRec));
        if (fr_end == NULL || fr_show == NULL) {
            wtb_log("frame log: out of memory");
            fr_state = 3;
        } else {
            fr_state = 2;
        }
    } else if (fr_state == 2 && c >= fr_t1) {
        fr_state = 3;
    }
}

void wtb_mark(const char *fmt, ...) {
    if (!active)
        return;
    char *m = marks[nmarks % 8];
    int n = snprintf(m, sizeof(marks[0]), "%u.%03u ", now_ms() / 1000, now_ms() % 1000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m + n, sizeof(marks[0]) - (size_t)n, fmt, ap);
    va_end(ap);
    nmarks++;
}

static int append(char *buf, size_t size, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int append(char *buf, size_t size, int pos, const char *fmt, ...) {
    if (pos < 0 || (size_t)pos >= size)
        return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, size - (size_t)pos, fmt, ap);
    va_end(ap);
    return n < 0 ? pos : pos + n > (int)size - 1 ? (int)size - 1 : pos + n;
}

static bool code_addr(u32 a) {
    return (a >= 0x80000000 && a < 0x81800000) || (a >= 0x90000000 && a < 0x94000000);
}

static int append_marks(char *buf, size_t size, int pos) {
    pos = append(buf, size, pos, "recent marks (oldest first):\n");
    const unsigned n = nmarks;
    for (unsigned i = n > 8 ? n - 8 : 0; i < n; i++)
        pos = append(buf, size, pos, "  %s\n", marks[i % 8]);
    return pos;
}

static void parked(void) {
    for (;;)
        usleep(1000 * 1000); // the crashed thread sleeps here; the watchdog takes over
}

static const char *exc_name(unsigned exid) {
#if _V_MAJOR_ >= 3
    switch (exid) {
        case PPC_EXCPT_DSI: return "DSI (bad data access)";
        case PPC_EXCPT_ISI: return "ISI (bad instruction fetch)";
        case PPC_EXCPT_ALIGN: return "alignment";
        case PPC_EXCPT_UNDEF: return "program (trap or illegal instruction)";
        case PPC_EXCPT_MCHK: return "machine check";
        case PPC_EXCPT_FPU: return "floating point";
        default: return "exception";
    }
#else
    switch (exid) {
        case EX_DSI: return "DSI (bad data access)";
        case EX_ISI: return "ISI (bad instruction fetch)";
        case EX_ALIGN: return "alignment";
        case EX_PRG: return "program (trap or illegal instruction)";
        case EX_MACH_CHECK: return "machine check";
        case EX_FP: return "floating point";
        default: return "exception";
    }
#endif
}

// Raw crash state, filled in exception context (no printf there: it can use the FPU, which is off
// in an exception, and take locks); the watchdog thread formats it
static struct {
    unsigned exid, t_ms;
    u32 pc, lr, msr, dar, dsisr, thread;
    u32 gpr[32];
    u32 chain[24];
    int nchain;
} crash;

static u32 record_crash(unsigned exid, u32 pc, u32 lr, u32 msr, const u32 *gpr) {
    __asm__ volatile("mfspr %0, 19" : "=r"(crash.dar));
    __asm__ volatile("mfspr %0, 18" : "=r"(crash.dsisr));
    crash.exid = exid;
    crash.t_ms = now_ms();
    crash.pc = pc;
    crash.lr = lr;
    crash.msr = msr;
    crash.thread = (u32)LWP_GetSelf();
    for (int i = 0; i < 32; i++)
        crash.gpr[i] = gpr[i];
    // Call chain: the saved LR of each frame up the back chain
    u32 sp = gpr[1];
    crash.nchain = 0;
    for (int i = 0; i < 24 && code_addr(sp) && (sp & 3) == 0; i++) {
        const u32 next = *(volatile u32 *)sp;
        if (!code_addr(next) || next <= sp)
            break;
        const u32 saved_lr = *(volatile u32 *)(next + 4);
        if (code_addr(saved_lr))
            crash.chain[crash.nchain++] = saved_lr;
        sp = next;
    }
    crashed = 1;
    return (u32)(uintptr_t)parked;
}

static void format_crash(void) {
    char *b = crash_text;
    const size_t z = sizeof(crash_text);
    int p = append(b, z, 0, "crash: %s (exception %u) at %u.%03u s, thread %08X\n", exc_name(crash.exid),
                   crash.exid, crash.t_ms / 1000, crash.t_ms % 1000, (unsigned)crash.thread);
    p = append(b, z, p, "pc %08X lr %08X sp %08X msr %08X dar %08X dsisr %08X\n", (unsigned)crash.pc,
               (unsigned)crash.lr, (unsigned)crash.gpr[1], (unsigned)crash.msr, (unsigned)crash.dar,
               (unsigned)crash.dsisr);
    for (int i = 0; i < 32; i += 4)
        p = append(b, z, p, "r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i, (unsigned)crash.gpr[i], i + 1,
                   (unsigned)crash.gpr[i + 1], i + 2, (unsigned)crash.gpr[i + 2], i + 3, (unsigned)crash.gpr[i + 3]);
    p = append(b, z, p, "stack: %08X %08X", (unsigned)crash.pc, (unsigned)crash.lr);
    for (int i = 0; i < crash.nchain; i++)
        p = append(b, z, p, " %08X", (unsigned)crash.chain[i]);
    p = append(b, z, p, "\n");
    append_marks(b, z, p);
}

#if _V_MAJOR_ >= 3
static PPCExcptPanicFn prev_panic;
static void wtb_panic(unsigned exid, PPCContext *ctx) {
    if (exid == PPC_EXCPT_PM) {
        prof_sample(ctx->pc);
        return;
    }
    if (crashed || exid == PPC_EXCPT_RESET) {
        prev_panic(exid, ctx);
        return;
    }
    ctx->pc = record_crash(exid, ctx->pc, ctx->lr, ctx->msr, ctx->gpr);
    ctx->msr |= 0x8000; // EE: interrupts on, so the parked thread can sleep
}
static void install_crash_hook(void) {
    prev_panic = PPCExcptCurPanicFn;
    PPCExcptCurPanicFn = wtb_panic;
}
#else
// libogc 2.x: its exception vectors call c_default_exceptionhandler; the game links with
// -Wl,--wrap=c_default_exceptionhandler so that calls arrive here first
void __real_c_default_exceptionhandler(frame_context *ctx);
void __wrap_c_default_exceptionhandler(frame_context *ctx) {
    if (active && ctx->EXCPT_Number == EX_PERF) {
        prof_sample(ctx->SRR0);
        return;
    }
    if (!active || crashed || ctx->EXCPT_Number == EX_SYS_RESET) {
        __real_c_default_exceptionhandler(ctx);
        return;
    }
    ctx->SRR0 = record_crash(ctx->EXCPT_Number, ctx->SRR0, ctx->LR, ctx->SRR1, ctx->GPR);
    ctx->SRR1 |= 0x8000;
}
static void install_crash_hook(void) {
}
#endif

void wtb_stream(const char *line) {
    if (!active || line == NULL)
        return;
    const size_t n = strlen(line);
    u32 level = IRQ_Disable();
    if (stream_len + n + 1 <= STREAM_SIZE) {
        memcpy(stream_buf + stream_len, line, n);
        stream_buf[stream_len + n] = '\n';
        stream_len += n + 1;
    } else {
        stream_dropped++;
    }
    IRQ_Restore(level);
}

static void stream_flush(void) {
    if (stream_len == 0 && stream_dropped == 0)
        return;
    u32 level = IRQ_Disable();
    size_t n = stream_len;
    memcpy(stream_out, stream_buf, n);
    stream_len = 0;
    const unsigned dropped = stream_dropped;
    stream_dropped = 0;
    IRQ_Restore(level);
    char header[96];
    snprintf(header, sizeof(header), "WTB/1 LOGS %s %u\n", job, (unsigned)n);
    int s = connect_runner();
    if (s < 0)
        return;
    if (send_all(s, header, strlen(header)))
        send_all(s, stream_out, n);
    net_close(s);
    if (dropped)
        wtb_log("log stream: %u lines dropped (more than 16 KB a second)", dropped);
}

static void send_report_and_exit(const char *file, const char *text, const char *status) {
    stream_flush();
    wtb_send_file(file, text, strlen(text));
    wtb_shots_send();
    wtb_done(status);
    // Back to the loader without running atexit handlers or flushing stdio: the stuck thread may
    // hold their locks. libogc's _exit returns to the Homebrew Channel's reload stub.
    _exit(0);
}

static void *watchdog(void *arg) {
    (void)arg;
    u32 last = alive_count;
    unsigned last_change = now_ms();
    unsigned last_flush = now_ms();
    for (;;) {
        usleep(250 * 1000);
        if (now_ms() - last_flush >= 1000) {
            stream_flush();
            last_flush = now_ms();
        }
        if (crashed) {
            format_crash();
            char status[160];
            const char *nl = strchr(crash_text, '\n');
            snprintf(status, sizeof(status), "%.*s", nl ? (int)(nl - crash_text) : 100, crash_text);
            wtb_log("%s", status);
            send_report_and_exit("crash.txt", crash_text, status);
        }
        const u32 a = alive_count;
        const unsigned t = now_ms();
        if (a != last) {
            last = a;
            last_change = t;
            continue;
        }
        const unsigned limit = (a == 0 ? (hang_s > 60 ? hang_s : 60) : hang_s) * 1000;
        if (hang_s && t - last_change > limit) {
            static char text[1024];
            int p = append(text, sizeof(text), 0,
                           "hang: no progress for %u s (at %u.%03u s, %u frames shown)\n", (t - last_change) / 1000,
                           t / 1000, t % 1000, (unsigned)a);
            append_marks(text, sizeof(text), p);
            char status[96];
            snprintf(status, sizeof(status), "hang: no progress for %u s", (t - last_change) / 1000);
            wtb_log("%s", status);
            send_report_and_exit("hang.txt", text, status);
        }
    }
    return NULL;
}

static void start_watchdog(void) {
    install_crash_hook();
    LWP_CreateThread(&watchdog_thread, watchdog, NULL, NULL, 32 * 1024, 120);
}

void wtb_alive(void) {
    if (!active)
        return;
    alive_count++;
    if (fr_state == 2 && fr_nend < FRAMES_MAX) {
        fr_end[fr_nend].t_us = now_us();
        fr_end[fr_nend].retrace = (u32)alive_count;
        fr_nend++;
    }
    if (prof_state == 1 || prof_state == 2 || fr_state == 1 || fr_state == 2)
        windows_update();
    if (++alive_calls % 30 == 1)
        mem_sample();
    if (test_kind && now_ms() >= test_ms) {
        const int k = test_kind;
        test_kind = 0;
        wtb_log("wtb_test: %s now", k == 1 ? "crash (trap)" : k == 2 ? "crash (null write)" : "freeze");
        if (k == 1)
            __builtin_trap();
        else if (k == 2)
            *(volatile u32 *)0x00000010 = 1; // no BAT maps address 0: DSI on hardware
        else
            for (;;) {
            }
    }
}

void wtb_done(const char *status) {
    if (!active)
        return;
    prof_send();
    frames_send();
    mem_send();
    wtb_shots_send();
    stream_flush();
    char line[200];
    snprintf(line, sizeof(line), "WTB/1 DONE %s %s\n", job, status);
    send_msg(line, NULL, 0);
}


// Asks the runner for the recipe's ROM (recipes with "serve_rom"): the game doesn't have it yet.
// alloc(size) provides the buffer. Returns it with *len set, or NULL.
void *wtb_fetch_rom(void *(*alloc)(size_t size), size_t *len) {
    char header[256];
    if (!active)
        return NULL;
    int s = connect_runner();
    if (s < 0)
        return NULL;
    snprintf(header, sizeof(header), "WTB/1 GETROM %s\n", job);
    void *buf = NULL;
    if (send_all(s, header, strlen(header))) {
        // reply header: "WTB/1 ROM <length>\n" or "WTB/1 NOROM\n"
        char line[64];
        size_t n = 0;
        while (n < sizeof(line) - 1) {
            s32 r = net_recv(s, line + n, 1, 0);
            if (r <= 0 || line[n] == '\n')
                break;
            n++;
        }
        line[n] = '\0';
        unsigned long size = 0;
        if (sscanf(line, "WTB/1 ROM %lu", &size) == 1 && size > 0 && (buf = alloc(size)) != NULL) {
            size_t got = 0;
            while (got < size) {
                s32 r = net_recv(s, (char *)buf + got, (size - got) > 65536 ? 65536 : (size - got), 0);
                if (r <= 0)
                    break;
                got += (size_t)r;
            }
            if (got == size)
                *len = size;
            else
                buf = NULL;
        }
    }
    net_close(s);
    return buf;
}

#else

#include "wtb.h"

bool wtb_init(int argc, char **argv, const char *file_path, const char *game, const char *build) {
    (void)argc; (void)argv; (void)file_path; (void)game; (void)build;
    return false;
}
bool wtb_active(void) { return false; }
int wtb_cfg_count(void) { return 0; }
const char *wtb_cfg_name(int i) { (void)i; return ""; }
const char *wtb_cfg_value(int i) { (void)i; return ""; }
void wtb_log(const char *fmt, ...) { (void)fmt; }
bool wtb_send_file(const char *name, const void *data, size_t len) { (void)name; (void)data; (void)len; return false; }
bool wtb_send_path(const char *name, const char *path) { (void)name; (void)path; return false; }
void wtb_done(const char *status) { (void)status; }
bool wtb_shot_due(void) { return false; }
void wtb_set_clock(unsigned (*ms)(void)) { (void)ms; }
bool wtb_shot_again(void) { return false; }
bool wtb_shot_add(const char *kind, int w, int h, const void *yuy2, int stride_bytes) {
    (void)kind; (void)w; (void)h; (void)yuy2; (void)stride_bytes; return false;
}
void wtb_shots_send(void) {}
void wtb_alive(void) {}
void wtb_stream(const char *line) { (void)line; }
void wtb_frame_shown(unsigned retrace) { (void)retrace; }
void wtb_mark(const char *fmt, ...) { (void)fmt; }

void *wtb_fetch_rom(void *(*alloc)(size_t size), size_t *len) { (void)alloc; (void)len; return NULL; }
#endif
