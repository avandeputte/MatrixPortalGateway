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
# Largest first: font1252Best() returns the FIRST face that fits the cell, so the order
# here is the preference order. 10x20 and 9x18 exist for the big cells a 256px-wide chain
# makes possible (a 15x3 wall on 256x64 is 17x21 px per module -- three times the area of
# the 8x12 cells a 128-wide chain gives you, and 6x13 simply floats in that much room).
FONTS = ["10x20", "9x18", "8x13", "6x13", "6x10", "6x9", "5x8"]

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


# ---------------------------------------------------------------------------
# The EXTRA glyphs: the pictographs the reel carries beyond Windows-1252.
#
# CP1252 has 256 slots and the split-flap wire protocol carries exactly one byte
# per character, so a heart simply has nowhere to live in the legacy protocol.
# These flaps are therefore reachable only by INDEX -- which is what the new
# /api/display/cells endpoint uses, and why that endpoint exists at all.
#
# They are NOT hand-drawn. The vendored X11 "misc-fixed" BDFs already carry
# thousands of glyphs, these among them, so they are generated from exactly the
# same source as every letter. The one exception is the sun at 5x8, which that
# face genuinely lacks -- see FALLBACK below.
#
# (codepoint, name, colour)
#
#   `colour` names one of the reel's SEVEN COLOUR FLAPS -- it is not a bespoke RGB. A heart
#   is drawn in exactly the red the red flap uses, so "red" means one thing on this wall.
#
#   Hand-picking an RGB is how the heart came out PURPLE the first time: (0xE0,0x30,0x40)
#   reads as red on a monitor, but on a HUB75 panel the blue LEDs are far more efficient per
#   unit of duty cycle than the red ones, so those 64 counts of blue dragged it to magenta.
#   The palette already solved that problem; do not solve it again, differently.
#
#   None = drawn in the normal warm split-flap white, like a letter.
#   The order below MUST match REEL_COLOUR_NAMES in src/reel.h (r o y g b p w).
EXTRA_COLOURS = ["red", "orange", "yellow", "green", "blue", "purple", "white"]
EXTRA_GLYPHS = [
    (0x2665, "heart",   "red"),
    (0x2666, "diamond", "red"),      # a card suit: red, like the heart
    (0x2663, "club",    None),
    (0x2660, "spade",   None),
    (0x263A, "smiley",  "yellow"),
    (0x266A, "note",    None),
    (0x25CF, "circle",  None),
    (0x25A0, "square",  None),
    (0x2302, "house",   None),
    (0x2190, "left",    None),
    (0x2191, "up",      None),
    (0x2192, "right",   None),
    (0x2193, "down",    None),
    (0x2600, "sun",     "yellow"),
]

# The only glyph any bundled face is missing. 5x8 has no U+2600, so it is drawn
# here rather than dropping the sun on small panels. Rows are 16-bit and
# left-aligned (bit 15 = leftmost column), like every other glyph; 5 columns used.
#         ..#..   #...#   .###.   #####   .###.   #...#   ..#..
FALLBACK = {
    ("5x8", 0x2600): [0x2000, 0x8800, 0x7000, 0xF800, 0x7000, 0x8800, 0x2000, 0x0000],
}


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

    Returns `height` 16-BIT rows, bit 15 = leftmost column. One byte per row used to be
    enough, and it capped every face at 8 pixels wide -- which is why the biggest bundled
    face was 6x13 no matter how much room a cell had. Sixteen bits admits 10x20 and 9x18.
    (render() raised loudly on a wider glyph rather than dropping its right-hand columns,
    which is the only reason that limit never shipped as a silent corruption.)

    BDF puts the origin on the
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
    if bw > 16:
        raise SystemExit("glyph wider than 16px -- packing assumes one uint16 per row")
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
                    rows[r] |= 0x8000 >> c
    return rows


