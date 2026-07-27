// effects.cpp -- on-device panel effects. See effects.h.
//
// Everything is integer/LUT work so a full 128x64 (or 256x64) frame fits comfortably inside one
// panel refresh: a 256-entry sine table and two 256-entry palettes, a shared PSRAM grid buffer
// (fire heat / Life cells), and small per-column and per-cell arrays. panelShow() paces us to the
// panel's frame rate; effects that should run slower than that gate on a raw frame counter.
#include <Arduino.h>
#include "effects.h"
#include "display.h"
#include "panel.h"
#include "font1252.h"          // flap glyphs for fliporama
#include "aafont.h"            // 1-bit Orbitron faces for the clock
#include <math.h>
#include <esp_random.h>
#include <string.h>
#include <time.h>
#include "rtc.h"           // rtcLocalNow: broken-down local time for the clock effect
#include "audio.h"         // microphone features for the audio-reactive effects (v3.4)
#include "vmodule.h"       // soundwall pokes flap targets directly
#include "reel.h"          // VM_COLOUR_BASE: the colour flap indices

volatile uint8_t gEffect      = EFFECT_NONE;
volatile bool    gEffectAudioMod = false;   // "audio":true -- mic modulates the effect

/* ---- audio-reactive state (v3.4), shared across renders ---- */
static AudioFrame fxAud = {};
static bool       fxAudOn = false;
static float      specCap[AUDIO_BANDS];
static unsigned long swLastLoudMs = 0;

volatile uint8_t gEffectSpeed = 4;
volatile uint8_t gEffectReq   = EFFECT_REQ_IDLE;   // pending start, picked up by taskDisplay
volatile int     gEffectHue     = -1;              // -1 = effect default; else 0..255 (see effects.h)
volatile int     gEffectDensity = -1;              // -1 = effect default; else 1..100

#define FX_MAXW      256                  // widest panel the firmware supports
#define FX_MAXCELLS  260                  // most flap cells (>= any wall grid)

static bool     fxReady = false;          // LUTs built
static uint8_t  sinT[256];                // (sin*0.5+0.5)*255
static uint8_t  plasmaPal[256][3];        // full-hue rainbow
static uint8_t  firePal[256][3];          // black -> red -> orange -> yellow -> white
static uint32_t fxFrame = 0;              // animation phase; advances by gEffectSpeed each frame
static uint32_t fxTick  = 0;              // raw frame counter, for gating the slow effects

// Shared PSRAM grid, 2*W*H bytes: fire uses [0,WH) as heat; Life uses [0,WH)=cells, [WH,2WH)=next.
static uint8_t* fxBuf = nullptr;
static int      fxCap = 0;

// Matrix rain: a falling head per column, position in 1/16-row fixed point.
static int32_t  mHead[FX_MAXW];
static uint8_t  mSpeed[FX_MAXW];          // base fall rate, 1/16 rows/frame
static const int MTRAIL = 14;

// Flap cells, shared by fliporama and clock: a glyph per cell that flips over to a new one. A
// flip runs cellFlip from FLIP_LEN down to 0 in gated steps, through three phases.
static int16_t  cellX[FX_MAXCELLS], cellY[FX_MAXCELLS];
static uint8_t  cellCur[FX_MAXCELLS], cellNxt[FX_MAXCELLS], cellFlip[FX_MAXCELLS];
static int      cellN = 0, cellW = 0, cellH = 0;
static const Font1252* cellFace = &FONT_6x10;
static const int FLIP_LEN = 6;

// Life stall detection.
static uint32_t lifePop = 0; static int lifeStale = 0;

// Fast xorshift PRNG. esp_random() reads a hardware register and is far too slow to call once per
// pixel (fire needs ~8k/frame); seeded once from it in fxBuildLUTs().
static uint32_t fxRng = 2463534242u;
static inline uint32_t rnd() {
  fxRng ^= fxRng << 13; fxRng ^= fxRng >> 17; fxRng ^= fxRng << 5; return fxRng;
}

// h in 0..255 around the wheel, full saturation and value.
static void hsv(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t region = h / 43, rem = (h - region * 43) * 6;
  uint8_t q = 255 - rem, t = rem;
  switch (region) {
    case 0:  r = 255; g = t;   b = 0;   break;
    case 1:  r = q;   g = 255; b = 0;   break;
    case 2:  r = 0;   g = 255; b = t;   break;
    case 3:  r = 0;   g = q;   b = 255; break;
    case 4:  r = t;   g = 0;   b = 255; break;
    default: r = 255; g = 0;   b = q;   break;
  }
}

static void fxBuildLUTs() {
  if (fxReady) return;
  for (int i = 0; i < 256; i++)
    sinT[i] = (uint8_t)((sinf(2.0f * (float)M_PI * i / 256.0f) * 0.5f + 0.5f) * 255.0f);
  for (int i = 0; i < 256; i++) hsv((uint8_t)i, plasmaPal[i][0], plasmaPal[i][1], plasmaPal[i][2]);
  // Most of the range is the fiery part -- black -> red -> orange -> yellow -- and only the very
  // hottest tip whitens, so a hot base reads as yellow-orange rather than a white blob.
  for (int i = 0; i < 256; i++) {
    uint8_t r, g, b;
    if      (i < 72)  { r = (uint8_t)(i * 255 / 71);       g = 0;                               b = 0; }
    else if (i < 184) { r = 255;                           g = (uint8_t)((i - 72) * 255 / 111); b = 0; }
    else              { r = 255;                           g = 255;                             b = (uint8_t)((i - 184) * 255 / 71); }
    firePal[i][0] = r; firePal[i][1] = g; firePal[i][2] = b;
  }
  fxRng = esp_random() | 1u;              // seed the fast PRNG once from real entropy
  fxReady = true;
}

// Names in enum order (index 0 == EFFECT_PLASMA == enum value 1), generated from EFFECT_TABLE.
static const char* const EFFECT_NAMES[] = {
#define EFFECT_NAME(sym, name) name,
  EFFECT_TABLE(EFFECT_NAME)
#undef EFFECT_NAME
};
static const int EFFECT_COUNT = (int)(sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]));

uint8_t effectByName(const char* name) {
  if (name)
    for (int i = 0; i < EFFECT_COUNT; i++)
      if (!strcmp(name, EFFECT_NAMES[i])) return (uint8_t)(i + 1);
  return EFFECT_NONE;
}
const char* effectName(uint8_t e) {
  return (e >= 1 && e <= EFFECT_COUNT) ? EFFECT_NAMES[e - 1] : "none";
}
// The advertised set as a JSON array -- one source of truth for GET /api/canvas + /api/capabilities.
/* ---- self-describing effect defs (v3.4) --------------------------------------
   The param vocabulary: ONE definition per knob (key, type, range, default, label).
   Its order is load-bearing for the legacy "effectParams" union, which derives from
   it (skipping "speed", which predates the union and was never listed). */
