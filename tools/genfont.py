#!/usr/bin/env python3
"""Generate src/font1252.cpp -- the Windows-1252 flap glyph tables.

The virtual split-flap modules carry every printable CP1252 glyph on the reel,
so the renderer needs a bitmap for each of them at several sizes (the cell size
falls out of panel geometry / grid, and the largest font that fits is chosen at
runtime). Adafruit_GFX ships only a CP437 5x7 face and ASCII-range GFXfonts,
neither of which covers CP1252, so the tables are generated here instead.

Source: the X11 "misc-fixed" bitmap fonts, ISO10646-1 encoded, which state
"Public domain font.  Share and enjoy." in their COPYRIGHT property. The BDFs
are vendored under tools/bdf/ so this script needs no network.

Run from the project root:  python3 tools/genfont.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BDF_DIR = os.path.join(HERE, "bdf")
OUT_CPP = os.path.join(ROOT, "src", "font1252.cpp")

# Fonts to bundle, largest first. `name` becomes the C symbol suffix.
#
# 5x7 and 4x6 are deliberately absent: at those sizes misc-fixed has no room for
# diacritics and draws Agrave, Scaron etc. *identically to their base letter*, so
# a reel showing the full CP1252 set would render A and A-grave the same. The
# accent check in validate() enforces this -- do not add a face back without
# running it.
FONTS = ["6x13", "6x10", "6x9", "5x8"]

# (accented byte, base byte) pairs that must NOT rasterise identically. Covers a
# grave, a diaeresis, a caron, a cedilla, an acute and a ring -- one per mark
# shape, at both cases, so a face that silently drops any of them is rejected.
ACCENT_CHECKS = [
    (0xC0, 0x41),  # Agrave    vs A
    (0xD6, 0x4F),  # Odieresis vs O
    (0xC5, 0x41),  # Aring     vs A
    (0xC7, 0x43),  # Ccedilla  vs C
    (0x8A, 0x53),  # Scaron    vs S
    (0x9E, 0x7A),  # scaron..zcaron vs z
    (0xE9, 0x65),  # eacute    vs e
    (0xFF, 0x79),  # ydieresis vs y
]

# Unicode code point for each CP1252 byte 0x80..0x9F (0 = undefined slot).
# Mirrors CP1252_HIGH in src/charset.cpp -- keep the two in step.
CP1252_HIGH = [
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
]


def cp1252_printable():
    """The CP1252 bytes that carry a printable glyph, ascending.

    Excludes the C0/C1 controls, the five undefined 0x80-0x9F slots, NBSP
    (0xA0) and the soft hyphen (0xAD) -- neither of which draws anything.
    Matches isFlapByte() in src/charset.cpp. 216 bytes.
    """
    out = [(b, b) for b in range(0x20, 0x7F)]
    out += [(0x80 + i, cp) for i, cp in enumerate(CP1252_HIGH) if cp]
    out += [(b, b) for b in range(0xA1, 0x100) if b != 0xAD]
    return sorted(out)


def parse_bdf(path):
    """Return (bbw, bbh, bbxoff, bbyoff, {codepoint: (bw, bh, bxoff, byoff, [rowints])})."""
    font_bb = None
    glyphs = {}
    enc = bbx = None
    bitmap = None
    with open(path, encoding="latin-1") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("FONTBOUNDINGBOX"):
                font_bb = tuple(int(v) for v in line.split()[1:5])
            elif line.startswith("ENCODING"):
                enc = int(line.split()[1])
            elif line.startswith("BBX"):
                bbx = tuple(int(v) for v in line.split()[1:5])
            elif line.startswith("BITMAP"):
                bitmap = []
            elif line.startswith("ENDCHAR"):
                if enc is not None and enc >= 0 and bbx is not None:
                    glyphs[enc] = (bbx[0], bbx[1], bbx[2], bbx[3], bitmap or [])
                enc = bbx = bitmap = None
            elif bitmap is not None and line:
                bitmap.append(line.strip())
    if font_bb is None:
        raise SystemExit(f"{path}: no FONTBOUNDINGBOX")
    return font_bb, glyphs


def render(font_bb, glyph, width, height):
    """Rasterise one BDF glyph into a fixed `width` x `height` cell.

    Returns `height` bytes, bit 7 = leftmost column. BDF puts the origin on the
    baseline with y growing upward, so a glyph's bitmap row j (0 = top) sits at
    y = byoff + bh - 1 - j. The cell's top row is ascent-1 above the baseline,
    hence cell row = ascent - byoff - bh + j.
    """
    bbw, bbh, bbxoff, bbyoff = font_bb
    ascent = bbh + bbyoff  # rows above the baseline
    rows = [0] * height
    if glyph is None:
        return rows
    bw, bh, bxoff, byoff, bits = glyph
    if bw > 8:
        raise SystemExit("glyph wider than 8px -- packing assumes one byte per row")
    # Each BDF bitmap row is hex, padded up to a whole number of bytes.
    for j, hexrow in enumerate(bits):
        val = int(hexrow, 16)
        nbits = len(hexrow) * 4
        r = ascent - byoff - bh + j
        if not (0 <= r < height):
            continue  # glyph overflows the box (rare); clip rather than corrupt
        for i in range(bw):
            # bit i of the row, counting from the MSB of the padded value
            if val & (1 << (nbits - 1 - i)):
                c = bxoff + i - bbxoff
                if 0 <= c < width:
                    rows[r] |= 0x80 >> c
    return rows


def validate(name, printable, index, width, height, data):
    """Reject a face that would render the reel wrong. Raises SystemExit."""
    def rows_of(byte):
        gi = index[byte]
        return data[gi * height:(gi + 1) * height]

    errs = []
    spill = 0xFF >> width if width < 8 else 0  # bits to the right of the glyph box
    for gi in range(len(printable)):
        if any(r & spill for r in data[gi * height:(gi + 1) * height]):
            errs.append(f"glyph 0x{printable[gi][0]:02X} has ink past column {width - 1}")

    blank = [b for b, _cp in printable if not any(rows_of(b))]
    if blank != [0x20]:
        errs.append(f"blank glyphs besides space: {[hex(b) for b in blank]}")

    if rows_of(0x41) == rows_of(0x61):
        errs.append("'A' and 'a' rasterise identically")

    for acc, base in ACCENT_CHECKS:
        if rows_of(acc) == rows_of(base):
            errs.append(f"0x{acc:02X} rasterises identically to its base 0x{base:02X} "
                        f"-- the face has no room for the diacritic")
    if errs:
        raise SystemExit(f"{name}: unusable face\n  " + "\n  ".join(errs))


def main():
    printable = cp1252_printable()
    count = len(printable)

    # byte -> glyph index, 0xFF for bytes with no printable glyph
    index = [0xFF] * 256
    for gi, (byte, _cp) in enumerate(printable):
        index[byte] = gi

    blobs = []
    for name in FONTS:
        path = os.path.join(BDF_DIR, f"{name}.bdf")
        font_bb, glyphs = parse_bdf(path)
        bbw, bbh, _x, bbyoff = font_bb
        missing = [hex(cp) for _b, cp in printable if cp not in glyphs]
        if missing:
            raise SystemExit(f"{name}: missing {len(missing)} glyphs: {missing[:8]}")
        data = []
        for _byte, cp in printable:
            data.extend(render(font_bb, glyphs[cp], bbw, bbh))
        validate(name, printable, index, bbw, bbh, data)
        blobs.append((name, bbw, bbh, bbh + bbyoff, data))
        print(f"  {name}: {count} glyphs, {bbw}x{bbh}, ascent {bbh + bbyoff}, "
              f"{len(data)} bytes", file=sys.stderr)

    with open(OUT_CPP, "w") as out:
        out.write(f"""// font1252.cpp -- GENERATED by tools/genfont.py. Do not edit by hand.