def validate(name, printable, index, width, height, data):
    """Reject a face that would render the reel wrong. Raises SystemExit."""
    def rows_of(byte):
        gi = index[byte]
        return data[gi * height:(gi + 1) * height]

    errs = []
    spill = 0xFFFF >> width if width < 16 else 0  # bits to the right of the glyph box
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
        # ...then the extra pictographs, at glyph indices count..count+len(EXTRA)-1.
        n_fb = 0
        for cp, gname, _col in EXTRA_GLYPHS:
            if cp in glyphs:
                data.extend(render(font_bb, glyphs[cp], bbw, bbh))
            elif (name, cp) in FALLBACK:
                rows = FALLBACK[(name, cp)]
                if len(rows) != bbh:
                    raise SystemExit(f"{name}: fallback for U+{cp:04X} is "
                                     f"{len(rows)} rows, face is {bbh}")
                data.extend(rows)
                n_fb += 1
            else:
                raise SystemExit(f"{name}: no glyph for U+{cp:04X} ({gname}) and no "
                                 f"FALLBACK -- add one, or drop it from EXTRA_GLYPHS")
        blobs.append((name, bbw, bbh, bbh + bbyoff, data))
        print(f"  {name}: {count} glyphs + {len(EXTRA_GLYPHS)} extra "
              f"({n_fb} hand-drawn), {bbw}x{bbh}, ascent {bbh + bbyoff}, "
              f"{len(data)} bytes", file=sys.stderr)

    with open(OUT_CPP, "w") as out:
        out.write(f"""// font1252.cpp -- GENERATED by tools/genfont.py. Do not edit by hand.
//
// Bitmaps for the {count} printable Windows-1252 glyphs the virtual split-flap
// reels carry, at seven sizes. The renderer picks the largest that fits a cell
// (see font1252Best), so the same firmware drives a cramped 8x10 cell on a
// 128x32 panel and a roomy 8x21 cell on a 128x64 one.
//
// Source: X11 "misc-fixed" bitmap fonts, ISO10646-1 -- "Public domain font.
// Share and enjoy." The BDFs are vendored under tools/bdf/.
//
// Row packing: one uint16 per row, bit 15 = leftmost column. (One byte capped every face\n// at 8 pixels wide, which is why nothing bigger than 6x13 used to be possible.)

#include "font1252.h"

// CP1252 byte -> glyph index; 0xFF where the byte carries no printable glyph
// (C0/C1 controls, the five undefined 0x80-0x9F slots, NBSP, soft hyphen).
const uint8_t FONT1252_INDEX[256] = {{
""")
        for r in range(0, 256, 16):
            out.write("  " + " ".join(f"0x{v:02X}," for v in index[r:r + 16]) + "\n")
        out.write("};\n")

        total = count + len(EXTRA_GLYPHS)
        for name, w, h, ascent, data in blobs:
            sym = name
            out.write(f"\nstatic const uint16_t F{sym}_ROWS[{total} * {h}] = {{\n")
            for gi in range(count):
                byte = printable[gi][0]
                rows = data[gi * h:(gi + 1) * h]
                out.write("  " + " ".join(f"0x{v:04X}," for v in rows)
                          + f"  // 0x{byte:02X}\n")
            out.write(f"  // --- the {len(EXTRA_GLYPHS)} extra pictographs "
                      f"(glyph index {count}..{total - 1}) ---\n")
            for k, (cp, gname, _col) in enumerate(EXTRA_GLYPHS):
                gi = count + k
                rows = data[gi * h:(gi + 1) * h]
                out.write("  " + " ".join(f"0x{v:04X}," for v in rows)
                          + f"  // U+{cp:04X} {gname}\n")
            out.write("};\n")
            out.write(f"const Font1252 FONT_{sym} = {{ {w}, {h}, {ascent}, F{sym}_ROWS }};\n")

        # The extras' identity, so reel.h can find a flap for a code point without
        # a second copy of this list. THIS is the single source of truth.
        out.write(f"\n// The extra pictographs, in glyph-index order from FONT_EXTRA_BASE.\n")
        out.write(f"const uint32_t FONT_EXTRA_CP[FONT_EXTRA_COUNT] = {{\n")
        for cp, gname, _col in EXTRA_GLYPHS:
            out.write(f"  0x{cp:04X},   // {gname}\n")
        out.write("};\n")
        out.write(f"const char* const FONT_EXTRA_NAME[FONT_EXTRA_COUNT] = {{\n")
        out.write("  " + ", ".join(f'"{g}"' for _cp, g, _c in EXTRA_GLYPHS) + "\n};\n")
        out.write("// Which COLOUR FLAP each pictograph is drawn in -- an index into the\n"
                  "// r/o/y/g/b/p/w palette, or -1 for the normal warm split-flap white.\n"
                  "// NOT an RGB: a heart is the red flap's red, so red means one thing here.\n")
        out.write(f"const int8_t FONT_EXTRA_COLOUR[FONT_EXTRA_COUNT] = {{\n")
        for cp, gname, col in EXTRA_GLYPHS:
            idx = EXTRA_COLOURS.index(col) if col else -1
            out.write(f"  {idx:>2},   // {gname:<8}{' -> ' + col if col else ''}\n")
        out.write("};\n")

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