struct EffectParamDef {
  const char* key; const char* type;
  int16_t minv, maxv, defv; bool hasDef;
  const char* label;
};
static const EffectParamDef EPV[] = {
  { "speed",   "int",  1, 10,  5, true,  "Speed"          },
  { "hue",     "int",  0, 255, 0, false, "Hue"            },
  { "density", "int",  1, 100, 0, false, "Density"        },
  { "audio",   "bool", 0, 1,   0, true,  "Audio reactive" },
};
enum { EPI_SPEED, EPI_HUE, EPI_DENSITY, EPI_AUDIO, EPI_COUNT };

// Per-effect: WHICH vocabulary knobs it actually consumes (verified against the
// render/reset code -- e.g. fire ignores speed and hue; soundwall takes nothing).
static const uint8_t P_PLASMA[]  = { EPI_SPEED, EPI_HUE, EPI_AUDIO };
static const uint8_t P_FIRE[]    = { EPI_AUDIO };
static const uint8_t P_MATRIX[]  = { EPI_SPEED, EPI_HUE, EPI_AUDIO };
static const uint8_t P_FLIP[]    = { EPI_SPEED, EPI_DENSITY };
static const uint8_t P_CLOCK[]   = { EPI_HUE };
static const uint8_t P_LIFE[]    = { EPI_SPEED, EPI_HUE, EPI_DENSITY };
static const uint8_t P_SPECT[]   = { EPI_HUE };
static const uint8_t P_MAZE[]    = { EPI_SPEED, EPI_HUE };
static const uint8_t P_RIPPLE[]  = { EPI_SPEED, EPI_HUE };
static const uint8_t P_NONE_[1]  = { 0 };                  // zero-length arrays are not C++

struct EffectDefRow { uint8_t id; const char* title; const uint8_t* p; uint8_t np; };
static const EffectDefRow DEFS[] = {
  { EFFECT_PLASMA,    "Plasma",      P_PLASMA, 3 },
  { EFFECT_FIRE,      "Fire",        P_FIRE,   1 },
  { EFFECT_MATRIX,    "Matrix Rain", P_MATRIX, 3 },
  { EFFECT_FLIPORAMA, "Flip-o-rama", P_FLIP,   2 },
  { EFFECT_CLOCK,     "Clock",       P_CLOCK,  1 },
  { EFFECT_LIFE,      "Game of Life",P_LIFE,   3 },
  { EFFECT_SPECTRUM,  "Spectrum",    P_SPECT,  1 },
  { EFFECT_SOUNDWALL, "Soundwall",   P_NONE_,  0 },
  { EFFECT_MAZE,      "Maze",        P_MAZE,   2 },
  { EFFECT_RIPPLE,    "Beat Ripples",P_RIPPLE, 2 },
};
// Registering an effect in EFFECT_TABLE without a def row (or vice versa) must not
// compile: the def list is how clients discover the effect's options. EFFECT_COUNT
// is generated from EFFECT_TABLE above.
static_assert(sizeof(DEFS) / sizeof(DEFS[0]) == (size_t)EFFECT_COUNT,
              "every EFFECT_TABLE entry needs an EffectDefRow in DEFS");

const char* effectDefsJson() {
  // Sized with headroom: nine effects currently emit ~1.7 KB. The build-time guard
  // below is loud but the JSON still goes out truncated -- keep the margin real.
  static char buf[3072];
  if (!buf[0]) {
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "[");
    for (size_t e = 0; e < sizeof(DEFS) / sizeof(DEFS[0]); e++) {
      const EffectDefRow& d = DEFS[e];
      n += snprintf(buf + n, sizeof(buf) - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"params\":[",
                    e ? "," : "", effectName(d.id), d.title);
      for (uint8_t k = 0; k < d.np; k++) {
        const EffectParamDef& p = EPV[d.p[k]];
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"key\":\"%s\",\"type\":\"%s\"",
                      k ? "," : "", p.key, p.type);
        if (p.type[0] == 'i')
          n += snprintf(buf + n, sizeof(buf) - n, ",\"min\":%d,\"max\":%d", p.minv, p.maxv);
        if (p.hasDef) {
          if (p.type[0] == 'b')
            n += snprintf(buf + n, sizeof(buf) - n, ",\"default\":%s", p.defv ? "true" : "false");
          else
            n += snprintf(buf + n, sizeof(buf) - n, ",\"default\":%d", p.defv);
        }
        n += snprintf(buf + n, sizeof(buf) - n, ",\"label\":\"%s\"}", p.label);
      }
      n += snprintf(buf + n, sizeof(buf) - n, "]}");
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
    if (strlen(buf) >= sizeof(buf) - 2) printf("[FX] effectDefs JSON TRUNCATED -- enlarge buf\n");
  }
  return buf;
}

const char* effectParamsUnionJson() {
  static char buf[96];
  if (!buf[0]) {
    int n = snprintf(buf, sizeof(buf), "[");
    for (int v = 0; v < EPI_COUNT; v++) {
      if (v == EPI_SPEED) continue;                        // legacy list never carried speed
      bool used = false;
      for (size_t e = 0; e < sizeof(DEFS) / sizeof(DEFS[0]) && !used; e++)
        for (uint8_t k = 0; k < DEFS[e].np; k++)
          if (DEFS[e].p[k] == v) { used = true; break; }
      if (used) n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\"",
                              buf[n - 1] == '[' ? "" : ",", EPV[v].key);
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
  }
  return buf;
}

const char* effectListJson() {
  static char buf[96];
  if (!buf[0]) {
    int n = 0; buf[n++] = '[';
    for (int i = 0; i < EFFECT_COUNT; i++)
      n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\"", i ? "," : "", EFFECT_NAMES[i]);
    snprintf(buf + n, sizeof(buf) - n, "]");
  }
  return buf;
}

// ---- flap cells (fliporama + clock) -------------------------------------------------------------

// One flap cell i: a TRUE-BLACK flap with bright warm ink and a subtle split seam -- the classic
// Solari look. (The old dim-grey card washed out to near-white at large sizes.) The glyph is
// split at its mid-line for the flip phases.
static void drawFlapCell(int i) {
  const int cx = cellX[i], cy = cellY[i], cw = cellW, ch = cellH;
  const Font1252* f = cellFace;
  const int gx = cx + (cw - f->width) / 2;
  const int gy = cy + (ch - f->height) / 2;
  const int hh = f->height / 2;                 // font rows in the top half
  const uint8_t R = 245, G = 240, B = 230;      // warm flap ink
  panelFillRect(cx, cy, cw, ch, 0, 0, 0);       // the flap: true black
  const uint8_t cur = cellCur[i], nxt = cellNxt[i];
  const int fl = cellFlip[i];
  if      (fl == 0) dispDrawGlyph1252(gx, gy, f, cur, 0, f->height, R, G, B);      // idle: whole glyph
  else if (fl > 4)  dispDrawGlyph1252(gx, gy, f, cur, hh, f->height, R, G, B);     // A: top gone
  else if (fl > 2) { dispDrawGlyph1252(gx, gy, f, nxt, 0, hh, R, G, B);            // B: new top...
                     dispDrawGlyph1252(gx, gy, f, cur, hh, f->height, R, G, B); }  //    ...old bottom
  else              dispDrawGlyph1252(gx, gy, f, nxt, 0, f->height, R, G, B);      // C: whole new glyph
  panelHLine(cx, gy + hh, cw, 30, 30, 40);      // the split seam, a subtle dark line
}

