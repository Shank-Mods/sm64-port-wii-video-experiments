// Puts the ROM's data back into the game at boot (see romdata.h and wii/tools/strip_rom_data.py).
#if defined(TARGET_GX) && defined(__wii__)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "romdata.h"
#include "configfile.h"
#include "wtb.h"

#define ROM_NAME "sm64.z64"
#define ROM_MIN_SIZE (8u << 20)
#define ROM_MAX_SIZE (64u << 20)

// Temporary memory (the ROM, one decompressed segment) is taken from the top of the MEM2 arena
// and given back when done, so the game's heap never sees it
static u32 sArena2HiOrig, sArena2Hi;

static void *temp_alloc(size_t size) {
    u32 lvl = IRQ_Disable();
    u32 lo = (u32)SYS_GetArena2Lo();
    u32 p = (sArena2Hi - size) & ~31u;
    void *ret = NULL;
    if (size < sArena2Hi && p > lo + (1u << 20)) { // leave a little for anything else in MEM2
        sArena2Hi = p;
        SYS_SetArena2Hi((void *)p);
        ret = (void *)p;
    }
    IRQ_Restore(lvl);
    return ret;
}

static void temp_release_to(u32 mark) {
    u32 lvl = IRQ_Disable();
    sArena2Hi = mark;
    SYS_SetArena2Hi((void *)mark);
    IRQ_Restore(lvl);
}

static void romdata_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void romdata_fatal(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    dbglog("romdata: %s", msg);
    if (wtb_active()) {
        wtb_log("romdata: %s", msg);
        char status[96];
        snprintf(status, sizeof(status), "romdata: %.80s", msg);
        wtb_done(status);
        exit(0);
    }
    // On screen, with libogc's text console, for 30 seconds; then back to the loader
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    printf("\x1b[3;0H");
    printf("  Super Mario 64 for Wii\n\n");
    printf("  %s\n\n", msg);
    printf("  This port needs your own copy of the game: put your Super Mario 64\n");
    printf("  (USA) ROM in the apps/sm64 folder of the SD card (or USB drive) as\n");
    printf("  %s (.z64, .v64 and .n64 byte orders all work), next to boot.dol.\n\n", ROM_NAME);
    printf("  Going back to the loader in 30 seconds...\n");
    fflush(stdout);
    for (int i = 0; i < 30 * 60; i++)
        VIDEO_WaitVSync();
    exit(0);
}

// Byte order: .z64 is the console's (big-endian, starts 80 37 12 40)
static bool fix_byte_order(u8 *rom, u32 size) {
    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)
        return true;
    if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) { // .v64
        for (u32 i = 0; i + 1 < size; i += 2) {
            u8 t = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = t;
        }
        return true;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) { // .n64
        for (u32 i = 0; i + 3 < size; i += 4) {
            u8 a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = b;
            rom[i + 3] = a;
        }
        return true;
    }
    return false;
}

static u8 *load_rom_file(const char *path, u32 *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    u8 *rom = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n < (long)ROM_MIN_SIZE || n > (long)ROM_MAX_SIZE) {
            fclose(f);
            romdata_fatal("%s is %ld bytes: that isn't a Super Mario 64 ROM.", path, n);
        }
        rewind(f);
        rom = temp_alloc((size_t)n);
        if (rom == NULL) {
            fclose(f);
            romdata_fatal("Not enough memory to load %s.", path);
        }
        size_t got = 0;
        while (got < (size_t)n) {
            size_t r = fread(rom + got, 1, ((size_t)n - got) > (256u << 10) ? (256u << 10) : ((size_t)n - got), f);
            if (r == 0)
                break;
            got += r;
        }
        if (got != (size_t)n) {
            fclose(f);
            romdata_fatal("Couldn't read %s.", path);
        }
        *size = (u32)n;
    }
    fclose(f);
    return rom;
}

