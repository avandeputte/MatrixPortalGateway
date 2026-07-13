// reel.h -- the flap reel, and the rule that turns a character into a flap index.
//
// Deliberately free of Arduino: it needs nothing but charset.h, so the firmware and the
// native regression test (tools/reel_test.cpp) compile the SAME code. The reel used to be
// typed out in vmodule.cpp and typed out AGAIN in the test, with a comment asking the two
// to be kept byte-identical. That is not a test; that is two copies of a guess.
//
// THE REEL
// --------
// A physical reel carries 64 leaves because it is a physical object. These modules are
// DRAWN, so there is nothing to ration: the reel carries one flap for every glyph the font
// can draw. 156 characters + 7 colours = 163 flaps, and every printable Windows-1252
// character has somewhere to land.
//
// It is BUILT, not enumerated -- from isFlapByte() and cp1252IsLower(), which already own
// "which bytes exist" and "which bytes are lowercase". Derive it and it cannot drift: add a
// glyph to the font and the reel grows a flap for it.
//
// NO LOWERCASE, and that is load-bearing rather than stylistic. The seven colour flaps are
// addressed by the LOWERCASE letters r o y g b p w -- that is the protocol, not a choice.
// Put lowercase letters on the reel and a lookup for 'r' would find the letter, and every
// colour command ever written would quietly start printing letters instead of colours. So
// lowercase folds to uppercase, which is what a real reel -- printed in capitals -- does
// anyway.
//
//   index 0        ' '        blank, the home position
//   index 0..155   the CP1252 repertoire in code-point order, minus the lowercase letters
//   index 156..162 r o y g b p w   the colour flaps -- LAST, always
#ifndef MPGW_REEL_H
#define MPGW_REEL_H

#include "charset.h"
#include <string.h>

#define SF_MAX_FLAPS     163                                  // 156 glyphs + 7 colours
#define SF_COLOUR_FLAPS  7                                    // r o y g b p w
#define SF_CHAR_FLAPS    (SF_MAX_FLAPS - SF_COLOUR_FLAPS)     // 156
#define VM_COLOUR_BASE   SF_CHAR_FLAPS                        // 156: the colours sit last

static const char REEL_COLOUR_CHARS[SF_COLOUR_FLAPS + 1] = "roygbpw";

// Fill `out` (SF_MAX_FLAPS bytes) with the reel. Returns the number of GLYPH flaps written,
// which must be SF_CHAR_FLAPS -- if it is not, the repertoire or the folding rule moved and
// SF_MAX_FLAPS is now a lie, so the caller should shout rather than ship a reel whose colour
// flaps sit at the wrong index.
static inline int reelBuild(char* out) {
  int n = 0;
  for (int b = 0x20; b <= 0xFF; b++) {
    if (!isFlapByte((uint8_t)b))   continue;   // control, undefined slot, NBSP, soft hyphen
    if (cp1252IsLower((uint8_t)b)) continue;   // folds onto its uppercase: no flap of its own
    if (n >= SF_CHAR_FLAPS) break;
    out[n++] = (char)b;
  }
  memcpy(out + SF_CHAR_FLAPS, REEL_COLOUR_CHARS, SF_COLOUR_FLAPS);
  return n;
}

// The flap index carrying `c`, or -1 if the reel cannot show it. The ORDER of these steps
// is the whole contract:
//
//   1. the colour codes FIRST -- they are lowercase by protocol, not letters by meaning,
//      and checking them here is what guarantees 'r' can only ever be red;
//   2. 'q', splitflap-os's legacy alias for the double-quote flap (the classic reel had no
//      lowercase, so its char map borrowed that byte). This reel carries a real '"', so the
//      alias is honoured instead of the frame being dropped;
//   3. fold to uppercase -- the reel is printed in capitals, like a real one;
//   4. scan the GLYPH flaps only. Scanning the colours too would let an uppercase letter
//      fall through into the colour range.
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

static inline char reelCharAt(const char* reel, int i) {
  if (i < 0 || i >= SF_MAX_FLAPS) return 0;
  return reel[i];
}

static inline bool reelIsColour(int i) {
  return i >= VM_COLOUR_BASE && i < VM_COLOUR_BASE + SF_COLOUR_FLAPS;
}

#endif // MPGW_REEL_H