// Advance every in-progress flip one gated step; settle the ones that finish.
static void stepFlips() {
  for (int i = 0; i < cellN; i++)
    if (cellFlip[i]) { cellFlip[i]--; if (!cellFlip[i]) cellCur[i] = cellNxt[i]; }
}

static const char FLIP_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789   -+";  // spaces bias to blanks
static inline char randGlyph() { return FLIP_CHARSET[rnd() % (sizeof(FLIP_CHARSET) - 1)]; }

static void initFlipGrid(int W, int H) {
  int cols = gPanel.cols ? gPanel.cols : (W / 8);
  int rows = gPanel.rows ? gPanel.rows : (H / 10);
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  cellW = W / cols; cellH = H / rows;
  cellFace = font1252Best((uint8_t)cellW, (uint8_t)cellH);   // same face picker as the reel wall
  cellN = 0;
  for (int r = 0; r < rows && cellN < FX_MAXCELLS; r++)
    for (int c = 0; c < cols && cellN < FX_MAXCELLS; c++) {
      cellX[cellN] = (int16_t)(c * cellW); cellY[cellN] = (int16_t)(r * cellH);
      cellCur[cellN] = cellNxt[cellN] = (uint8_t)randGlyph();
      cellFlip[cellN] = 0;
      cellN++;
    }
}

static int flipGate() { int g = 7 - gEffectSpeed / 2; return g < 2 ? 2 : g; }

static void renderFliporama() {
  if (fxTick % flipGate() == 0) {
    stepFlips();
    const int dens = (gEffectDensity >= 0) ? gEffectDensity : 50;   // churn; 50 == the classic rate
    int nTrigger = 1 + cellN * dens / 1200;     // a few new flips each gated step
    for (int k = 0; k < nTrigger; k++) {
      int i = (int)(rnd() % (uint32_t)cellN);
      if (!cellFlip[i]) { cellNxt[i] = (uint8_t)randGlyph(); cellFlip[i] = (uint8_t)FLIP_LEN; }
    }
  }
  panelClear();
  for (int i = 0; i < cellN; i++) drawFlapCell(i);
  panelShow();
}

// ---- anti-aliased text (aafont.h) ---------------------------------------------------------------

static const AAGlyph* aaFind(const AAFont* f, char c) {
  uint8_t u = (uint8_t)c;
  if (u >= 128) return nullptr;
  uint8_t i = f->idx[u];
  return (i == 0xFF) ? nullptr : &f->g[i];
}
static int aaAdvance(const AAFont* f, char c) { const AAGlyph* g = aaFind(f, c); return g ? g->adv : 0; }
static int aaTextW(const AAFont* f, const char* s) { int w = 0; for (; *s; s++) w += aaAdvance(f, *s); return w; }
// One glyph, pen baseline at (px,by), from its 1-bit packed mask (row padded to whole bytes, MSB
// = leftmost). A pixel is either lit or off -- grayscale AA reads as muddy on the few bitplanes.
static void aaGlyph(int px, int by, const AAFont* f, char c, uint8_t r, uint8_t g, uint8_t b) {
  const AAGlyph* gl = aaFind(f, c);
  if (!gl || !gl->w) return;
  const uint8_t* bits = f->cov + gl->off;
  const int stride = (gl->w + 7) >> 3;                 // bytes per glyph row
  const int ox = px + gl->xoff, oy = by + gl->yoff;
  for (int yy = 0; yy < gl->h; yy++) {
    const uint8_t* row = bits + yy * stride;
    for (int xx = 0; xx < gl->w; xx++)
      if (row[xx >> 3] & (0x80 >> (xx & 7))) panelPixel(ox + xx, oy + yy, r, g, b);
  }
}
// One glyph centred inside a fixed slotW-wide slot at slotX -- so proportional (Orbitron) digits
// stay put in the monospaced clock instead of drifting sideways as the numbers change.
static void aaGlyphSlot(int slotX, int slotW, int by, const AAFont* f, char c,
                        uint8_t r, uint8_t g, uint8_t b) {
  const AAGlyph* gl = aaFind(f, c);
  if (!gl) return;
  aaGlyph(slotX + (slotW - gl->w) / 2 - gl->xoff, by, f, c, r, g, b);
}
static void aaText(int px, int by, const AAFont* f, const char* s, uint8_t r, uint8_t g, uint8_t b) {
  for (; *s; s++) { aaGlyph(px, by, f, *s, r, g, b); px += aaAdvance(f, *s); }
}

