// Native check of the load-bearing reel claims, compiled against the REAL src/reel.h and
// src/charset.cpp. Nothing here is a transcription: reel.h is deliberately Arduino-free, so
// this test runs the same reelBuild()/reelIndexOf() the firmware runs. (The previous version
// kept its own copy of the reel and asked, in a comment, for the two to be kept byte-
// identical. That tests the copy, not the code.)
//
// What it pins down:
//   * the reel is 237 flaps -- 156 CP1252 glyphs, 7 colours, 60 lowercase, 14 pictographs
//   * the LEGACY indices never moved, so an existing controller still works
//   * on the legacy path 'r' resolves to RED and lowercase folds -- as it always has
//   * on the INDEX path 'r' is the letter r, 'a' != 'A', and a heart has a flap
//   * the colour flaps deliberately share a byte with 7 lowercase letters -- the exact
//     ambiguity a one-byte protocol cannot resolve, and why there are two resolvers
//   * lowercase folds to uppercase, INCLUDING every trap (y-diaeresis -> 0x9F, not eszett;
//     the division sign is not a letter; eszett has no uppercase; oe/s-caron/z-caron)
//   * EVERY printable Windows-1252 character resolves to a flap -- the whole point of the
//     change, and the thing the old 64-flap reel could not do for 99 of them
//   * 'q' still reaches the double-quote flap (splitflap-os's legacy alias)
//   * no byte sits on two flaps, and every glyph flap is a valid flap byte
//   * the 'A' reply carries the 163 LEGACY flaps only, and is byte-clean (a pictograph
//     has no byte: splicing one into an ASCII frame would truncate it at a NUL)
//
// Build:  c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp src/font1252.cpp \
//             -o /tmp/reel_test && /tmp/reel_test
#include "reel.h"
#include "font1252.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#define TX_MAX_BYTES 512
extern const Font1252* const FONT1252_ALL[FONT1252_COUNT];

static int fails = 0;

static void ck(bool ok, const char* what) {
  printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) fails++;
}
static void cki(int got, int want, const char* what) {
  bool ok = (got == want);
  printf("  %s  %-56s got %d, want %d\n", ok ? "PASS" : "FAIL", what, got, want);
  if (!ok) fails++;
}

