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

volatile uint8_t gEffect      = EFFECT_NONE;
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

void effectReset(uint8_t type) {
  fxBuildLUTs();
  fxFrame = 0; fxTick = 0;
  const int W = gPanel.panelW, H = gPanel.panelH;
  // Only fire and Life use the shared grid -- don't allocate 2*W*H (32 KB on a
  // 256x64 panel) for plasma/matrix/clock/fliporama, which never touch it.
  if (type == EFFECT_FIRE || type == EFFECT_LIFE) {
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
  for (int x = 0; x < W; x++)
    fxBuf[(H - 1) * W + x] = (rnd() & 15) ? (uint8_t)(160 + (rnd() & 63)) : 255;
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
  for (int x = 0; x < cols; x++) {
    mHead[x] += (int32_t)mSpeed[x] * gEffectSpeed / 4;   // advance in 1/16-row units
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

void effectRender(uint8_t type) {
  if (!gPanel.ready) return;
  fxFrame += gEffectSpeed;
  fxTick++;
  const int W = gPanel.panelW, H = gPanel.panelH;
  switch (type) {
    case EFFECT_PLASMA:    renderPlasma();    break;
    case EFFECT_FIRE:      renderFire();      break;
    case EFFECT_MATRIX:    renderMatrix();    break;
    case EFFECT_FLIPORAMA: renderFliporama(); break;
    case EFFECT_CLOCK:     renderClock();     break;
    case EFFECT_LIFE:      renderLife(W, H);  break;
    default: break;
  }
}