//
// Bitmaps for the {count} printable Windows-1252 glyphs the virtual split-flap
// reels carry, at four sizes. The renderer picks the largest that fits a cell
// (see font1252Best), so the same firmware drives a cramped 8x10 cell on a
// 128x32 panel and a roomy 8x21 cell on a 128x64 one.
//
// Source: X11 "misc-fixed" bitmap fonts, ISO10646-1 -- "Public domain font.
// Share and enjoy." The BDFs are vendored under tools/bdf/.
//
// Row packing: one byte per row, bit 7 = leftmost column.

#include "font1252.h"

// CP1252 byte -> glyph index; 0xFF where the byte carries no printable glyph
// (C0/C1 controls, the five undefined 0x80-0x9F slots, NBSP, soft hyphen).
const uint8_t FONT1252_INDEX[256] = {{
""")
        for r in range(0, 256, 16):
            out.write("  " + " ".join(f"0x{v:02X}," for v in index[r:r + 16]) + "\n")
        out.write("};\n")

        for name, w, h, ascent, data in blobs:
            sym = name.replace("x", "x")
            out.write(f"\nstatic const uint8_t F{sym}_ROWS[{count} * {h}] = {{\n")
            for gi in range(count):
                byte = printable[gi][0]
                rows = data[gi * h:(gi + 1) * h]
                out.write("  " + " ".join(f"0x{v:02X}," for v in rows)
                          + f"  // 0x{byte:02X}\n")
            out.write("};\n")
            out.write(f"const Font1252 FONT_{sym} = {{ {w}, {h}, {ascent}, F{sym}_ROWS }};\n")

        out.write(f"""
// Largest-first, so font1252Best() returns the roomiest face that fits.
const Font1252* const FONT1252_ALL[FONT1252_COUNT] = {{
  {", ".join("&FONT_" + n for n, *_ in blobs)}
}};

const Font1252* font1252Best(uint8_t cellW, uint8_t cellH) {{
  // Reserve one pixel on each axis: that gutter is the seam drawn between
  // adjacent flaps, and without it neighbouring glyphs touch.
  for (int i = 0; i < FONT1252_COUNT; i++) {{
    const Font1252* f = FONT1252_ALL[i];
    if (f->width + 1 <= cellW && f->height + 1 <= cellH) return f;
  }}
  return FONT1252_ALL[FONT1252_COUNT - 1];   // nothing fits: cramped beats blank
}}
""")
    print(f"wrote {OUT_CPP}", file=sys.stderr)


if __name__ == "__main__":
    main()
