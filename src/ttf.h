// ttf.h -- scalable, anti-aliased on-device text (v0.2).
//
// Rasterizes a bundled or uploaded TrueType face at an arbitrary pixel size with grayscale
// anti-aliasing and the full CP1252 charset, then composites the coverage onto the panel
// through panelBlitCoverRow(). A PSRAM glyph cache keyed by (face, size, codepoint) makes
// repeated frames (a ticking clock, a scrolling list) nearly free -- the steady state is a
// coverage blit, no re-rasterization.
//
// This is what lets the text-app catalog draw as ops instead of pushing whole pixel frames:
// the companion lays text out with PIL against the SAME face, and because the bundled TTF is
// a metrics-preserving CP1252 subset of that file, advance widths agree on the glass.

#ifndef MPGW_TTF_H
#define MPGW_TTF_H

#include <stdint.h>
#include <stddef.h>

enum { TTF_SANS = 0, TTF_MONO = 1, TTF_CUSTOM = 2, TTF_FACES = 3 };

#define TTF_MIN_SIZE 4
#define TTF_MAX_SIZE 512

// Parse the bundled faces. Safe to call once at boot; returns false only if the default
// (sans) face fails to parse, in which case gtext is a no-op and the op layer skips it.
bool ttfBegin();
bool ttfFaceReady(uint8_t face);

// Install / drop the uploaded custom face (raw TTF bytes, kept resident in PSRAM). Both
// clear the glyph cache so stale coverage for the old face can't survive.
bool ttfSetCustom(const uint8_t* ttf, size_t len);
void ttfClearCustom();

// Pixel width of a CP1252 string at a size (sum of advances + tracking). For align math and
// the companion metrics check. Bytes are CP1252 (the op layer folds UTF-8 -> CP1252 first).
int ttfTextWidth(uint8_t face, int size, const char* cp1252, int tracking);

// Draw a CP1252 string. (x,y) is the top-left of the ascent box; align shifts about x
// (0 left, 1 center, 2 right). Coverage composites through the panel's current batch blend
// mode/alpha. outline (1 px ring) and shadow (+1,+1 drop) are drawn under the fill when
// enabled, matching the bitmap text op. aa=false thresholds coverage to a hard 1-bit edge.
void ttfDrawText(int x, int y, int size, uint8_t face, const char* cp1252, int align,
                 uint8_t r, uint8_t g, uint8_t b, bool aa, int tracking,
                 bool hasOutline, uint8_t orr, uint8_t org, uint8_t orb,
                 bool hasShadow, uint8_t shr, uint8_t shg, uint8_t shb);

// Glyph-cache stats for /api/capabilities debug.
void ttfCacheStats(uint32_t* entries, uint32_t* bytes);

#endif // MPGW_TTF_H