int aaTextDraw(int x, int y, int size, const char* str, int align,
               uint8_t r, uint8_t g, uint8_t b) {
  const AAFont* f = (size >= 30) ? &AAFONT_BIG : (size >= 18) ? &AAFONT_MED : &AAFONT_SMALL;
  char up[96];
  size_t n = 0;
  for (const char* p = str; *p && n < sizeof(up) - 1; p++)
    up[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
  up[n] = 0;
  int wpx = 0;
  for (const char* p = up; *p; p++) wpx += aaAdvance(f, *p);
  int px = (align == 1) ? x - wpx / 2 : (align == 2) ? x - wpx : x;
  aaText(px, y + f->asc, f, up, r, g, b);
  return wpx;
}

// Month names for the spelled-out date, e.g. "JULY 16".
static const char* const CLK_MONTHS[12] = {
  "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
  "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER" };

// The clock reads the RTC once per frame but rebuilds these strings only when the SECOND changes,
// not 70x a second. Both persist across an unsynced or lock-contended frame (rtcLocalNow returns
// false), so the panel holds the last good time instead of blanking. clkDigits is HHMMSS.
static int  clkCacheSec = -1;
static char clkDigits[6] = {'0','0','0','0','0','0'};
static char clkDate[16]  = "";
static void clockRefresh() {
  struct tm lt;
  if (!rtcLocalNow(&lt) || lt.tm_sec == clkCacheSec) return;
  clkCacheSec = lt.tm_sec;
  clkDigits[0] = (char)('0' + lt.tm_hour / 10); clkDigits[1] = (char)('0' + lt.tm_hour % 10);
  clkDigits[2] = (char)('0' + lt.tm_min  / 10); clkDigits[3] = (char)('0' + lt.tm_min  % 10);
  clkDigits[4] = (char)('0' + lt.tm_sec  / 10); clkDigits[5] = (char)('0' + lt.tm_sec  % 10);
  int mo = lt.tm_mon; if (mo < 0) mo = 0; else if (mo > 11) mo = 11;
  snprintf(clkDate, sizeof(clkDate), "%s %d", CLK_MONTHS[mo], lt.tm_mday);
}

// Seconds into the minute (0..60), interpolated between integer ticks with millis() so anything
// driven off it moves smoothly rather than jumping once a second.
static uint8_t clkLastSec = 255; static uint32_t clkSecMs = 0;
static float clockSecondsSmooth(const char dg[6]) {
  int sec = (dg[4]-'0')*10 + (dg[5]-'0');
  uint32_t now = millis();
  if (sec != clkLastSec) { clkLastSec = (uint8_t)sec; clkSecMs = now; }
  uint32_t sub = now - clkSecMs; if (sub > 1000) sub = 1000;
  return sec + sub / 1000.0f;
}

// A left-anchored bar that shrinks smoothly from full width to zero over the minute. It carries a
// gentle gradient (only ~SPAN of hue across the full width) rather than a whole rainbow, so it
// reads as one colour drifting rather than a stripe of many.
static void clockBar(int x, int y, int w, int h, float secs, int hb) {
  const int SPAN = 22;
  int bw = (int)((60.0f - secs) / 60.0f * w + 0.5f);
  if (bw > w) bw = w; else if (bw < 0) bw = 0;
  uint8_t r, g, b;
  for (int i = 0; i < bw; i++) {
    hsv((uint8_t)(hb + i * SPAN / (w > 0 ? w : 1)), r, g, b);
    panelFillRect(x + i, y, 1, h, r, g, b);
  }
}

// Draw HH MM centred on x=cx with digits from font f, and the colon as two small dots -- so the
// separator is tight rather than a full mono character cell. Each digit sits centred in its own
// digW-wide slot (so proportional Orbitron digits do not jitter). Each glyph steps the rainbow.
static void drawClockTime(int cx, int by, const AAFont* f, const char dg[6], int hb, int hstep) {
  const int digW = aaAdvance(f, '0');
  const AAGlyph* z = aaFind(f, '0');
  const int capH = z ? z->h : f->asc;
  const int dot = (capH / 8 < 2) ? 2 : capH / 8;
  const int colonW = dot + digW / 3;                        // tight -- a fraction of a digit wide
  int x = cx - (4 * digW + colonW) / 2;
  uint8_t r, g, b;
  hsv((uint8_t)(hb),           r, g, b); aaGlyphSlot(x, digW, by, f, dg[0], r, g, b); x += digW;
  hsv((uint8_t)(hb + hstep),   r, g, b); aaGlyphSlot(x, digW, by, f, dg[1], r, g, b); x += digW;
  hsv((uint8_t)(hb + 2*hstep), r, g, b);                    // colon: dots at the digit's 1/3 and 2/3
  { int cxo = x + (colonW - dot) / 2;
    panelFillRect(cxo, by - capH * 2 / 3, dot, dot, r, g, b);
    panelFillRect(cxo, by - capH / 3,     dot, dot, r, g, b); }
  x += colonW;
  hsv((uint8_t)(hb + 3*hstep), r, g, b); aaGlyphSlot(x, digW, by, f, dg[2], r, g, b); x += digW;
  hsv((uint8_t)(hb + 4*hstep), r, g, b); aaGlyphSlot(x, digW, by, f, dg[3], r, g, b);
}

// A clean digital clock: big anti-aliased HH MM (Orbitron) with a small dot colon, drifting through
// a rainbow, the date (month spelled out) below it, and a seconds bar that shrinks over the minute.
static void renderClock() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  clockRefresh();                                          // rebuild HHMMSS + date only on a new second
  panelClear();
  const int hb = (int)(fxTick / 3);
  const float secs = clockSecondsSmooth(clkDigits);
  const int barH = (H >= 48) ? 3 : 2;
  const int barY = H - barH;
  clockBar(1, barY, W - 2, barH, secs, hb);                 // seconds bar, full width along the bottom

  if (H < 48) {                                             // short panel (128x32): HH MM + bar only
    const AAFont* f = &AAFONT_MED;
    const AAGlyph* z = aaFind(f, '0');
    const int capH = z ? z->h : f->asc;
    drawClockTime(W / 2, (barY - 1 - capH) / 2 + capH, f, clkDigits, hb, 8);
    panelShow();
    return;
  }

  // Tall panel: big HH MM up top, the spelled-out date centred above the bar.
  const AAFont* big = &AAFONT_BIG;
  const AAFont* sm  = &AAFONT_SMALL;
  const int byDate  = barY - 3;                             // date baseline just above the bar
  const int dateTop = byDate - sm->asc;
  aaText((W - aaTextW(sm, clkDate)) / 2, byDate, sm, clkDate, 150, 150, 165);   // centred, muted
  const AAGlyph* z = aaFind(big, '0');
  const int capH = z ? z->h : big->asc;
  int byBig = (dateTop - capH) / 2 + capH; if (byBig < capH + 1) byBig = capH + 1;
  drawClockTime(W / 2, byBig, big, clkDigits, hb, 8);
  panelShow();
}

// ---- Conway's Game of Life ----------------------------------------------------------------------

// Life keeps two generations in the shared PSRAM grid and swaps a pointer each step rather than
// memcpy'ing the whole board back. lifeCur names the live half; the other half takes the next gen.
static uint8_t* lifeCur = nullptr;

static void seedLife(int W, int H) {
  if (!lifeCur) return;
  const int seed = (gEffectDensity >= 0) ? gEffectDensity : 28;   // % alive, default ~28
  for (int i = 0; i < W * H; i++) lifeCur[i] = ((int)(rnd() % 100) < seed) ? 1 : 0;
}

static void stepLife(int W, int H) {
  uint8_t* cur = lifeCur;
  uint8_t* nxt = (cur == fxBuf) ? fxBuf + W * H : fxBuf;
  uint32_t pop = 0;
  // Toroidal wrap, hoisted: the wrapped x-neighbour index depends only on x and
  // the wrapped row base only on y, so resolve each once instead of branching
  // eight times per cell (~130k branches per generation on a 256x64 board).
  static int16_t xm[FX_MAXW], xp[FX_MAXW];
  for (int x = 0; x < W; x++) {
    xm[x] = (int16_t)(x ? x - 1 : W - 1);
    xp[x] = (int16_t)(x + 1 < W ? x + 1 : 0);
  }
  for (int y = 0; y < H; y++) {
    const uint8_t* up = cur + (y ? y - 1 : H - 1) * W;
    const uint8_t* md = cur + y * W;
    const uint8_t* dn = cur + (y + 1 < H ? y + 1 : 0) * W;
    for (int x = 0; x < W; x++) {
      const int l = xm[x], r = xp[x];
      int n = (up[l] ? 1 : 0) + (up[x] ? 1 : 0) + (up[r] ? 1 : 0)
            + (md[l] ? 1 : 0)                   + (md[r] ? 1 : 0)
            + (dn[l] ? 1 : 0) + (dn[x] ? 1 : 0) + (dn[r] ? 1 : 0);
      uint8_t a = md[x], na;
      if (a) na = (n == 2 || n == 3) ? (uint8_t)(a < 255 ? a + 1 : 255) : 0;   // survive (age) / die
      else   na = (n == 3) ? 1 : 0;                                            // birth
      nxt[y * W + x] = na;
      if (na) pop++;
    }
  }
  lifeCur = nxt;                                          // swap generations -- no board-sized memcpy
  // A settled or dead board is dull -- reseed once it stops changing for a while.
  if (pop == 0 || pop == lifePop) { if (++lifeStale > 40) { seedLife(W, H); lifeStale = 0; } }
  else lifeStale = 0;
  lifePop = pop;
}

