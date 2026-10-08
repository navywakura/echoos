#!/usr/bin/env python3
"""psf2c — convert a PSF console font to kernel/font8x16.h (8x16 glyphs).

Emits a 256-entry table indexed by **ISO-8859-1 (Latin-1) code point**, using the
font's Unicode table so that byte 0xE1 renders 'a-acute', 0xF1 'n-tilde', 0xBF
'inverted-?', etc. This lets the OS use Latin-1 bytes internally for accented
Spanish text and have them render correctly.

Usage: python3 scripts/psf2c.py /usr/share/consolefonts/Lat15-VGA16.psf.gz
Fedora: dnf install kbd; fonts under /usr/lib/kbd/consolefonts/.
"""
import gzip, struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/consolefonts/Lat15-VGA16.psf.gz"
data = gzip.open(path, "rb").read() if path.endswith(".gz") else open(path, "rb").read()

uni_map = {}   # unicode codepoint -> glyph index

if data[:2] == b"\x36\x04":                      # PSF1
    mode, charsize = data[2], data[3]
    nglyphs = 512 if (mode & 0x01) else 256
    height, width = charsize, 8
    off = 4
    glyph_bytes = nglyphs * charsize
    if mode & 0x06:                              # has unicode table
        tab = data[off + glyph_bytes:]
        gi, i = 0, 0
        while i + 1 < len(tab) and gi < nglyphs:
            v = tab[i] | (tab[i + 1] << 8); i += 2
            if v == 0xFFFF:
                gi += 1
            elif v == 0xFFFE:
                pass                              # sequence separator
            else:
                uni_map.setdefault(v, gi)
else:                                            # PSF2
    magic, ver, hs, flags, nglyphs, charsize, height, width = struct.unpack("<8I", data[:32])
    off = hs
    glyph_bytes = nglyphs * charsize
    if flags & 1:
        tab = data[off + glyph_bytes:]
        gi, i = 0, 0
        while i < len(tab) and gi < nglyphs:
            b = tab[i]
            if b == 0xFF:
                gi += 1; i += 1
            elif b == 0xFE:
                i += 1
            else:
                if b < 0x80: cp, n = b, 1
                elif b < 0xE0: cp, n = b & 0x1F, 2
                elif b < 0xF0: cp, n = b & 0x0F, 3
                else: cp, n = b & 0x07, 4
                for k in range(1, n):
                    cp = (cp << 6) | (tab[i + k] & 0x3F)
                uni_map.setdefault(cp, gi)
                i += n

assert width == 8 and height == 16, f"{width}x{height}, need 8x16"
sys.stderr.write(f"{path}: 8x16, {nglyphs} glyphs, {len(uni_map)} unicode entries\n")


def glyph(gi):
    return data[off + gi * charsize: off + gi * charsize + 16]


with open("kernel/font8x16.h", "w") as f:
    f.write("/* 8x16 font (Latin-1 indexed) from %s. Regenerate: scripts/psf2c.py */\n"
            % path.split("/")[-1])
    f.write("#ifndef ROGEX_FONT8X16_H\n#define ROGEX_FONT8X16_H\n")
    f.write("static const unsigned char FONT8X16[256][16] = {\n")
    for cp in range(256):
        gi = uni_map.get(cp, cp if cp < nglyphs else 0)
        g = glyph(gi) if gi < nglyphs else b"\x00" * 16
        f.write("  {" + ",".join(str(b) for b in g) + "},\n")
    f.write("};\n#endif\n")
sys.stderr.write("wrote kernel/font8x16.h (Latin-1 indexed)\n")
