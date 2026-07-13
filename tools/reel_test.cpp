// Native check of the load-bearing reel claims, compiled against the REAL src/reel.h and
// src/charset.cpp. Nothing here is a transcription: reel.h is deliberately Arduino-free, so
// this test runs the same reelBuild()/reelIndexOf() the firmware runs. (The previous version
// kept its own copy of the reel and asked, in a comment, for the two to be kept byte-
// identical. That tests the copy, not the code.)
//
// What it pins down:
//   * the reel is 163 flaps -- 156 CP1252 glyphs + 7 colours -- and flap 0 is blank
//   * the colour flaps are the LAST seven, and 'r' resolves to RED, never to a letter
//   * lowercase folds to uppercase, INCLUDING every trap (y-diaeresis -> 0x9F, not eszett;
//     the division sign is not a letter; eszett has no uppercase; oe/s-caron/z-caron)
//   * EVERY printable Windows-1252 character resolves to a flap -- the whole point of the
//     change, and the thing the old 64-flap reel could not do for 99 of them
//   * 'q' still reaches the double-quote flap (splitflap-os's legacy alias)
//   * no byte sits on two flaps, and every glyph flap is a valid flap byte
//   * the 'A' reply -- which now carries all 163 flaps -- still fits TX_MAX_BYTES
//
// Build:  c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp -o /tmp/reel_test && /tmp/reel_test
#include "reel.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#define TX_MAX_BYTES 512

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
  cki(SF_MAX_FLAPS, 163, "total flaps (glyphs + colours)");
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
  int dupes = 0;
  for (int i = 0; i < SF_MAX_FLAPS; i++)
    for (int j = i + 1; j < SF_MAX_FLAPS; j++)
      if (reel[i] == reel[j]) dupes++;
  cki(dupes, 0, "bytes sitting on two flaps");
  int notflap = 0;
  for (int i = 0; i < SF_CHAR_FLAPS; i++)
    if (!isFlapByte((uint8_t)reel[i])) notflap++;
  cki(notflap, 0, "glyph flaps that are not valid flap bytes");

  printf("\nthe 'A' reply now carries the whole reel -- does it still fit the wire?\n");
  char frame[TX_MAX_BYTES + 1];
  int n = snprintf(frame, sizeof(frame), "m%dA:%d:%d:%s:%d:%d:%d:%d::%u:",
                   254, 31, 254, "FA5E4827E2205AC80010", 2832, 4096, 1, 162,
                   (unsigned)SF_MAX_FLAPS);
  memcpy(frame + n, reel, SF_MAX_FLAPS);
  n += SF_MAX_FLAPS;
  frame[n++] = '\n';
  printf("        worst-case 'A' frame: %d bytes (TX_MAX_BYTES = %d)\n", n, TX_MAX_BYTES);
  ck(n < TX_MAX_BYTES, "the 'A' reply fits inside TX_MAX_BYTES");

  if (fails) printf("\n%d FAILED\n", fails);
  else       printf("\nall passed\n");
  return fails ? 1 : 0;
}