static void renderLife(int W, int H) {
  if (!lifeCur) { panelShow(); return; }                 // grid alloc failed: nothing to show
  int gate = 8 - gEffectSpeed / 2; if (gate < 1) gate = 1;
  if (fxTick % gate == 0) stepLife(W, H);
  const uint8_t* cells = lifeCur;
  // Read the volatile hue ONCE per frame and resolve its RGB once -- not per live
  // pixel, which recomputed the same hsv() up to 16k times a frame.
  const int hue = gEffectHue;
  uint8_t hr = 0, hg = 0, hb = 0;
  if (hue >= 0) hsv((uint8_t)hue, hr, hg, hb);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t a = cells[y * W + x];
      if (!a) { panelPixel(x, y, 0, 0, 0); continue; }
      if (hue < 0) {                                             // classic green colouring
        if (a <= 2) panelPixel(x, y, 180, 255, 190);            // newborn: bright white-green
        else { uint8_t g = (uint8_t)(120 + (a > 135 ? 135 : a)); // older: steadier green, glowing
               panelPixel(x, y, 0, g, a > 90 ? 60 : 0); }
      } else {                                                   // tinted by hue, brightness by age
        if (a <= 2) panelPixel(x, y, (uint8_t)(128 + hr / 2), (uint8_t)(128 + hg / 2), (uint8_t)(128 + hb / 2));
        else { uint8_t lv = (uint8_t)(120 + (a > 135 ? 135 : a));
               panelPixel(x, y, (uint8_t)(hr * lv / 255), (uint8_t)(hg * lv / 255), (uint8_t)(hb * lv / 255)); }
      }
    }
  panelShow();
}

// ---- lifecycle ----------------------------------------------------------------------------------

/* ---- maze (v3.4): Hunt-and-Kill generation, watched live -------------------------
   Corridors are 2 px wide on a 3 px pitch (2 px passage + 1 px wall), so the grid is
   (W-1)/3 x (H-1)/3 cells -- 85x21 on a 256x64 panel, which reads as a maze rather
   than a texture. fxBuf is the pixel map: 0 = wall, 1 = retracted corridor (solved
   away), 2..255 = passage brightness (255 fresh, decaying to a floor -- the carving
   leaves a glowing, fading trail). Phases: WALK randomly carves into unvisited
   neighbours until cornered; HUNT scans for an unvisited cell touching a visited one
   and resumes there (no on-screen sweep -- an earlier dotted scan-line overlay
   flashed like a display artifact because hunts are frequent and brief); when every
   cell is visited, SOLVE runs animated dead-end filling -- corridors retract until
   only the corner-to-corner solution remains lit (Hunt-and-Kill mazes are perfect,
   so the survivor is THE path); DONE holds the golden solution, then a fresh maze
   begins. */
enum { MZ_WALK, MZ_HUNT, MZ_SOLVE, MZ_DONE };
static uint8_t  mzMode;
static int16_t  mzCols, mzRows, mzCx, mzCy, mzHuntRow;
static uint32_t mzVisited, mzTotal;
static uint16_t mzHold;
// Rainbow-by-carve-order: the second half of fxBuf (2*W*H allocated) holds a hue per
// pixel, stamped from a rolling hue that advances a little per carved cell -- the
// finished maze is a gradient tracing the algorithm's own history. 8.8 fixed point,
// scaled in mzInit so a full generation spans ~1.5 turns of the wheel.
static uint16_t mzHueAcc, mzHueStep;

static inline uint8_t* mzHuePx(int x, int y) {
  return &fxBuf[(size_t)gPanel.panelW * gPanel.panelH + y * gPanel.panelW + x];
}

static inline uint8_t* mzPx(int x, int y) { return &fxBuf[y * gPanel.panelW + x]; }
static inline bool mzSeen(int cx, int cy) { return *mzPx(3 * cx + 1, 3 * cy + 1) >= 2; }

static void mzFillCell(int cx, int cy, uint8_t v) {      // the 2x2 passage block
  const int px = 3 * cx + 1, py = 3 * cy + 1;
  const uint8_t h = (uint8_t)(mzHueAcc >> 8);
  *mzPx(px, py) = v; *mzPx(px + 1, py) = v;
  *mzPx(px, py + 1) = v; *mzPx(px + 1, py + 1) = v;
  *mzHuePx(px, py) = h; *mzHuePx(px + 1, py) = h;
  *mzHuePx(px, py + 1) = h; *mzHuePx(px + 1, py + 1) = h;
}
static const int8_t MZDX[4] = { 1, -1, 0, 0 };
static const int8_t MZDY[4] = { 0, 0, 1, -1 };
// The 1-thick, 2-long wall gap between (cx,cy) and its neighbour in direction d.
static void mzFillWall(int cx, int cy, int d, uint8_t v) {
  const int px = 3 * cx + 1, py = 3 * cy + 1;
  const uint8_t h = (uint8_t)(mzHueAcc >> 8);
  switch (d) {
    case 0: *mzPx(px + 2, py) = v; *mzPx(px + 2, py + 1) = v;
            *mzHuePx(px + 2, py) = h; *mzHuePx(px + 2, py + 1) = h; break;   // right
    case 1: *mzPx(px - 1, py) = v; *mzPx(px - 1, py + 1) = v;
            *mzHuePx(px - 1, py) = h; *mzHuePx(px - 1, py + 1) = h; break;   // left
    case 2: *mzPx(px, py + 2) = v; *mzPx(px + 1, py + 2) = v;
            *mzHuePx(px, py + 2) = h; *mzHuePx(px + 1, py + 2) = h; break;   // down
    default:*mzPx(px, py - 1) = v; *mzPx(px + 1, py - 1) = v;
            *mzHuePx(px, py - 1) = h; *mzHuePx(px + 1, py - 1) = h; break;   // up
  }
}
static bool mzWallOpen(int cx, int cy, int d) {          // sample one gap pixel
  const int px = 3 * cx + 1, py = 3 * cy + 1;
  switch (d) {
    case 0:  return *mzPx(px + 2, py) >= 2;
    case 1:  return *mzPx(px - 1, py) >= 2;
    case 2:  return *mzPx(px, py + 2) >= 2;
    default: return *mzPx(px, py - 1) >= 2;
  }
}

static void mzInit() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  memset(fxBuf, 0, (size_t)W * H);
  mzCols = (int16_t)((W - 1) / 3);
  mzRows = (int16_t)((H - 1) / 3);
  mzCx = (int16_t)(rnd() % (uint32_t)mzCols);
  mzCy = (int16_t)(rnd() % (uint32_t)mzRows);
  mzFillCell(mzCx, mzCy, 255);
  mzVisited = 1;
  mzTotal   = (uint32_t)mzCols * mzRows;
  mzMode    = MZ_WALK;
  mzHuntRow = 0;
  mzHold    = 0;
  mzHueAcc  = (uint16_t)(rnd() << 8);                        // fresh palette each maze
  mzHueStep = (uint16_t)((256u * 384u) / (mzTotal ? mzTotal : 1));   // ~1.5 wheel turns per maze
}

