// font1252.h -- bitmap glyphs for the printable Windows-1252 flap set.
//
// The virtual reels carry every printable CP1252 glyph (216 of them; see
// isFlapByte in charset.h) plus seven colour flaps. Colour flaps are painted as
// solid swatches and need no glyph, so only the 216 characters live here.
//
// Adafruit_GFX's built-in face was CP437 and its GFXfonts are ASCII-range, so the
// tables are generated instead -- by tools/genfont.py, from the public-domain
// X11 "misc-fixed" BDFs vendored under tools/bdf/. Regenerate with:
//
//     python3 tools/genfont.py
//
// Four sizes are bundled. The cell size falls out of the panel geometry and the
// module grid (see display.h), so the renderer asks font1252Best() for the
// largest face that fits and the same binary drives an 8x10 cell on a 128x32
// panel or an 8x21 cell on a 128x64 one.

#ifndef MPGW_FONT1252_H
#define MPGW_FONT1252_H

#include <stdint.h>

#define FONT1252_GLYPHS 216   // printable CP1252 glyphs (see tools/genfont.py)
#define FONT1252_COUNT    4   // bundled faces

// One bitmap face. `rows` is FONT1252_GLYPHS * height bytes, glyph-major; within
// a glyph, one byte per row top-to-bottom, bit 7 = leftmost column. `ascent` is
// the number of rows above the baseline -- the renderer only needs it to sit
// accented capitals and descenders correctly inside a cell.
struct Font1252 {
  uint8_t        width;
  uint8_t        height;
  uint8_t        ascent;
  const uint8_t* rows;
};

// Bundled faces. 5x7 and 4x6 are deliberately absent: at those sizes the source
// face has no room for diacritics and draws e.g. 'A-grave' identically to 'A',
// which on a reel carrying the whole CP1252 set is a correctness bug, not just an
// aesthetic one. tools/genfont.py rejects any face that does this.
extern const Font1252 FONT_6x13;   // roomiest -- an 8x21 cell on a 128x64 panel
extern const Font1252 FONT_6x10;
extern const Font1252 FONT_6x9;    // the 8x10 cell of a 15x3 wall on a 128x32
extern const Font1252 FONT_5x8;    // smallest face that still carries accents

// Largest face first.
extern const Font1252* const FONT1252_ALL[FONT1252_COUNT];

// CP1252 byte -> glyph index, or 0xFF when the byte carries no printable glyph.
extern const uint8_t FONT1252_INDEX[256];

// Bitmap row `r` of glyph `gi` (bit 7 = leftmost pixel). Returns 0 out of range.
static inline uint8_t font1252Row(const Font1252& f, uint8_t gi, uint8_t r) {
  if (gi >= FONT1252_GLYPHS || r >= f.height) return 0;
  return f.rows[(uint16_t)gi * f.height + r];
}

// The roomiest bundled face that fits inside a cellW x cellH cell while leaving a
// one-pixel gutter on each axis, so neighbouring flaps' glyphs never touch. Falls
// back to the smallest face when nothing fits (a cramped glyph beats a blank
// cell), so this never returns NULL.
const Font1252* font1252Best(uint8_t cellW, uint8_t cellH);

#endif // MPGW_FONT1252_H
