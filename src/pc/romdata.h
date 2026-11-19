#ifndef ROMDATA_H
#define ROMDATA_H

// The game data boot.dol shares with the ROM (textures, sounds, animations, models...) isn't in
// boot.dol: wii/tools/strip_rom_data.py moves it to .bss at link time and lists where each piece
// is in the ROM (build/<version>_wii/romdata_table.c). romdata_restore() puts it back from the
// player's own ROM (apps/sm64/sm64.z64) before the game starts.

struct WiiRomSource {
    unsigned int rom_offset;  // where it starts in the ROM
    unsigned int stored_size; // bytes in the ROM
    unsigned int size;        // bytes once decompressed
    unsigned int mio0;        // 0: the ROM as it is (rom_offset 0); 1: an MIO0 block
};

struct WiiRomPiece {
    void *dest;
    unsigned short source; // index in gWiiRomSources
    unsigned short kind;   // 0: copied as is; 1: N64 vertices (16 bytes) expanded to the port's float Vtx (24)
    unsigned int offset;   // in the (decompressed) source
    unsigned int size;     // bytes at dest
};

extern const unsigned int gWiiRomCrc[2];
extern const unsigned int gWiiRomDataSum;
extern const unsigned int gWiiRomSourceCount;
extern const unsigned int gWiiRomPieceCount;
extern const struct WiiRomSource gWiiRomSources[];
extern const struct WiiRomPiece gWiiRomPieces[];

// Loads the ROM (from apps/sm64/sm64.z64 on the SD card or USB drive; in a test bench run, from
// the runner if it isn't there, saving it there) and restores every piece. If it can't, it says
// why on screen (or to the test bench) and exits to the loader.
void romdata_restore(void);

#endif
