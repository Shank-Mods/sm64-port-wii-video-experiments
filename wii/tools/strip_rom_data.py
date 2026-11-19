#!/usr/bin/env python3
"""Keep the game data that comes from the ROM out of boot.dol (Wii port; same idea as the
MK64, MM and DKR ports' strip tools, adapted to the sm64-port build).

The sm64-port build turns the ROM's textures, sounds, animations and so on (extract_assets.py)
back into C arrays and links them into boot.dol. boot.dol is shipped, so before the link this
tool goes through the asset objects (compiled with -fdata-sections, so each array is its own
section) and, for every data section without relocations whose bytes are found in the user's
ROM, either as stored (sound banks, Mario's animations) or inside one of the ROM's
MIO0-compressed segments (textures, models, collision, ...):

  - writes a copy of the object in which that section is renamed to .bss.wiirom.N (no file
    bytes: it ends up in .bss, so it isn't in boot.dol at all) with a symbol __wiirom_N at its
    start, and
  - lists it in a generated C table (where it is, which ROM segment holds it, offset, size),
    which src/pc/romdata.c uses to copy every piece back from the user's ROM at boot, before
    the game starts. The ROM's CRCs and a checksum of every restored byte are in the table too.

Sections with relocations (display lists, scripts, anything with pointers) are left as they
are: those are the port's own converted forms, not the ROM's bytes.

Usage: strip_rom_data.py --rom ROM --objcopy OBJCOPY --build BUILD_DIR --out OUT_DIR
                         --table TABLE.c [--report REPORT.txt] OBJ...
Each OBJ (under BUILD_DIR) is copied to OUT_DIR/<same relative path>, stripped.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

MIN_SIZE = 32        # smaller pieces stay in boot.dol (not worth a table entry)
SHT_PROGBITS, SHT_RELA, SHT_REL = 1, 4, 9
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR, SHF_MERGE = 1, 2, 4, 0x10
DATA_PREFIXES = (".rodata.", ".data.", ".rodata", ".data")


def normalize_rom(rom):
    if rom[:4] == b"\x37\x80\x40\x12":  # .v64: byte-swapped
        b = bytearray(rom)
        b[0::2], b[1::2] = rom[1::2], rom[0::2]
        return bytes(b)
    if rom[:4] == b"\x40\x12\x37\x80":  # .n64: little-endian words
        b = bytearray(len(rom))
        for k in range(4):
            b[k::4] = rom[3 - k::4]
        return bytes(b)
    if rom[:4] != b"\x80\x37\x12\x40":
        sys.exit("strip_rom_data: not an N64 ROM")
    return rom


KIND_COPY, KIND_VTX = 0, 1


def vtx_to_n64(data):
    """The N64 form (16 bytes a vertex: s16 x, y, z, flag, s16 s, t, rgba) of an array of the
    port's float vertices (GBI_FLOATS: float x, y, z, u16 flag, s16 s, t, rgba, 2 bytes padding),
    if they are whole numbers the N64 form holds exactly; else None. romdata.c expands them back."""
    if len(data) % 24:
        return None
    out = bytearray()
    for k in range(0, len(data), 24):
        x, y, z = struct.unpack_from(">fff", data, k)
        if data[k + 22:k + 24] != b"\0\0":
            return None
        for v in (x, y, z):
            if v != int(v) or not -32768 <= v <= 32767:
                return None
        out += struct.pack(">hhh", int(x), int(y), int(z)) + data[k + 12:k + 22]
    if vtx_from_n64(bytes(out)) != data:
        return None  # -0.0 or the like: not exactly what the expansion gives
    return bytes(out)


def vtx_from_n64(n64):
    out = bytearray()
    for k in range(0, len(n64), 16):
        x, y, z = struct.unpack_from(">hhh", n64, k)
        out += struct.pack(">fff", x, y, z) + n64[k + 6:k + 16] + b"\0\0"
    return bytes(out)


def mio0_decode(rom, off):
    """(decompressed bytes, compressed length) of the MIO0 block at off, or None if it isn't one"""
    if rom[off:off + 4] != b"MIO0" or off + 16 > len(rom):
        return None
    dsize, comp_off, raw_off = struct.unpack_from(">III", rom, off + 4)
    if not 0 < dsize <= 0x400000 or not 16 <= comp_off <= raw_off or off + raw_off > len(rom):
        return None
    out = bytearray()
    bits_pos, comp_pos, raw_pos = off + 16, off + comp_off, off + raw_off
    end_bits = off + comp_off
    bitbuf, nbits = 0, 0
    try:
        while len(out) < dsize:
            if nbits == 0:
                if bits_pos + 4 > end_bits:
                    return None
                bitbuf, = struct.unpack_from(">I", rom, bits_pos)
                bits_pos += 4
                nbits = 32
            bit = bitbuf & 0x80000000
            bitbuf = (bitbuf << 1) & 0xFFFFFFFF
            nbits -= 1
            if bit:
                out.append(rom[raw_pos])
                raw_pos += 1
            else:
                v = (rom[comp_pos] << 8) | rom[comp_pos + 1]
                comp_pos += 2
                length, dist = (v >> 12) + 3, (v & 0xFFF) + 1
                if dist > len(out):
                    return None
                for _ in range(length):
                    out.append(out[-dist])
        if raw_pos > len(rom) or comp_pos > off + raw_off:
            return None
    except IndexError:
        return None
    return bytes(out[:dsize]), max(raw_pos, comp_pos) - off


