// Wii test bench client, protocol WTB/1 (see wtb.h). Self-contained: libogc and the C library
// only, so other ports can copy it as is.

#if defined(TARGET_GX) && defined(__wii__)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <network.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>

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

// Screenshots: the schedule (milliseconds since wtb_init, sorted), the queue of captured images
static unsigned shot_ms[WTB_MAX_SHOTS];
static int nshot_ms, next_shot;
static unsigned shot_every_ms;
static int shot_no = -1;      // the shot being captured (wtb_shot_due returned true for it)
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
    char ip[16] = { 0 }, mask[16] = { 0 }, gw[16] = { 0 };
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
    char hello[256];
    snprintf(hello, sizeof(hello), "WTB/1 HELLO %s %s %s %s\n", job, game, net_ok ? ip : "-", build);
    send_msg(hello, NULL, 0);
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

bool wtb_shot_due(void) {
    shot_no = -1;
    if (!active || shots_taken >= WTB_MAX_SHOTS)
        return false;
    const unsigned now = (unsigned)ticks_to_millisecs(gettime() - t_start);
    unsigned due = ~0u;
    if (next_shot < nshot_ms && shot_ms[next_shot] <= now) {
        due = shot_ms[next_shot++];
        // several times passed at once (a long load): take one now, the next ones on later images
    } else if (shot_every_ms && now >= shot_every_ms * (unsigned)(shots_taken + 1)) {
        due = now;
    } else {
        return false;
    }
    shot_no = shots_taken++;
    shot_t_ms = due;
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

void wtb_done(const char *status) {
    if (!active)
        return;
    char line[200];
    snprintf(line, sizeof(line), "WTB/1 DONE %s %s\n", job, status);
    send_msg(line, NULL, 0);
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
bool wtb_shot_add(const char *kind, int w, int h, const void *yuy2, int stride_bytes) {
    (void)kind; (void)w; (void)h; (void)yuy2; (void)stride_bytes; return false;
}
void wtb_shots_send(void) {}

#endif
