// Native check of the load-bearing reel claims, compiled against the REAL charset.cpp.
// Keep VM_DEFAULT_REEL below byte-identical to src/vmodule.cpp.
//   * the default reel is exactly 64 flaps (the count is protocol)
//   * flap 0 is blank; the seven colour flaps are the LAST seven (indices 57..63)
//   * a first-match lookup resolves 'r' to the RED flap (57), never the letter 'R' (18)
//   * 'q' (index 49) is the double-quote flap; the umlauts/eszett/euro are present
//   * every reel byte is a valid flap byte, and none but the space repeats
//   * the 'A' reply fits MSG_MAX_BYTES, and its colon arithmetic survives an EMPTY
//     flap map (the gateway's parser counts colons, not fields)
//   * utf8ToFlap maps euro and accented letters to their Windows-1252 flap bytes
//
// Build:  c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp -o /tmp/reel_test && /tmp/reel_test
#include "charset.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#define SF_MAX_FLAPS    64
#define SF_COLOUR_FLAPS 7
#define MSG_MAX_BYTES   320
#define TX_MAX_BYTES    512

// The German default reel -- MUST match src/vmodule.cpp's VM_DEFAULT_REEL byte for byte.
// Written with CP1252 hex escapes so the file's own encoding cannot corrupt it:
//   \xC4 A-umlaut  \xD6 O-umlaut  \xDC U-umlaut  \xDF eszett  \x80 euro
static const char REEL[] =
  " ABCDEFGHIJKLMNOPQRSTUVWXYZ" "\xC4\xD6\xDC\xDF" "0123456789!@#&" "\x80" "-=;q:'.,/?*" "roygbpw";
static const int REEL_LEN = (int)sizeof(REEL) - 1;

static int indexOf(char c) {   // mirrors vmFlapIndexOf: first match wins
  for (int i = 0; i < REEL_LEN; i++) if (REEL[i] == c) return i;
  return -1;
}