static void mzWalkStep() {
  int dirs[4], nd = 0;
  for (int d = 0; d < 4; d++) {
    const int nx = mzCx + MZDX[d], ny = mzCy + MZDY[d];
    if (nx >= 0 && ny >= 0 && nx < mzCols && ny < mzRows && !mzSeen(nx, ny)) dirs[nd++] = d;
  }
  if (nd) {
    const int d = dirs[rnd() % (uint32_t)nd];
    mzFillWall(mzCx, mzCy, d, 255);
    mzCx = (int16_t)(mzCx + MZDX[d]); mzCy = (int16_t)(mzCy + MZDY[d]);
    mzFillCell(mzCx, mzCy, 255);
    mzVisited++;
    mzHueAcc = (uint16_t)(mzHueAcc + mzHueStep);
  } else if (mzVisited >= mzTotal) {
    mzMode = MZ_SOLVE;
  } else {
    mzMode = MZ_HUNT; mzHuntRow = 0;
  }
}

static bool mzSolvePass() {                      // one dead-end-filling pass
  bool changed = false;
  for (int cy = 0; cy < mzRows; cy++)
    for (int cx = 0; cx < mzCols; cx++) {
      if ((cx == 0 && cy == 0) || (cx == mzCols - 1 && cy == mzRows - 1)) continue;
      if (!mzSeen(cx, cy)) continue;
      int open = 0, lastD = -1;
      for (int d = 0; d < 4; d++) {
        const int nx = cx + MZDX[d], ny = cy + MZDY[d];
        if (nx >= 0 && ny >= 0 && nx < mzCols && ny < mzRows && mzWallOpen(cx, cy, d)) {
          open++; lastD = d;
        }
      }
      if (open <= 1) {                            // dead end: retract this cell
        mzFillCell(cx, cy, 1);
        if (lastD >= 0) mzFillWall(cx, cy, lastD, 1);
        changed = true;
      }
    }
  return changed;
}

static void renderMaze() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  if (!fxBuf) { panelShow(); return; }

  if (mzMode == MZ_WALK) {
    // A genuinely wide speed range: 1..3 step FRACTIONALLY (one carve every 4th/3rd/
    // 2nd frame -- meditative), 4..10 step quadratically (1..49 carves per frame --
    // 10 finishes a 256x64 maze in a few seconds).
    int steps;
    if (gEffectSpeed <= 3) steps = (fxTick % (5 - gEffectSpeed) == 0) ? 1 : 0;
    else                   steps = (gEffectSpeed - 3) * (gEffectSpeed - 3);
    for (int i = 0; i < steps && mzMode == MZ_WALK; i++) mzWalkStep();
  } else if (mzMode == MZ_HUNT) {
    // Hunt without ceremony: scan from the remembered row until a resume point is
    // found (bounded per frame so a long fruitless stretch cannot stall the render).
    int rows = 2 + gEffectSpeed / 2;
    while (rows-- > 0 && mzMode == MZ_HUNT) {
      bool found = false;
      for (int cx = 0; cx < mzCols && !found; cx++) {
        if (mzSeen(cx, mzHuntRow)) continue;
        for (int d = 0; d < 4 && !found; d++) {
          const int nx = cx + MZDX[d], ny = mzHuntRow + MZDY[d];
          if (nx >= 0 && ny >= 0 && nx < mzCols && ny < mzRows && mzSeen(nx, ny)) {
            mzFillCell(cx, mzHuntRow, 255);
            mzFillWall(cx, mzHuntRow, d, 255);
            mzCx = (int16_t)cx; mzCy = mzHuntRow;
            mzVisited++;
            mzHueAcc = (uint16_t)(mzHueAcc + mzHueStep);
            mzMode = MZ_WALK;
            found = true;
          }
        }
      }
      if (!found && ++mzHuntRow >= mzRows) {
        mzMode = (mzVisited >= mzTotal) ? MZ_SOLVE : MZ_WALK;
        mzHuntRow = 0;
      }
    }
  } else if (mzMode == MZ_SOLVE) {
    int passes = (gEffectSpeed <= 3) ? ((fxTick & 1) ? 0 : 1) : 1 + gEffectSpeed / 4;
    while (passes-- > 0 && mzMode == MZ_SOLVE)
      if (!mzSolvePass()) { mzMode = MZ_DONE; mzHold = 0; }
  } else {
    if (++mzHold > 220) mzInit();                 // ~3 s at the panel rate
  }

  // Corridors wear the rainbow of their carve order (hue map, offset by gEffectHue);
  // walls stay dark neutral so the colours carry the image; the solution stays gold.
  const int hueOfs = (gEffectHue >= 0) ? gEffectHue : 0;
  const bool solved = (mzMode == MZ_DONE);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t v = fxBuf[y * W + x];
      if (v == 0)      panelPixel(x, y, 10, 10, 18);
      else if (v == 1) panelPixel(x, y, 3, 3, 6);
      else {
        if (solved) { panelPixel(x, y, 255, 240, 120); continue; }   // the path, golden
        uint8_t r, g, b;
        hsv((uint8_t)(*mzHuePx(x, y) + hueOfs), r, g, b);
        panelPixel(x, y, (uint8_t)((r * v) >> 8), (uint8_t)((g * v) >> 8), (uint8_t)((b * v) >> 8));
        if (v > 140) fxBuf[y * W + x] = (uint8_t)(v - 3);            // fresh-carve flash fades
      }
    }
  if (mzMode == MZ_WALK || mzMode == MZ_HUNT) {   // the carving head burns white
    const int px = 3 * mzCx + 1, py = 3 * mzCy + 1;
    panelPixel(px, py, 255, 255, 255);     panelPixel(px + 1, py, 255, 255, 255);
    panelPixel(px, py + 1, 255, 255, 255); panelPixel(px + 1, py + 1, 255, 255, 255);
  }
  panelShow();
}

/* ---- beat ripples (v3.4): every beat launches an expanding ring ------------------
   Ring colour comes from the dominant frequency band (bass red -> treble violet,
   gEffectHue rotates the palette), brightness from the beat's loudness, both fading
   as the ring grows. A faint centre glow breathes with the room level so the panel
   reads as listening even between beats. Inherently audio-driven, like spectrum. */
struct Ripple { int16_t x, y; uint16_t r8; uint8_t hue, amp; bool alive; };
static Ripple ripPool[14];

