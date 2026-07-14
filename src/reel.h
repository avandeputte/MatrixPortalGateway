// reel.h -- the flap reel, and the two rules that turn a character into a flap index.
//
// Deliberately free of Arduino: it needs only charset.h and font1252.h, so the firmware and
// the native regression test (tools/reel_test.cpp) compile the SAME code. The reel used to
// be typed out in vmodule.cpp and typed out AGAIN in the test, with a comment asking that
// the two be kept byte-identical. That is not a test; that is two copies of a guess.
//
// THE REEL
// --------
// A physical reel carries 64 leaves because it is a physical object. These modules are
// DRAWN, so there is nothing to ration: 237 flaps -- every Windows-1252 glyph, the seven
// colours, every lowercase letter, and fourteen pictographs.
//
// It is BUILT, not enumerated -- from isFlapByte(), cp1252IsLower() and the font's own
// FONT_EXTRA_CP list, which already own "which bytes exist", "which are lowercase" and
// "which pictographs we have". Derive it and it cannot drift: add a glyph to the font and
// the reel grows a flap for it.
//
// TWO WAYS IN, AND THEY ARE NOT THE SAME
// --------------------------------------
// The legacy wire protocol carries ONE BYTE per character, and it has a problem it can
// never solve: the byte for lowercase 'r' already means RED. So on that path lowercase must
// fold to uppercase, and a heart -- which has no CP1252 byte at all -- cannot be addressed
// by character in ANY way. That is not a limitation of this firmware; it is a limitation of
// a one-byte alphabet that spent seven of its letters on colours.
//
//   reelIndexOf(char)            the legacy path. Colours first, fold to uppercase, scan
//                                only the 156 glyph flaps. Byte-for-byte as it always was.
//
//   reelIndexOfCodepoint(cp)     the index-addressed path, used by POST /api/display/cells.
//                                No folding, no colour-stealing, and it reaches the
//                                lowercase and pictograph flaps. Colours are NAMED by the
//                                caller instead -- which is precisely why that API had to
//                                have a different shape.
//
// The legacy sections come FIRST and keep the indices they have always had, so growing the
// reel can never move a flap an existing controller addresses by number.
#ifndef MPGW_REEL_H
#define MPGW_REEL_H

#include "charset.h"
#include "font1252.h"
#include <string.h>

/* ---- the reel, section by section -------------------------------------------------
   The order is not cosmetic: everything the LEGACY protocol can reach sits first, and
   at the same index it has always sat, so adding flaps cannot move a flap that an
   existing controller already addresses by number.

     0..155     the CP1252 glyphs, no lowercase        legacy: m<id>-<char>
     156..162   the colour flaps  r o y g b p w        legacy: m<id>-r  == RED
     ---- everything below is UNREACHABLE from the legacy protocol ----
     163..222   the 60 lowercase letters               index only
     223..236   the pictographs (heart, smiley, ...)   index only

   Why the split. The wire carries ONE BYTE per character, and the byte for lowercase 'r'
   is already spoken for: it means RED. So on the legacy path lowercase MUST fold to
   uppercase, and a heart -- which has no CP1252 byte at all -- cannot be addressed by
   character in any way. Both are reachable by INDEX (m<id>+<n>), which is what the
   /api/display/cells endpoint sends, and the only reason that endpoint has to exist.  */
#define SF_CHAR_FLAPS    156                                   // CP1252, no lowercase
#define SF_COLOUR_FLAPS  7                                     // r o y g b p w
#define SF_LOWER_FLAPS   60                                    // the lowercase letters
#define SF_EMOJI_FLAPS   FONT_EXTRA_COUNT                      // heart, smiley, arrows...

#define VM_COLOUR_BASE   SF_CHAR_FLAPS                         // 156
#define SF_LEGACY_FLAPS  (SF_CHAR_FLAPS + SF_COLOUR_FLAPS)     // 163 -- all the old protocol sees
#define SF_LOWER_BASE    SF_LEGACY_FLAPS                       // 163
#define SF_EMOJI_BASE    (SF_LOWER_BASE + SF_LOWER_FLAPS)      // 223
#define SF_MAX_FLAPS     (SF_EMOJI_BASE + SF_EMOJI_FLAPS)      // 237

static const char REEL_COLOUR_CHARS[SF_COLOUR_FLAPS + 1] = "roygbpw";
// The colour flaps by NAME, in reel order. The index-addressed API names them, because on
// that path 'r' is the LETTER r -- which is the whole point of it.
static const char* const REEL_COLOUR_NAMES[SF_COLOUR_FLAPS] = {
  "red", "orange", "yellow", "green", "blue", "purple", "white"
};

// Fill `out` (SF_MAX_FLAPS bytes) with the reel: each entry is the CP1252 byte of that
// flap, or 0 for a flap that has no byte (the colours, and the pictographs). Returns the
// number of GLYPH flaps written, which must be SF_CHAR_FLAPS -- if it is not, the
// repertoire or the folding rule moved and the section bases below are all wrong.
static inline int reelBuild(char* out) {
  memset(out, 0, SF_MAX_FLAPS);
  int n = 0;
  for (int b = 0x20; b <= 0xFF && n < SF_CHAR_FLAPS; b++) {
    if (!isFlapByte((uint8_t)b))   continue;   // control, undefined slot, NBSP, soft hyphen
    if (cp1252IsLower((uint8_t)b)) continue;   // folds onto its uppercase on the legacy path
    out[n++] = (char)b;
  }
  memcpy(out + VM_COLOUR_BASE, REEL_COLOUR_CHARS, SF_COLOUR_FLAPS);
  int lo = 0;
  for (int b = 0x20; b <= 0xFF && lo < SF_LOWER_FLAPS; b++) {
    if (!isFlapByte((uint8_t)b))    continue;
    if (!cp1252IsLower((uint8_t)b)) continue;
    out[SF_LOWER_BASE + lo++] = (char)b;       // the lowercase letters get flaps of their own
  }
  // The pictographs have no CP1252 byte, so their entries stay 0: they are identified by
  // POSITION (SF_EMOJI_BASE + k <-> FONT_EXTRA_CP[k]), which is what reelGlyph() uses.
  return n;
}