int main() {
  static char reel[SF_MAX_FLAPS];
  const int glyphs = reelBuild(reel);

  printf("shape:\n");
  cki(glyphs, SF_CHAR_FLAPS, "glyph flaps built");
  cki(SF_MAX_FLAPS, 237, "total flaps (156 glyph + 7 colour + 60 lower + 14 pictograph)");
  ck(reel[0] == ' ', "flap 0 is blank (the home position)");
  ck(memcmp(reel + VM_COLOUR_BASE, "roygbpw", 7) == 0,
     "the colour flaps are the LAST seven");

  printf("\ncolours beat letters -- the reason the reel carries no lowercase:\n");
  cki(reelIndexOf(reel, 'r'), VM_COLOUR_BASE + 0, "'r' -> RED, not a letter");
  cki(reelIndexOf(reel, 'g'), VM_COLOUR_BASE + 3, "'g' -> GREEN");
  cki(reelIndexOf(reel, 'w'), VM_COLOUR_BASE + 6, "'w' -> WHITE");
  ck(reelIndexOf(reel, 'R') >= 0 && !reelIsColour(reelIndexOf(reel, 'R')),
     "'R' -> the LETTER R, well clear of the colour range");

  printf("\nlowercase folds to uppercase (a real reel is printed in capitals):\n");
  ck(reelIndexOf(reel, 'a') == reelIndexOf(reel, 'A'), "'a' -> the 'A' flap");
  ck(reelIndexOf(reel, 'z') == reelIndexOf(reel, 'Z'), "'z' -> the 'Z' flap");
  ck(reelIndexOf(reel, '\xE4') == reelIndexOf(reel, '\xC4'), "a-umlaut -> A-umlaut");
  ck(reelIndexOf(reel, '\xE9') == reelIndexOf(reel, '\xC9'), "e-acute  -> E-acute");
  ck(reelIndexOf(reel, '\xF8') == reelIndexOf(reel, '\xD8'), "o-slash  -> O-slash");
  ck(reelIndexOf(reel, '\x9C') == reelIndexOf(reel, '\x8C'), "oe-ligature -> OE");
  ck(reelIndexOf(reel, '\x9A') == reelIndexOf(reel, '\x8A'), "s-caron -> S-caron");

  printf("\nthe folding traps -- every one of these has bitten somebody:\n");
  ck(reelIndexOf(reel, '\xFF') == reelIndexOf(reel, '\x9F'),
     "y-diaeresis folds to 0x9F...");
  ck(reelIndexOf(reel, '\xFF') != reelIndexOf(reel, '\xDF'),
     "...and NOT to eszett, which a naive -0x20 would produce");
  ck(!cp1252IsLower(0xF7), "the division sign is not a lowercase letter");
  ck(reelIndexOf(reel, '\xF7') >= 0 &&
     reelIndexOf(reel, '\xF7') != reelIndexOf(reel, '\xD7'),
     "...so it has its OWN flap, not the multiplication sign's");
  ck(!cp1252IsLower(0xDF), "eszett has no uppercase in CP1252...");
  ck(reelIndexOf(reel, '\xDF') >= 0, "...so it keeps a flap of its own");
  ck(!cp1252IsLower(0xB5), "the micro sign is a symbol, not a lowercase letter");

  printf("\nlegacy alias:\n");
  ck(reelIndexOf(reel, 'q') == reelIndexOf(reel, '"'),
     "'q' still reaches the double-quote flap (splitflap-os)");

  printf("\nEVERY printable CP1252 character resolves to a flap:\n");
  int unresolved = 0, firstBad = -1;
  for (int b = 0x20; b <= 0xFF; b++) {
    if (!isFlapByte((uint8_t)b)) continue;
    if (reelIndexOf(reel, (char)b) < 0) {
      unresolved++;
      if (firstBad < 0) firstBad = b;
    }
  }
  cki(unresolved, 0, "printable CP1252 bytes with nowhere to land");
  if (unresolved) printf("        first unresolved byte: 0x%02X\n", firstBad);

  printf("\nthings the old 64-flap German reel could only show as a BLANK:\n");
  bool all = true;
  for (const char* p = "$%()+<>[]{}"; *p; p++) all = all && (reelIndexOf(reel, *p) >= 0);
  ck(all, "$ % ( ) + < > [ ] { }  all resolve now");
  ck(reelIndexOf(reel, '\xC9') >= 0 && reelIndexOf(reel, '\xD1') >= 0 &&
     reelIndexOf(reel, '\xA9') >= 0 && reelIndexOf(reel, '\xB0') >= 0,
     "E-acute, N-tilde, (c), degree  all resolve now");

  printf("\nintegrity:\n");
  // Within a SECTION a byte must be unique, or a lookup would be ambiguous.
  int dupes = 0;
  for (int i = 0; i < SF_CHAR_FLAPS; i++)
    for (int j = i + 1; j < SF_CHAR_FLAPS; j++)
      if (reel[i] == reel[j]) dupes++;
  cki(dupes, 0, "duplicate bytes inside the glyph section");
  dupes = 0;
  for (int i = 0; i < SF_LOWER_FLAPS; i++)
    for (int j = i + 1; j < SF_LOWER_FLAPS; j++)
      if (reel[SF_LOWER_BASE + i] == reel[SF_LOWER_BASE + j]) dupes++;
  cki(dupes, 0, "duplicate bytes inside the lowercase section");

  // ACROSS sections, one collision is deliberate and is the crux of the whole design: the
  // seven colour flaps carry the SAME bytes as seven lowercase letters (r o y g b p w).
  // That is the ambiguity a one-byte protocol cannot resolve, and the reason there are two
  // resolvers. Assert it explicitly rather than pretending it is not there.
  int collide = 0;
  for (int c = 0; c < SF_COLOUR_FLAPS; c++)
    for (int l = 0; l < SF_LOWER_FLAPS; l++)
      if (reel[VM_COLOUR_BASE + c] == reel[SF_LOWER_BASE + l]) collide++;
  cki(collide, SF_COLOUR_FLAPS,
      "colour flaps DELIBERATELY share a byte with 7 lowercase letters");
  ck(reelIndexOf(reel, 'r') != reelIndexOfCodepoint(reel, 'r'),
     "...and the two resolvers send that byte to different flaps");

  int notflap = 0;
  for (int i = 0; i < SF_CHAR_FLAPS; i++)
    if (!isFlapByte((uint8_t)reel[i])) notflap++;
  cki(notflap, 0, "glyph flaps that are not valid flap bytes");
  notflap = 0;
  for (int i = 0; i < SF_LOWER_FLAPS; i++)
    if (!isFlapByte((uint8_t)reel[SF_LOWER_BASE + i])) notflap++;
  cki(notflap, 0, "lowercase flaps that are not valid flap bytes");
  int withbyte = 0;
  for (int i = 0; i < SF_EMOJI_FLAPS; i++)
    if (reel[SF_EMOJI_BASE + i] != 0) withbyte++;
  cki(withbyte, 0, "pictograph flaps carrying a byte (they must not -- there is none)");

  printf("\nthe INDEX-ADDRESSED path -- everything the legacy protocol cannot do:\n");
  // lowercase is a REAL flap here, not a fold
  int lo_r = reelIndexOfCodepoint(reel, 'r');
  ck(lo_r >= SF_LOWER_BASE && lo_r < SF_LOWER_BASE + SF_LOWER_FLAPS,
     "'r' -> a real LOWERCASE flap, not the red swatch");
  ck(lo_r != reelIndexOf(reel, 'r'), "...and NOT where the legacy path sends it");
  ck(reelIndexOf(reel, 'r') == VM_COLOUR_BASE, "...while the legacy path still says RED");
  ck(reelIndexOfCodepoint(reel, 'a') != reelIndexOfCodepoint(reel, 'A'),
     "'a' and 'A' are different flaps (no folding here)");
  ck(reelIndexOfCodepoint(reel, 0xE9) != reelIndexOfCodepoint(reel, 0xC9),
     "e-acute and E-acute are different flaps");

  printf("\npictographs -- no byte, so no legacy address at all:\n");
  int heart = reelIndexOfCodepoint(reel, 0x2665);
  ck(heart >= SF_EMOJI_BASE, "heart U+2665 resolves to a pictograph flap");
  ck(reelIsEmoji(heart), "...and reelIsEmoji() agrees");
  ck(cp1252FromUnicode(0x2665) < 0, "...while CP1252 has no byte for it, as expected");
  ck(reelGlyph(reel, heart) >= FONT_EXTRA_BASE, "the heart draws a real font glyph");
  // The heart must be drawn in the RED FLAP's red -- not a bespoke RGB that only looks red
  // on a monitor. It first shipped as (0xE0,0x30,0x40) and came out PURPLE on the panel,
  // because a HUB75's blue LEDs are far brighter per unit duty than its red ones.
  cki(reelTint(heart), reelColourIndex("red") - VM_COLOUR_BASE,
      "the heart is drawn in the RED FLAP's red");
  cki(reelTint(reelIndexOfCodepoint(reel, 0x263A)),
      reelColourIndex("yellow") - VM_COLOUR_BASE, "the smiley is the YELLOW flap's yellow");
  cki(reelTint(reelIndexOfCodepoint(reel, 0x2660)), -1, "the spade takes the normal text ink");
  int badtint = 0;
  for (int k = 0; k < SF_EMOJI_FLAPS; k++) {
    int t = reelTint(SF_EMOJI_BASE + k);
    if (t < -1 || t >= SF_COLOUR_FLAPS) badtint++;
  }
  cki(badtint, 0, "every pictograph tint is a real colour flap (or none)");
  ck(reelIndexOfCodepoint(reel, 0x1F600) < 0,
     "an emoji we do NOT carry is rejected, not silently blanked");

  printf("\nthe pictographs must actually DRAW -- in every bundled face:\n");
  // This is the check that was missing. reelGlyph() can return a perfectly good glyph
  // index and the reel can resolve, and the panel still draws nothing, because
  // font1252Row() bounds-checks the index against the table size. Ask for the ink.
  for (int f = 0; f < FONT1252_COUNT; f++) {
    const Font1252& fn = *FONT1252_ALL[f];
    int blank = 0;
    for (int k = 0; k < SF_EMOJI_FLAPS; k++) {
      int gi = reelGlyph(reel, SF_EMOJI_BASE + k);
      int lit = 0;
      for (int r = 0; r < fn.height; r++) lit += (font1252Row(fn, (uint8_t)gi, (uint8_t)r) != 0);
      if (!lit) { blank++; printf("        %dx%d: %s draws NOTHING\n",
                                  fn.width, fn.height, FONT_EXTRA_NAME[k]); }
    }
    char what[64];
    snprintf(what, sizeof(what), "%dx%d: pictographs with no ink at all", fn.width, fn.height);
    cki(blank, 0, what);
  }

  printf("\ncolours by NAME (the legacy letters are letters here):\n");
  cki(reelColourIndex("red"),   VM_COLOUR_BASE + 0, "\"red\"");
  cki(reelColourIndex("white"), VM_COLOUR_BASE + 6, "\"white\"");
  cki(reelColourIndex("mauve"), -1, "an unknown colour name is rejected");

  printf("\nthe legacy indices did not move -- an old controller still works:\n");
  cki(reelIndexOf(reel, ' '), 0,               "blank is still flap 0");
  cki(reelIndexOf(reel, 'r'), VM_COLOUR_BASE,  "red is still the first colour flap");
  ck(reelIndexOf(reel, 'A') < SF_CHAR_FLAPS,   "'A' is still in the glyph section");
  cki(SF_LEGACY_FLAPS, 163, "the legacy reel is still 163 flaps");

  printf("\nthe 'A' reply carries the LEGACY reel -- does it fit, and is it byte-clean?\n");
  int nul = 0;
  for (int i = 0; i < SF_LEGACY_FLAPS; i++) if (reel[i] == 0) nul++;
  cki(nul, 0, "no NUL byte inside the 163 legacy flaps");
  ck(reel[SF_EMOJI_BASE] == 0, "...while a pictograph flap HAS no byte (hence the cap)");

  printf("\nthe 'A' reply now carries the whole reel -- does it still fit the wire?\n");
  char frame[TX_MAX_BYTES + 1];
  int n = snprintf(frame, sizeof(frame), "m%dA:%d:%d:%s:%d:%d:%d:%d::%u:",
                   254, 31, 254, "FA5E4827E2205AC80010", 2832, 4096, 1, 162,
                   (unsigned)SF_LEGACY_FLAPS);
  memcpy(frame + n, reel, SF_LEGACY_FLAPS);
  n += SF_LEGACY_FLAPS;
  frame[n++] = '\n';
  printf("        worst-case 'A' frame: %d bytes (TX_MAX_BYTES = %d)\n", n, TX_MAX_BYTES);
  ck(n < TX_MAX_BYTES, "the 'A' reply fits inside TX_MAX_BYTES");

  if (fails) printf("\n%d FAILED\n", fails);
  else       printf("\nall passed\n");
  return fails ? 1 : 0;
}