int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
  printf("reel length %d\n", REEL_LEN);
  CHECK(REEL_LEN == SF_MAX_FLAPS, "reel must be %d flaps, got %d", SF_MAX_FLAPS, REEL_LEN);
  CHECK(REEL[0] == ' ', "flap 0 must be blank");

  // Every reel byte must be a valid flap glyph.
  for (int i = 0; i < REEL_LEN; i++)
    CHECK(isFlapByte((uint8_t)REEL[i]), "reel byte %d (0x%02X) is not a flap byte", i, (uint8_t)REEL[i]);

  // Colours are the LAST seven, and first-match resolves 'r' to red, not the letter 'R'.
  const char* names = "roygbpw";
  for (int i = 0; i < 7; i++)
    CHECK(indexOf(names[i]) == SF_MAX_FLAPS - 7 + i,
          "'%c' must resolve to colour flap %d, got %d", names[i], SF_MAX_FLAPS - 7 + i, indexOf(names[i]));
  CHECK(indexOf('R') == 18, "'R' (the letter) must be at index 18, got %d", indexOf('R'));
  CHECK(indexOf('r') == 57, "'r' must resolve to the red flap at 57, got %d", indexOf('r'));

  // Fixed positions the firmware/renderer depend on.
  CHECK(indexOf(' ') == 0, "space must resolve to flap 0");
  CHECK(indexOf('A') == 1, "'A' must be at index 1, got %d", indexOf('A'));
  CHECK(indexOf('q') == 49, "'q' (the double-quote flap) must be at index 49, got %d", indexOf('q'));
  CHECK(indexOf((char)0xC4) == 27, "A-umlaut must be at index 27, got %d", indexOf((char)0xC4));
  CHECK(indexOf((char)0xDF) == 30, "eszett must be at index 30, got %d", indexOf((char)0xDF));
  CHECK(indexOf((char)0x80) == 45, "euro sign must be at index 45, got %d", indexOf((char)0x80));

  // Only the space may repeat -- otherwise a first-match scan hides the later copy.
  int repeats = 0;
  for (int i = 0; i < REEL_LEN; i++)
    if (REEL[i] != ' ' && indexOf(REEL[i]) != i) repeats++;
  CHECK(repeats == 0, "no non-space byte may repeat on the reel, found %d", repeats);

  printf("  'r'->%d  'R'->%d  'q'->%d  A-uml->%d  euro->%d\n",
         indexOf('r'), indexOf('R'), indexOf('q'), indexOf((char)0xC4), indexOf((char)0x80));

  // ---- worst-case 'A' reply, id 254, full default reel, EMPTY map ----
  char sn[21]; memset(sn, 'F', 20); sn[20] = 0;
  char out[1024];
  int n = snprintf(out, sizeof(out), "m%dA:%d:%d:%s:%d:%d:%d:%d::%u:",
                   254, 31, 254, sn, 2832, 4096, 1, REEL_LEN - 1, (unsigned)REEL_LEN);
  memcpy(out + n, REEL, REEL_LEN); n += REEL_LEN;
  out[n++] = '\n';
  printf("worst-case 'A' reply: %d bytes\n", n);
  CHECK(n <= MSG_MAX_BYTES, "'A' must fit a monitor entry (%d > %d)", n, MSG_MAX_BYTES);
  CHECK(n <= TX_MAX_BYTES,  "'A' must fit a frame (%d > %d)", n, TX_MAX_BYTES);

  // ---- the gateway's parser: colon #8 ends the (empty) map, #9 ends flapCount ----
  char aBuf[1024];
  const char* body = strchr(out, ':');           // sfParseResponse: p+1 after "m<id>A"
  strncpy(aBuf, body + 1, sizeof(aBuf) - 1);
  aBuf[sizeof(aBuf) - 1] = 0;
  for (int k = (int)strlen(aBuf) - 1; k >= 0 && (aBuf[k]=='\n'||aBuf[k]=='\r'); k--) aBuf[k] = 0;
  int colons = 0; const char *c8 = nullptr, *c9 = nullptr;
  for (const char* cp = aBuf; *cp; cp++)
    if (*cp == ':') { if (++colons == 8) c8 = cp; else if (colons == 9) { c9 = cp; break; } }
  CHECK(c8 && c9, "an 'A' reply with an empty map must still carry 9 colons");
  int gotCount = c8 ? atoi(c8 + 1) : -1;
  CHECK(gotCount == REEL_LEN, "flapCount parsed as %d, expected %d", gotCount, REEL_LEN);
  char gotChars[SF_MAX_FLAPS + 1] = "";
  if (c9) { strncpy(gotChars, c9 + 1, SF_MAX_FLAPS); gotChars[SF_MAX_FLAPS] = 0; }
  CHECK((int)strlen(gotChars) == REEL_LEN, "flapChars round-trip length %d, expected %d",
        (int)strlen(gotChars), REEL_LEN);
  CHECK(memcmp(gotChars, REEL, REEL_LEN) == 0, "flapChars round-trip mismatch");

  // The map field (f[7]) must come out empty.
  char split[1024]; strcpy(split, aBuf);
  char* f[16] = {0}; f[0] = split; int fi = 1;
  for (char* cp = split; *cp && fi < 16; cp++) if (*cp == ':') { *cp = 0; f[fi++] = cp + 1; }
  CHECK(f[7] && f[7][0] == 0, "the map field must parse as empty, got '%s'", f[7] ? f[7] : "(null)");
  CHECK(f[3] && strcmp(f[3], "2832") == 0, "homeOffset field wrong");
  CHECK(f[4] && strcmp(f[4], "4096") == 0, "totalSteps field wrong");

  // ---- utf8 round-trip through the real transcoder: euro, U-umlaut, eszett ----
  char enc[64]; bool ok = false;
  size_t k = utf8ToFlap("\xE2\x82\xAC\xC3\x9C\xC3\x9F", enc, sizeof(enc), &ok);  // EUR, U-uml, eszett
  CHECK(ok && k == 3, "utf8ToFlap should map 3 glyphs, got %zu ok=%d", k, (int)ok);
  CHECK((uint8_t)enc[0] == 0x80 && (uint8_t)enc[1] == 0xDC && (uint8_t)enc[2] == 0xDF,
        "utf8ToFlap mapped %02X %02X %02X", (uint8_t)enc[0], (uint8_t)enc[1], (uint8_t)enc[2]);

  printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", fails);
  return fails ? 1 : 0;
}
