#!/usr/bin/env python3
"""Pack one BDF face into an .mpft blob for PUT /api/canvas/font.

The gateway can carry ONE custom Font1252 face in PSRAM (the "custom" font of the
ticker and the canvas text op) and a library of them on FATFS (/fonts/<name>.fnt).
This tool builds the wire blob from a BDF bitmap font:

    "MPFT"(4)  ver(1)=1  width(1)  height(1)  ascent(1)
    then 216 glyphs of `height` big-endian uint16 rows, bit 15 = leftmost column,
    in the CP1252 order FONT1252_INDEX implies.

The glyph repertoire, ordering and row packing are genfont.py's, imported and
reused rather than copied -- the 216 printable CP1252 glyphs, in ascending byte
order, rendered exactly as the bundled faces are. The FONT_EXTRA pictographs are
firmware-only and are NOT part of the blob (the on-device table zero-pads them).
Faces wider than 16 px cannot be packed (one uint16 per row).

Usage:  python3 tools/fontpack.py in.bdf out.mpft
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import genfont  # noqa: E402  (the glyph enumeration + BDF renderer live there)


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} in.bdf out.mpft")
    inp, outp = sys.argv[1], sys.argv[2]

    font_bb, glyphs = genfont.parse_bdf(inp)
    bbw, bbh, _bbx, bbyoff = font_bb
    ascent = bbh + bbyoff
    if bbw < 1 or bbw > 16:
        sys.exit(f"{inp}: face is {bbw}px wide -- the format packs one uint16 "
                 f"per row, max 16px")
    if not (1 <= bbh <= 255 and 0 <= ascent <= bbh):
        sys.exit(f"{inp}: implausible metrics (height {bbh}, ascent {ascent})")

    printable = genfont.cp1252_printable()
    missing = [f"U+{cp:04X}" for _b, cp in printable if cp not in glyphs]
    if missing:
        sys.exit(f"{inp}: missing {len(missing)} CP1252 glyphs: "
                 f"{missing[:8]}{'...' if len(missing) > 8 else ''}")

    data = []
    for _byte, cp in printable:
        data.extend(genfont.render(font_bb, glyphs[cp], bbw, bbh))

    # genfont's quality gate (accents must differ from their base letters, no ink
    # outside the box...). A face that fails would still DISPLAY, so warn, don't
    # refuse -- the user may be packing a deliberately odd face.
    index = [0xFF] * 256
    for gi, (byte, _cp) in enumerate(printable):
        index[byte] = gi
    try:
        genfont.validate(inp, printable, index, bbw, bbh, data)
    except SystemExit as e:
        print(f"WARNING: {e}", file=sys.stderr)

    blob = b"MPFT" + bytes([1, bbw, bbh, ascent])
    blob += b"".join(struct.pack(">H", row) for row in data)
    assert len(blob) == 8 + len(printable) * bbh * 2

    with open(outp, "wb") as f:
        f.write(blob)
    print(f"{outp}: {len(printable)} glyphs, {bbw}x{bbh}, ascent {ascent}, "
          f"{len(blob)} bytes")


if __name__ == "__main__":
    main()