class Elf:
    """Just enough of a 32-bit big-endian relocatable ELF"""

    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
            raise ValueError(f"{path}: not a 32-bit big-endian ELF")
        shoff, = struct.unpack_from(">I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(">HHH", d, 0x2E)
        self.secs = []
        for i in range(shnum):
            self.secs.append(struct.unpack_from(">IIIIIIIIII", d, shoff + i * shentsize))
        stroff = self.secs[shstrndx][4]
        self.names = [self.cstr(stroff + s[0]) for s in self.secs]

    def cstr(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode("latin-1")

    def candidates(self):
        """(index, name, bytes) of the data sections that could be ROM data"""
        relocated = set()
        for s in self.secs:
            if s[1] in (SHT_RELA, SHT_REL):
                relocated.add(s[7])  # sh_info: the section the relocations apply to
        for i, s in enumerate(self.secs):
            name, typ, flags, off, size = self.names[i], s[1], s[2], s[4], s[5]
            if typ != SHT_PROGBITS or not flags & SHF_ALLOC or flags & (SHF_EXECINSTR | SHF_MERGE):
                continue
            if not name.startswith(DATA_PREFIXES) or i in relocated or size < MIN_SIZE:
                continue
            yield i, name, self.d[off:off + size]


class Corpus:
    """The ROM as stored, then each of its MIO0 segments decompressed, searchable as one string"""

    def __init__(self, rom):
        self.sources = [(0, len(rom), len(rom))]  # (ROM offset, stored length, length); 0 = the ROM as is
        parts, self.starts = [rom], [0]
        pos, total = 0, len(rom)
        while True:
            pos = rom.find(b"MIO0", pos)
            if pos < 0:
                break
            r = mio0_decode(rom, pos) if pos % 2 == 0 else None
            if r:
                data, clen = r
                self.sources.append((pos, clen, len(data)))
                self.starts.append(total)
                parts.append(data)
                total += len(data)
                pos += clen
            else:
                pos += 4
        self.blob = b"".join(parts)
        self.ends = [s + n for s, (_, _, n) in zip(self.starts, self.sources)]

    def find(self, data):
        """(source index, offset in it) of data, or None"""
        import bisect
        pos = self.blob.find(data)
        while pos >= 0:
            k = bisect.bisect_right(self.starts, pos) - 1
            if pos + len(data) <= self.ends[k]:
                return k, pos - self.starts[k]
            pos = self.blob.find(data, pos + 1)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--table", required=True)
    ap.add_argument("--report")
    ap.add_argument("objs", nargs="+")
    a = ap.parse_args()

    if not os.path.isfile(a.rom):
        sys.exit(f"strip_rom_data: {a.rom} not found: the Wii build needs the ROM to keep its data out of boot.dol")
    rom = normalize_rom(open(a.rom, "rb").read())
    crc1, crc2 = struct.unpack_from(">II", rom, 0x10)
    corpus = Corpus(rom)
    used_sources = {}
    pieces = []  # (symbol, source index, offset, size, byte sum)
    kept = stripped = 0
    report = []
    for obj in a.objs:
        rel = os.path.relpath(obj, a.build)
        dst = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        elf = Elf(obj)
        args = []
        obj_stripped = obj_kept = 0
        for i, name, data in elf.candidates():
            if len(set(data)) < 2:
                obj_kept += len(data)  # one repeated byte: nothing of the ROM's
                continue
            kind, hit = KIND_COPY, corpus.find(data)
            if hit is None:
                n64 = vtx_to_n64(data)
                if n64 is not None:
                    kind, hit = KIND_VTX, corpus.find(n64)
            if hit is None:
                obj_kept += len(data)
                continue
            src, off = hit
            sym = f"__wiirom_{len(pieces)}"
            newname = f".bss.wiirom.{len(pieces)}"
            args += [f"--rename-section={name}={newname},alloc", f"--add-symbol={sym}={newname}:0,global"]
            used_sources.setdefault(src, len(used_sources))
            pieces.append((sym, src, off, len(data), sum(data), kind))
            obj_stripped += len(data)
        # everything else that boot.dol gets from this object (code, relocated data)
        other = sum(s[5] for i, s in enumerate(elf.secs)
                    if s[1] == SHT_PROGBITS and s[2] & SHF_ALLOC and elf.names[i].startswith(DATA_PREFIXES)) \
            - obj_stripped - obj_kept
        stripped += obj_stripped
        kept += obj_kept
        report.append(f"{rel}: {obj_stripped} bytes from the ROM, {obj_kept} kept (no relocations), "
                      f"{other} kept (relocated data)")
        if args:
            with tempfile.NamedTemporaryFile("w", suffix=".args", delete=False) as f:
                f.write("\n".join(args))
            try:
                subprocess.run([a.objcopy, f"@{f.name}", obj, dst], check=True)
            finally:
                os.unlink(f.name)
        else:
            shutil.copyfile(obj, dst)

    # sources in ROM order (raw first), so the game reads the ROM front to back
    order = sorted(used_sources, key=lambda s: corpus.sources[s][0])
    index = {s: k for k, s in enumerate(order)}
    pieces.sort(key=lambda p: (index[p[1]], p[2]))
    total_sum = sum(p[4] for p in pieces) & 0xFFFFFFFF
    with open(a.table, "w") as f:
        f.write("/* Generated by wii/tools/strip_rom_data.py: where each piece of boot.dol's ROM data is\n"
                " * in the ROM (offsets and sizes only). See src/pc/romdata.c. */\n"
                "#include \"pc/romdata.h\"\n\n")
        for p in pieces:
            f.write(f"extern char {p[0]}[];\n")
        f.write(f"\nconst unsigned int gWiiRomCrc[2] = {{ 0x{crc1:08X}, 0x{crc2:08X} }};\n")
        f.write(f"const unsigned int gWiiRomDataSum = 0x{total_sum:08X};\n")
        f.write(f"const unsigned int gWiiRomSourceCount = {len(order)};\n")
        f.write(f"const unsigned int gWiiRomPieceCount = {len(pieces)};\n")
        f.write("const struct WiiRomSource gWiiRomSources[] = {\n")
        for s in order:
            off, clen, dlen = corpus.sources[s]
            f.write(f"    {{ 0x{off:X}, 0x{clen:X}, 0x{dlen:X}, {int(s != 0)} }},\n")
        f.write("    { 0, 0, 0, 0 }\n};\n")
        f.write("const struct WiiRomPiece gWiiRomPieces[] = {\n")
        for sym, src, off, size, _, kind in pieces:
            f.write(f"    {{ {sym}, {index[src]}, {kind}, 0x{off:X}, 0x{size:X} }},\n")
        f.write("    { 0, 0, 0, 0, 0 }\n};\n")
    summary = (f"strip_rom_data: {stripped} bytes in {len(pieces)} pieces (from {len(order)} ROM segments) "
               f"now come from the ROM at boot; {kept} bytes of relocation-free asset data stay in boot.dol")
    print(summary)
    if a.report:
        with open(a.report, "w") as f:
            f.write(summary + "\n" + "\n".join(report) + "\n")


if __name__ == "__main__":
    main()