static void renderRipple() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const int maxR = W / 2 + 8;

  if (fxAud.beat) {
    for (auto& rp : ripPool) {
      if (rp.alive) continue;
      int dom = 0;
      for (int b = 1; b < AUDIO_BANDS; b++) if (fxAud.bands[b] > fxAud.bands[dom]) dom = b;
      rp.x = (int16_t)(20 + rnd() % (uint32_t)(W > 40 ? W - 40 : W));
      rp.y = (int16_t)(H / 4 + rnd() % (uint32_t)(H / 2));
      rp.r8 = 8;
      rp.hue = (uint8_t)(dom * 190 / (AUDIO_BANDS - 1) +
                         (gEffectHue >= 0 ? gEffectHue : 0));
      rp.amp = (uint8_t)(120 + fxAud.level * 135);
      rp.alive = true;
      break;
    }
  }

  panelClear();
  // centre glow: the room's level, breathing
  {
    const int g = (int)(fxAud.level * 40);
    if (g > 2) panelCircle(W / 2, H / 2, 2 + g / 14, true,
                           (uint8_t)(g * 2), (uint8_t)(g * 2), (uint8_t)(g * 3));
  }
  const int step = 2 + gEffectSpeed;                 // expansion, 1/8 px per frame
  for (auto& rp : ripPool) {
    if (!rp.alive) continue;
    const int radius = rp.r8 >> 3;
    const int fade = 255 - (255 * radius) / maxR;
    if (radius >= maxR || fade <= 10) { rp.alive = false; continue; }
    uint8_t r, g, b;
    hsv(rp.hue, r, g, b);
    const int lvl = rp.amp * fade / 255;
    panelCircle(rp.x, rp.y, radius, false,
                (uint8_t)(r * lvl / 255), (uint8_t)(g * lvl / 255), (uint8_t)(b * lvl / 255));
    if (radius > 1)                                   // a dimmer inner ring for thickness
      panelCircle(rp.x, rp.y, radius - 1, false,
                  (uint8_t)(r * lvl / 510), (uint8_t)(g * lvl / 510), (uint8_t)(b * lvl / 510));
    rp.r8 = (uint16_t)(rp.r8 + step);
  }
  panelShow();
}

void effectReset(uint8_t type) {
  fxBuildLUTs();
  fxFrame = 0; fxTick = 0;
  const int W = gPanel.panelW, H = gPanel.panelH;
  // Only fire and Life use the shared grid -- don't allocate 2*W*H (32 KB on a
  // 256x64 panel) for plasma/matrix/clock/fliporama, which never touch it.
  if (type == EFFECT_FIRE || type == EFFECT_LIFE || type == EFFECT_MAZE) {
    const int need = 2 * W * H;
    if (fxCap < need) {                   // (re)allocate the shared grid in PSRAM
      if (fxBuf) free(fxBuf);
      fxBuf = (uint8_t*)ps_malloc(need);
      if (!fxBuf) fxBuf = (uint8_t*)malloc(need);
      fxCap = fxBuf ? need : 0;
    }
  }
  switch (type) {
    case EFFECT_FIRE:
      if (fxBuf) memset(fxBuf, 0, W * H);
      break;
    case EFFECT_MATRIX: {
      const int cols = W < FX_MAXW ? W : FX_MAXW;
      for (int x = 0; x < cols; x++) {    // stagger the rain so columns don't march in lockstep
        mHead[x]  = -(int32_t)(rnd() % (uint32_t)(H * 16));
        mSpeed[x] = (uint8_t)(4 + (rnd() % 12));
      }
      break;
    }
    case EFFECT_FLIPORAMA: initFlipGrid(W, H); break;
    case EFFECT_CLOCK:     clkCacheSec = -1; clkLastSec = 255; break;   // force a fresh read
    case EFFECT_LIFE:      lifeCur = fxBuf; lifePop = 0; lifeStale = 0; seedLife(W, H); break;
    case EFFECT_SPECTRUM:
      for (int b = 0; b < AUDIO_BANDS; b++) specCap[b] = 0;
      audioMaybeStart();
      break;
    case EFFECT_SOUNDWALL: swLastLoudMs = 0; audioMaybeStart(); break;
    case EFFECT_MAZE:      if (fxBuf) mzInit(); break;
    case EFFECT_RIPPLE:    memset(ripPool, 0, sizeof(ripPool)); audioMaybeStart(); break;
    default: break;
  }
}

static void renderPlasma() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const uint32_t t = fxFrame;
  const int hue = gEffectHue;             // one volatile read per frame, not per pixel
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t a = sinT[(uint8_t)(x * 2 + t)];
      uint8_t b = sinT[(uint8_t)(y * 3 - t)];
      uint8_t c = sinT[(uint8_t)((x + y) + (t >> 1))];
      uint8_t d = sinT[(uint8_t)((x - y) + t)];
      uint8_t idx = (uint8_t)(((int)a + b + c + d) >> 2);
      if (hue >= 0) idx = (uint8_t)(idx + hue);                 // tint: rotate the palette
      panelPixel(x, y, plasmaPal[idx][0], plasmaPal[idx][1], plasmaPal[idx][2]);
    }
  panelShow();
}

static void renderFire() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  if (!fxBuf) { panelShow(); return; }
  // Bottom row is the source: a flickering orange-yellow base (160..223), with the occasional
  // white-hot spark. Keeping the base below the white tip is what stops it becoming a white slab.
  // With "audio":true the fire breathes: the source row's energy follows loudness
  // (quiet room = embers, loud = roaring) and a beat throws extra white-hot sparks.
  const bool afire = fxAudOn && gEffectAudioMod;
  const uint8_t fbase = afire ? (uint8_t)(100 + fxAud.level * 123) : 160;
  const uint32_t sparkMask = (afire && fxAud.beat) ? 3 : 15;
  for (int x = 0; x < W; x++)
    fxBuf[(H - 1) * W + x] = (rnd() & sparkMask) ? (uint8_t)(fbase + (rnd() & 63)) : 255;
  // Doom-style spread: carry each cell up one row with a random sideways DRIFT and a random decay.
  // That asymmetry -- not a symmetric blur -- is what breaks the sheet into flame tongues. Rows are
  // the outer loop (row-major) so each source row is fully read before the next iteration writes
  // over it, and the framebuffer is walked sequentially instead of column-strided.
  for (int y = 1; y < H; y++)
    for (int x = 0; x < W; x++) {
      int pixel = fxBuf[y * W + x];
      if (pixel == 0) { fxBuf[(y - 1) * W + x] = 0; continue; }
      uint32_t r = rnd();
      int nx = x + (int)(r & 3) - 1;                  // drift -1..+2
      if (nx < 0) nx = 0; else if (nx >= W) nx = W - 1;
      int decay = 1 + (int)((r >> 2) & 7);            // cool 1..8 per row
      int v = pixel - decay;
      fxBuf[(y - 1) * W + nx] = (uint8_t)(v < 0 ? 0 : v);
    }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t h = fxBuf[y * W + x];
      panelPixel(x, y, firePal[h][0], firePal[h][1], firePal[h][2]);
    }
  panelShow();
}

