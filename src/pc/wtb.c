// Wii test bench client, protocol WTB/1 (see wtb.h). Self-contained: libogc and the C library
// only, so other ports can copy it as is.

#if defined(TARGET_GX) && defined(__wii__)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <network.h>
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
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!inet_aton(host, &addr.sin_addr))
        return -1;
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0)
        return -1;
    if (net_connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
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

    // DHCP; a few seconds
    char ip[16] = { 0 }, mask[16] = { 0 }, gw[16] = { 0 };
    net_ok = if_config(ip, mask, gw, true, 20) >= 0;
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

#endif