static void save_rom_file(const u8 *rom, u32 size) {
    const char *path = get_storage_path(ROM_NAME);
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    bool ok = fwrite(rom, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        unlink(path); // never leave half a ROM behind
    wtb_log("romdata: ROM from the runner %s %s", ok ? "saved as" : "could not be saved as", path);
}

static inline u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool mio0_decode(const u8 *in, u32 in_size, u8 *out, u32 out_size) {
    if (in_size < 16 || memcmp(in, "MIO0", 4) != 0 || be32(in + 4) != out_size)
        return false;
    u32 comp = be32(in + 8), raw = be32(in + 12);
    if (comp > in_size || raw > in_size)
        return false;
    const u8 *bits = in + 16, *cp = in + comp, *rp = in + raw, *end = in + in_size;
    u32 n = 0, bitbuf = 0;
    int nbits = 0;
    while (n < out_size) {
        if (nbits == 0) {
            if (bits + 4 > end)
                return false;
            bitbuf = be32(bits);
            bits += 4;
            nbits = 32;
        }
        if (bitbuf & 0x80000000u) {
            if (rp >= end)
                return false;
            out[n++] = *rp++;
        } else {
            if (cp + 2 > end)
                return false;
            u32 v = ((u32)cp[0] << 8) | cp[1];
            cp += 2;
            u32 len = (v >> 12) + 3, dist = (v & 0xFFF) + 1;
            if (dist > n)
                return false;
            while (len-- > 0 && n < out_size) {
                out[n] = out[n - dist];
                n++;
            }
        }
        bitbuf <<= 1;
        nbits--;
    }
    return true;
}

// N64 vertex (s16 x, y, z, flag, s, t, u8 rgba/normal) to the port's (float x, y, z, then the rest)
static void expand_vertices(u8 *dest, const u8 *src, u32 count) {
    for (u32 i = 0; i < count; i++, dest += 24, src += 16) {
        float xyz[3];
        for (int k = 0; k < 3; k++)
            xyz[k] = (float)(s16)((src[2 * k] << 8) | src[2 * k + 1]);
        memcpy(dest, xyz, 12);
        memcpy(dest + 12, src + 6, 10);
        dest[22] = dest[23] = 0;
    }
}

void romdata_restore(void) {
    if (gWiiRomPieceCount == 0)
        return; // built without strip_rom_data.py (STRIP_ROM_DATA=0)
    u64 t0 = gettime();
    sArena2HiOrig = sArena2Hi = (u32)SYS_GetArena2Hi();

    // The ROM: apps/sm64/sm64.z64 on the SD card, then the USB drive
    u32 rom_size = 0;
    u8 *rom = NULL;
    static const char *const paths[] = { "sd:/apps/sm64/" ROM_NAME, "usb:/apps/sm64/" ROM_NAME };
    const char *from = NULL;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && rom == NULL; i++) {
        rom = load_rom_file(paths[i], &rom_size);
        from = paths[i];
    }
    if (rom == NULL && wtb_active()) {
        // a test bench run on a Wii that doesn't have it yet: the runner has it (serve_rom)
        size_t len = 0;
        rom = wtb_fetch_rom(temp_alloc, &len);
        if (rom != NULL && (len < ROM_MIN_SIZE || len > ROM_MAX_SIZE))
            romdata_fatal("The runner sent a %u-byte ROM.", (unsigned)len);
        if (rom != NULL) {
            rom_size = (u32)len;
            from = "the test bench runner";
            if (fix_byte_order(rom, rom_size))
                save_rom_file(rom, rom_size);
        }
    }
    if (rom == NULL)
        romdata_fatal("No ROM found at sd:/apps/sm64/%s or usb:/apps/sm64/%s.", ROM_NAME, ROM_NAME);
    if (!fix_byte_order(rom, rom_size))
        romdata_fatal("%s isn't an N64 ROM.", from);
    if (be32(rom + 0x10) != gWiiRomCrc[0] || be32(rom + 0x14) != gWiiRomCrc[1])
        romdata_fatal("%s isn't the right ROM: this build needs Super Mario 64 (USA), CRC %08X %08X.",
                      from, gWiiRomCrc[0], gWiiRomCrc[1]);
    u64 t1 = gettime();

    // Every piece, one ROM segment at a time (the pieces are sorted by segment)
    u32 sum = 0, bytes = 0, p = 0;
    for (u32 s = 0; s < gWiiRomSourceCount; s++) {
        const struct WiiRomSource *src = &gWiiRomSources[s];
        u32 mark = sArena2Hi;
        const u8 *base;
        u32 base_size;
        if (src->rom_offset + src->stored_size > rom_size)
            romdata_fatal("%s is too short (segment at 0x%X).", from, src->rom_offset);
        if (src->mio0) {
            u8 *buf = temp_alloc(src->size);
            if (buf == NULL)
                romdata_fatal("Not enough memory to decompress the ROM's segment at 0x%X.", src->rom_offset);
            if (!mio0_decode(rom + src->rom_offset, src->stored_size, buf, src->size))
                romdata_fatal("The ROM's segment at 0x%X doesn't decompress.", src->rom_offset);
            base = buf;
            base_size = src->size;
        } else {
            base = rom;
            base_size = rom_size;
        }
        for (; p < gWiiRomPieceCount && gWiiRomPieces[p].source == s; p++) {
            const struct WiiRomPiece *pc = &gWiiRomPieces[p];
            u8 *dest = pc->dest;
            u32 need = pc->kind == 1 ? pc->size / 24 * 16 : pc->size;
            if (pc->offset + need > base_size)
                romdata_fatal("Bad ROM data table entry %u.", (unsigned)p);
            if (pc->kind == 1)
                expand_vertices(dest, base + pc->offset, pc->size / 24);
            else
                memcpy(dest, base + pc->offset, pc->size);
            for (u32 k = 0; k < pc->size; k++)
                sum += dest[k];
            bytes += pc->size;
            DCFlushRange(dest, pc->size);
        }
        temp_release_to(mark);
    }
    temp_release_to(sArena2HiOrig); // the ROM itself
    if (p != gWiiRomPieceCount)
        romdata_fatal("Bad ROM data table (%u of %u pieces).", (unsigned)p, gWiiRomPieceCount);
    if (sum != gWiiRomDataSum)
        romdata_fatal("The data from %s doesn't match this build (checksum %08X, expected %08X).", from, sum,
                      gWiiRomDataSum);
    u64 t2 = gettime();
    dbglog("romdata: %u bytes in %u pieces from %s (load %u ms, restore %u ms)", bytes, gWiiRomPieceCount, from,
           (unsigned)ticks_to_millisecs(t1 - t0), (unsigned)ticks_to_millisecs(t2 - t1));
    wtb_log("romdata: %u bytes in %u pieces from %s (load %u ms, restore %u ms)", bytes, gWiiRomPieceCount, from,
            (unsigned)ticks_to_millisecs(t1 - t0), (unsigned)ticks_to_millisecs(t2 - t1));
}

#endif
