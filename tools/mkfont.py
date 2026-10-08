"""Rasterise JetBrains Mono into the kernel's glyph tables.

The kernel cannot parse TrueType: no FPU, no allocator on that path, and a
hinting engine is not a thing a console should carry. So the outlines are
rasterised here, on the host, into fixed-size cells and emitted as C arrays.

Antialiased: a 1-bit console font at 16 px is what makes a terminal look
like 1994. Coverage is packed at 4 bits per pixel — sixteen levels, which at
this size is indistinguishable from 256 and costs half the .rodata. The
kernel blends with one multiply per pixel and no floating point.

Three faces — regular, bold, italic — for the ranges where a weight means
something, and regular only for the symbol blocks (a bold box-drawing corner
is not a thing). Codepoints are looked up through a sorted range table, so
coverage is not limited to 256 Latin-1 slots.

No single face covers everything a console prints. JetBrains Mono has the
text, the box drawing and the block elements; it has no arrows past a
handful, half the maths, and no Braille at all. So each codepoint is taken
from the first face that really has it — asking for a glyph a face lacks
gives you its .notdef box, which is ink, so "did it draw something" is not
the test; the raster is compared against that box. Braille is not in any
face here and does not need one: the pattern *is* the low eight bits of the
codepoint, so those 256 cells are drawn from the definition.

Anything no face has becomes U+FFFD, never a blank cell: a missing glyph
should look missing.

The output is committed to the tree. The build never runs this: it would tie
a reproducible ISO to whichever fonts happen to be installed on the machine
doing the build.

Usage:
  python3 tools/mkfont.py --out drivers/video/font_jbmono.h
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H, PX, BASELINE = 10, 22, 16, 17

FONT_DIR = Path.home() / ".local/share/fonts/JetBrainsMonoNerd"
FACES = {
    "regular": "JetBrainsMonoNerdFontMono-Regular.ttf",
    "bold":    "JetBrainsMonoNerdFontMono-Bold.ttf",
    "italic":  "JetBrainsMonoNerdFontMono-Italic.ttf",
}
# Second choice, by style. DejaVu Sans Mono ships with every distribution and
# covers the arrows, geometry and symbols JetBrains Mono leaves out.
FALLBACK = {
    "regular": "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "bold":    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf",
    "italic":  "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Oblique.ttf",
}
REPLACEMENT = 0xFFFD

# (first, last, styled) — styled ranges get all three faces; the rest are
# symbols where a bold or slanted variant would be noise, and share regular.
RANGES = [
    (0x0020, 0x007E, True,  "ASCII"),
    (0x00A0, 0x00FF, True,  "Latin-1 supplement"),
    (0x0100, 0x017F, True,  "Latin Extended-A"),
    (0x0192, 0x0192, True,  "florin"),
    (0x0391, 0x03C9, True,  "Greek (maths and physics text)"),
    (0x2010, 0x2027, True,  "general punctuation"),
    (0x2030, 0x205E, True,  "punctuation, per-mille, primes"),
    (0x20A0, 0x20BF, True,  "currency signs"),
    (0x2116, 0x2122, True,  "numero, ohm, trademark"),
    (0x2190, 0x21FF, False, "arrows"),
    (0x2200, 0x22FF, False, "mathematical operators"),
    (0x2300, 0x231F, False, "misc technical (power, corners)"),
    (0x2500, 0x257F, False, "box drawing"),
    (0x2580, 0x259F, False, "block elements"),
    (0x25A0, 0x25FF, False, "geometric shapes"),
    (0x2600, 0x26FF, False, "misc symbols"),
    (0x2700, 0x27BF, False, "dingbats (check, cross, stars)"),
    (0x27F0, 0x27FF, False, "supplemental arrows"),
    (0x2800, 0x28FF, False, "Braille patterns"),
    (0xFFFD, 0xFFFD, False, "replacement character"),
]

# Braille dot numbering: bits 0-2 are the left column top to bottom, 3-5 the
# right column, 6 and 7 the fourth row (left, right).
BRAILLE_DOTS = ((0, 0, 0), (1, 0, 1), (2, 0, 2), (3, 1, 0),
                (4, 1, 1), (5, 1, 2), (6, 0, 3), (7, 1, 3))


def pack(img):
    """Two pixels per byte, high nibble first."""
    px = img.tobytes()
    out = bytearray()
    for i in range(0, len(px), 2):
        out.append(((px[i] >> 4) << 4) | (px[i + 1] >> 4))
    return bytes(out)


def raster(font, cp):
    img = Image.new("L", (CELL_W, CELL_H), 0)
    ImageDraw.Draw(img).text((0, BASELINE), chr(cp), font=font, fill=255,
                             anchor="ls")
    return img


def braille(cp):
    """Draw the dot pattern the codepoint literally encodes, 4x supersampled."""
    S = 4
    img = Image.new("L", (CELL_W * S, CELL_H * S), 0)
    d = ImageDraw.Draw(img)
    bits = cp - 0x2800
    r = 1.6 * S
    for bit, col, row in BRAILLE_DOTS:
        if not (bits >> bit) & 1:
            continue
        cx = (2.7 + col * 4.6) * S
        cy = (4.5 + row * 4.6) * S
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    return img.resize((CELL_W, CELL_H), Image.LANCZOS)


def notdef_of(font):
    """The face's own .notdef box, so 'it drew ink' is not mistaken for 'it
       has the glyph' — every face draws a box for what it lacks."""
    return raster(font, 0x10FFFD).tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--font-dir", type=Path, default=FONT_DIR)
    args = ap.parse_args()

    fonts = {}
    for style, name in FACES.items():
        path = args.font_dir / name
        if not path.exists():
            raise SystemExit(f"missing {path}\n"
                             "JetBrains Mono Nerd Font is not installed here.")
        fonts[style] = ImageFont.truetype(str(path), PX)

    fallbacks = {}
    for style, path in FALLBACK.items():
        if Path(path).exists():
            fallbacks[style] = ImageFont.truetype(path, PX)
    notdef = {k: notdef_of(v) for k, v in fonts.items()}
    fb_notdef = {k: notdef_of(v) for k, v in fallbacks.items()}

    def cell_for(style, cp):
        """First face that really has this codepoint, else None."""
        if 0x2800 <= cp <= 0x28FF:
            return braille(cp), "braille"
        img = raster(fonts[style], cp)
        if img.tobytes() != notdef[style] or cp == 0x20:
            return img, "jetbrains"
        if style in fallbacks:
            img = raster(fallbacks[style], cp)
            if img.tobytes() != fb_notdef[style]:
                return img, "dejavu"
        return None, None

    glyphs = {"regular": [], "bold": [], "italic": []}
    table = []          # (first, last, regular base, styled base, label)
    source_count = {"jetbrains": 0, "dejavu": 0, "braille": 0, "replacement": 0}

    replacement = {}
    for style in ("regular", "bold", "italic"):
        img, _ = cell_for(style, REPLACEMENT)
        replacement[style] = img if img else Image.new("L", (CELL_W, CELL_H), 0)

    for first, last, styled, label in RANGES:
        base = len(glyphs["regular"])
        # Symbols share the regular shape for every style; the table says so
        # with a styled base of JB_NO_STYLE, and the kernel falls back rather
        # than indexing past the end of the styled arrays.
        sbase = len(glyphs["bold"]) if styled else None
        for cp in range(first, last + 1):
            img, src = cell_for("regular", cp)
            if img is None:
                img, src = replacement["regular"], "replacement"
            source_count[src] += 1
            glyphs["regular"].append(pack(img))
            if styled:
                for style in ("bold", "italic"):
                    simg, _ = cell_for(style, cp)
                    glyphs[style].append(pack(simg if simg else replacement[style]))
        table.append((first, last, base, sbase, label))

    styled_count = sum(1 for g in glyphs["bold"])
    total = len(glyphs["regular"])

    out = []
    w = out.append
    w("/*\n * JetBrains Mono, rasterised for the echOS console.\n *\n"
      " * Generated by tools/mkfont.py — do not edit by hand. Regenerate with:\n"
      " *   python3 tools/mkfont.py --out drivers/video/font_jbmono.h\n *\n"
      f" * Cell {CELL_W}x{CELL_H} px, {PX} px face, baseline at row {BASELINE}.\n"
      " * Two pixels per byte, high nibble first: 0 = background, 15 = full\n"
      " * ink. Scale a nibble to 8 bits with v * 17.\n"
      " *\n"
      " * JetBrains Mono is licensed under the SIL Open Font License 1.1.\n"
      " * Nerd Font patches by Ryan McIntyre, same licence.\n */\n")
    w("#ifndef ECHOS_FONT_JBMONO_H\n#define ECHOS_FONT_JBMONO_H\n")
    w("#include \"rogex.h\"\n")
    w(f"\n#define JB_CELL_W {CELL_W}\n#define JB_CELL_H {CELL_H}\n"
      f"#define JB_CELL_BYTES {CELL_W * CELL_H // 2}\n"
      f"#define JB_GLYPHS {total}\n#define JB_STYLED_GLYPHS {styled_count}\n")
    w("\n#define JB_NO_STYLE 0xFFFFFFFFu\n")
    w("typedef struct { uint32_t first, last, base, styled_base; } jb_range_t;\n")

    w(f"\nstatic const jb_range_t JB_RANGES[] = {{\n")
    for first, last, base, sbase, label in table:
        sb = "JB_NO_STYLE" if sbase is None else f"{sbase:5d}"
        w(f"    {{ 0x{first:04X}, 0x{last:04X}, {base:5d}, {sb} }},"
          f"  /* {label} */\n")
    w("};\n")
    w(f"#define JB_RANGE_COUNT {len(table)}\n")

    for style in ("regular", "bold", "italic"):
        data = glyphs[style]
        w(f"\nstatic const uint8_t JB_{style.upper()}[{len(data)}]"
          f"[JB_CELL_BYTES] = {{\n")
        for cell in data:
            w("{" + ",".join(str(b) for b in cell) + "},\n")
        w("};\n")

    w("\n#endif /* ECHOS_FONT_JBMONO_H */\n")
    args.out.write_text("".join(out))

    size = args.out.stat().st_size
    print(f"[mkfont] {total} glyphs ({styled_count} in three weights) "
          f"-> {args.out} ({size // 1024} KiB of source)")
    for src, n in source_count.items():
        print(f"[mkfont]   {n:5d} from {src}")


if __name__ == "__main__":
    main()