static void renderMatrix() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const int cols = W < FX_MAXW ? W : FX_MAXW;
  panelClear();                           // black field
  // With "audio":true the rain falls harder with loudness, and a beat spawns a
  // burst of fresh columns at the top.
  const bool arain = fxAudOn && gEffectAudioMod;
  const int  aspd  = arain ? (int)(16 + 32 * fxAud.level) : 16;   // /16 fixed-point
  if (arain && fxAud.beat)
    for (int k = 0; k < cols / 6; k++) {
      const int x = (int)(rnd() % (uint32_t)cols);
      mHead[x] = 0; mSpeed[x] = (uint8_t)(10 + (rnd() % 8));
    }
  for (int x = 0; x < cols; x++) {
    mHead[x] += (int32_t)mSpeed[x] * gEffectSpeed * aspd / 64;   // advance in 1/16-row units
    int hy = mHead[x] >> 4;
    if (hy - MTRAIL > H) {                // fully off the bottom: respawn above the top
      mHead[x]  = -(int32_t)(rnd() % (uint32_t)(H * 8));
      mSpeed[x] = (uint8_t)(4 + (rnd() % 12));
      hy = mHead[x] >> 4;
    }
    const int mh = gEffectHue;                           // -1 = the classic green rain
    uint8_t hr = 0, hg = 0, hb = 0;
    if (mh >= 0) hsv((uint8_t)mh, hr, hg, hb);
    for (int k = 0; k < MTRAIL; k++) {
      int yy = hy - k;
      if (yy < 0 || yy >= H) continue;
      if (k == 0) {                                      // bright near-white head
        if (mh < 0) panelPixel(x, yy, 200, 255, 200);
        else panelPixel(x, yy, (uint8_t)(128 + hr / 2), (uint8_t)(128 + hg / 2), (uint8_t)(128 + hb / 2));
      } else {
        uint8_t lvl = (uint8_t)(230 * (MTRAIL - k) / MTRAIL);
        if (mh < 0) panelPixel(x, yy, 0, lvl, 0);        // classic green trail
        else panelPixel(x, yy, (uint8_t)(hr * lvl / 255), (uint8_t)(hg * lvl / 255), (uint8_t)(hb * lvl / 255));
      }
    }
  }
  panelShow();
}

/* ---- spectrum (v3.4): log-band bars, falling peak caps, hue gradient ---- */
static void renderSpectrum() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  panelClear();
  const int bw   = W / AUDIO_BANDS;                    // 8 px per band at 128, 16 at 256
  const int base = (gEffectHue >= 0) ? gEffectHue : 160;   // default: red bass -> blue treble
  for (int b = 0; b < AUDIO_BANDS; b++) {
    const float v = fxAud.bands[b];
    const int   h = (int)(v * (H - 1) + 0.5f);
    uint8_t r, g, bl;
    hsv((uint8_t)(base + 200 - b * 200 / AUDIO_BANDS), r, g, bl);
    const int x0 = b * bw;
    for (int y = 0; y < h; y++) {
      // dimmer at the base, full colour at the tip -- reads as depth, costs nothing
      const uint8_t lvl = (uint8_t)(120 + 135 * y / (h > 1 ? h - 1 : 1));
      for (int x = x0; x < x0 + bw - 1; x++)
        panelPixel(x, H - 1 - y, (uint8_t)(r * lvl / 255), (uint8_t)(g * lvl / 255), (uint8_t)(bl * lvl / 255));
    }
    // falling cap: rises instantly, sinks ~0.4 px/frame
    if (h > specCap[b]) specCap[b] = (float)h;
    else { specCap[b] -= 0.4f; if (specCap[b] < 0) specCap[b] = 0; }
    const int cy = H - 1 - (int)specCap[b];
    if (cy >= 0 && cy < H)
      for (int x = x0; x < x0 + bw - 1; x++) panelPixel(x, cy, 230, 230, 230);
  }
  panelShow();
}

/* ---- soundwall (v3.4): the flap wall itself is the visual ---- */
// Runs on taskDisplay every display tick while gEffect == EFFECT_SOUNDWALL, with the
// NORMAL wall renderer still active: this only pokes vmodule targets and lets the
// reels flip there on their own. A beat flips a loudness-scaled handful of random
// cells to a colour flap chosen by the dominant frequency band (bass=red ...
// treble=white); after ~2.5 s of quiet the wall settles home a few cells at a time.
void effectSoundwallTick() {
  if (!audioAvailable()) return;
  audioMaybeStart();                                  // (re)arm capture if it self-stopped
  AudioFrame a;
  audioRead(a);
  if (a.seq == 0) return;                             // capture still spinning up

  const unsigned long now = millis();
  if (a.level > 0.18f) swLastLoudMs = now;

  if (a.beat && vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    // dominant band -> colour flap (reel order r o y g b p w at VM_COLOUR_BASE)
    int dom = 0;
    for (int b = 1; b < AUDIO_BANDS; b++) if (a.bands[b] > a.bands[dom]) dom = b;
    const int16_t flap = (int16_t)(VM_COLOUR_BASE + (dom * 7) / AUDIO_BANDS);
    const int n = 1 + (int)(a.level * vmCount / 5);   // louder beat, bigger splash
    for (int k = 0; k < n; k++) {
      VModule& m = vmods[rnd() % (uint32_t)vmCount];
      m.target = flap;
    }
    xSemaphoreGive(vmMutex);
  } else if (swLastLoudMs && now - swLastLoudMs > 2500) {
    // quiet: settle home gently, a few cells per tick, instead of one mass wipe
    if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      for (int k = 0; k < 3; k++) {
        VModule& m = vmods[rnd() % (uint32_t)vmCount];
        if (m.curIndex != 0 && m.target < 0) m.target = 0;
      }
      xSemaphoreGive(vmMutex);
    }
  }
}

void effectRender(uint8_t type) {
  if (!gPanel.ready) return;
  fxAudOn = (type == EFFECT_SPECTRUM) || (type == EFFECT_RIPPLE) ||
            (gEffectAudioMod && audioAvailable());
  if (fxAudOn) audioRead(fxAud); else fxAud = AudioFrame{};
  fxFrame += gEffectSpeed;
  // Audio modulation, the shared part: loudness adds tempo, a beat adds a lurch --
  // visible on plasma (phase), and harmless elsewhere since renders that don't use
  // fxFrame ignore it.
  if (fxAudOn && gEffectAudioMod) {
    fxFrame += (uint32_t)(fxAud.level * gEffectSpeed * 3);
    if (fxAud.beat) fxFrame += 40;
  }
  fxTick++;
  const int W = gPanel.panelW, H = gPanel.panelH;
  switch (type) {
    case EFFECT_PLASMA:    renderPlasma();    break;
    case EFFECT_FIRE:      renderFire();      break;
    case EFFECT_MATRIX:    renderMatrix();    break;
    case EFFECT_FLIPORAMA: renderFliporama(); break;
    case EFFECT_CLOCK:     renderClock();     break;
    case EFFECT_LIFE:      renderLife(W, H);  break;
    case EFFECT_SPECTRUM:  renderSpectrum();  break;
    case EFFECT_MAZE:      renderMaze();      break;
    case EFFECT_RIPPLE:    renderRipple();    break;
    // EFFECT_SOUNDWALL never reaches effectRender: taskDisplay routes it to
    // effectSoundwallTick() and keeps the wall renderer running.
    default: break;
  }
}