/* ---- LEGACY resolution: a character off the wire --------------------------------------
   Order is the contract:
     1. the colour codes FIRST -- lowercase by protocol, not letters by meaning. This is
        what guarantees 'r' can only ever be red;
     2. 'q', splitflap-os's alias for the double-quote flap (the classic reel had no
        lowercase, so its char map borrowed that byte);
     3. fold to uppercase -- the reel's glyph section is printed in capitals;
     4. scan the GLYPH flaps ONLY. Never the lowercase or pictograph sections: they do not
        exist as far as this protocol is concerned, and scanning them would let an
        uppercase letter fall through into a range no controller knows about.            */
static inline int reelIndexOf(const char* reel, char c) {
  uint8_t b = (uint8_t)c;

  const char* col = b ? strchr(REEL_COLOUR_CHARS, (char)b) : 0;
  if (col) return VM_COLOUR_BASE + (int)(col - REEL_COLOUR_CHARS);

  if (b == 'q') b = '"';
  else          b = cp1252ToUpper(b);

  for (int i = 0; i < SF_CHAR_FLAPS; i++)
    if ((uint8_t)reel[i] == b) return i;
  return -1;
}

/* ---- INDEX-ADDRESSED resolution: a Unicode code point from the new API ----------------
   Everything the legacy path cannot do:
     * it does NOT fold case, so 'r' is the LETTER r and lands on a lowercase flap;
     * it does NOT treat r/o/y/g/b/p/w as colours -- the caller names colours explicitly,
       which is exactly why the API had to change shape;
     * it reaches the pictographs, which have no byte and so no legacy address at all.
   Returns -1 if the reel has no flap for the code point.                                */
static inline int reelIndexOfCodepoint(const char* reel, uint32_t cp) {
  for (int k = 0; k < SF_EMOJI_FLAPS; k++)          // a heart, a smiley, an arrow
    if (FONT_EXTRA_CP[k] == cp) return SF_EMOJI_BASE + k;

  const int fb = cp1252FromUnicode(cp);
  if (fb < 0 || !isFlapByte((uint8_t)fb)) return -1;
  const uint8_t b = (uint8_t)fb;

  if (cp1252IsLower(b)) {                           // a REAL lowercase flap, not a fold
    for (int i = 0; i < SF_LOWER_FLAPS; i++)
      if ((uint8_t)reel[SF_LOWER_BASE + i] == b) return SF_LOWER_BASE + i;
    return -1;
  }
  for (int i = 0; i < SF_CHAR_FLAPS; i++)
    if ((uint8_t)reel[i] == b) return i;
  return -1;
}

// A colour flap by name ("red"..."white"), or -1. Case-sensitive, lowercase.
static inline int reelColourIndex(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < SF_COLOUR_FLAPS; i++)
    if (strcmp(REEL_COLOUR_NAMES[i], name) == 0) return VM_COLOUR_BASE + i;
  return -1;
}

static inline bool reelIsColour(int i) {
  return i >= VM_COLOUR_BASE && i < VM_COLOUR_BASE + SF_COLOUR_FLAPS;
}
static inline bool reelIsEmoji(int i) {
  return i >= SF_EMOJI_BASE && i < SF_EMOJI_BASE + SF_EMOJI_FLAPS;
}

// The FONT glyph index for a flap, or -1 if it has none (a colour flap: a solid swatch,
// not a glyph). This is the one place that knows a pictograph's glyph lives past the
// CP1252 block rather than being found through FONT1252_INDEX.
static inline int reelGlyph(const char* reel, int i) {
  if (i < 0 || i >= SF_MAX_FLAPS) return -1;
  if (reelIsColour(i)) return -1;
  if (reelIsEmoji(i))  return FONT_EXTRA_BASE + (i - SF_EMOJI_BASE);
  uint8_t gi = FONT1252_INDEX[(uint8_t)reel[i]];
  return (gi == 0xFF) ? -1 : (int)gi;
}

// The COLOUR FLAP a pictograph is drawn in (0..6), or -1 for the normal text ink. The
// caller turns that into pixels through the same palette the colour flaps use, so a heart
// is the red flap's red -- there is exactly one red on this wall.
static inline int reelTint(int i) {
  if (!reelIsEmoji(i)) return -1;
  return (int)FONT_EXTRA_COLOUR[i - SF_EMOJI_BASE];
}

// The UNICODE CODE POINT a flap shows, or 0 if it has none (a colour flap: a swatch, not a
// character). This is the only way to name a pictograph flap outside the panel: it has no
// CP1252 byte, so reelCharAt() returns 0 for it and every byte-shaped API is simply blind to
// it. /api/display/state reports through here, which is why the wall can read back a heart.
static inline uint32_t reelCodepointAt(const char* reel, int i) {
  if (i < 0 || i >= SF_MAX_FLAPS) return 0;
  if (reelIsColour(i)) return 0;
  if (reelIsEmoji(i))  return FONT_EXTRA_CP[i - SF_EMOJI_BASE];
  return cp1252ToUnicode((uint8_t)reel[i]);
}

static inline char reelCharAt(const char* reel, int i) {
  if (i < 0 || i >= SF_MAX_FLAPS) return 0;
  return reel[i];
}

#endif // MPGW_REEL_H
