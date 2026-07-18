// font1252.h -- bitmap glyphs for the printable Windows-1252 flap set.
//
// The virtual reel carries every printable CP1252 glyph (216 of them; see
// isFlapByte in charset.h) plus seven colour flaps and the pictographs. Colour
// flaps are painted as solid swatches and need no glyph, so the tables hold the
// 216 characters plus the FONT_EXTRA_* pictographs appended after them.
//
// Adafruit_GFX's built-in face was CP437 and its GFXfonts are ASCII-range, so the
// tables are generated instead -- by tools/genfont.py, from the public-domain
// X11 "misc-fixed" BDFs vendored under tools/bdf/. Regenerate with:
//
//     python3 tools/genfont.py
//
// Seven sizes are bundled. The cell size falls out of the panel geometry and the
// module grid (see display.h), so the renderer asks font1252Best() for the
// largest face that fits and the same binary drives an 8x10 cell on a 128x32
// panel or an 8x21 cell on a 128x64 one.

#ifndef MPGW_FONT1252_H
#define MPGW_FONT1252_H

#include <stdint.h>

#define FONT1252_GLYPHS 216   // printable CP1252 glyphs (see tools/genfont.py)
#define FONT1252_COUNT    7   // bundled faces: 10x20 9x18 8x13 6x13 6x10 6x9 5x8

// The EXTRA pictographs, beyond Windows-1252: heart, smiley, arrows, card suits...
// They live at glyph indices FONT_EXTRA_BASE .. +FONT_EXTRA_COUNT-1 in the same row
// tables, so the renderer needs no second code path -- only a glyph index.
//
// They exist because the wire protocol carries ONE BYTE per character and CP1252 has
// no heart. These flaps are reachable only by INDEX, which is exactly what the
// /api/display/cells endpoint sends -- and the reason that endpoint exists.
//
// FONT_EXTRA_CP is the single source of truth for which pictographs exist: reel.h
// derives its flap layout from it, rather than keeping a second list that could drift.
#define FONT_EXTRA_COUNT 14
#define FONT_EXTRA_BASE  FONT1252_GLYPHS

extern const uint32_t    FONT_EXTRA_CP[FONT_EXTRA_COUNT];      // U+2665, ...
extern const char* const FONT_EXTRA_NAME[FONT_EXTRA_COUNT];    // "heart", ...
// Which of the reel's seven colour flaps each pictograph is drawn in (0..6 = r o y g b p w),
// or -1 for the normal warm split-flap white. NOT an RGB, deliberately: a heart is drawn in
// exactly the red the RED FLAP uses, so "red" means one thing on this wall. Hand-picking an
// RGB is how the heart first came out purple -- blue LEDs are far more efficient per unit of
// duty on a HUB75 panel, so a mere 64 counts of blue dragged a near-red to magenta.
extern const int8_t      FONT_EXTRA_COLOUR[FONT_EXTRA_COUNT];

// One bitmap face. `rows` is FONT1252_TOTAL * height uint16 entries, glyph-major;
// within a glyph, one row per entry top-to-bottom, bit 15 = leftmost column.
// `ascent` is the number of rows above the baseline -- the renderer only needs it
// to sit accented capitals and descenders correctly inside a cell.
// Rows are 16-BIT, bit 15 = leftmost column. One byte per row used to be enough, and it
// silently capped every face at 8 pixels wide -- which is the real reason nothing bigger
// than 6x13 was ever bundled, however much room a cell had.
struct Font1252 {
  uint8_t         width;
  uint8_t         height;
  uint8_t         ascent;
  const uint16_t* rows;
};

// Bundled faces. 5x7 and 4x6 are deliberately absent: at those sizes the source
// face has no room for diacritics and draws e.g. 'A-grave' identically to 'A',
// which on a reel carrying the whole CP1252 set is a correctness bug, not just an
// aesthetic one. tools/genfont.py rejects any face that does this.
extern const Font1252 FONT_10x20;  // the big cells a 256px-wide chain makes possible
extern const Font1252 FONT_9x18;
extern const Font1252 FONT_8x13;
extern const Font1252 FONT_6x13;   // an 8x21 cell on a 128x64 panel
extern const Font1252 FONT_6x10;
extern const Font1252 FONT_6x9;    // the 8x10 cell of a 15x3 wall on a 128x32
extern const Font1252 FONT_5x8;    // smallest face that still carries accents

// Largest face first.
extern const Font1252* const FONT1252_ALL[FONT1252_COUNT];

// CP1252 byte -> glyph index, or 0xFF when the byte carries no printable glyph.
extern const uint8_t FONT1252_INDEX[256];

// Glyphs in each face's row table: the CP1252 set, then the extra pictographs. Bound
// font1252Row() with THIS, not with FONT1252_GLYPHS.
//
// That distinction has already cost a bug. The guard below used to read
// `gi >= FONT1252_GLYPHS`, which was exactly right while 216 was the whole table -- and the
// moment the pictographs were appended at indices 216..229 it began silently returning a
// zero row for every one of them. The reel resolved, the frame went out, the module moved
// to the right flap, and the panel drew nothing at all. A bounds check that is one constant
// out of date fails completely silently: it does not crash, it just erases.
#define FONT1252_TOTAL (FONT1252_GLYPHS + FONT_EXTRA_COUNT)

// Bitmap row `r` of glyph `gi` (bit 15 = leftmost pixel). Returns 0 out of range.
static inline uint16_t font1252Row(const Font1252& f, uint8_t gi, uint8_t r) {
  if (gi >= FONT1252_TOTAL || r >= f.height) return 0;
  return f.rows[(uint16_t)gi * f.height + r];
}

// The roomiest bundled face that fits inside a cellW x cellH cell while leaving a
// one-pixel gutter on each axis, so neighbouring flaps' glyphs never touch. Falls
// back to the smallest face when nothing fits (a cramped glyph beats a blank
// cell), so this never returns NULL.
const Font1252* font1252Best(uint8_t cellW, uint8_t cellH);

#endif // MPGW_FONT1252_H
