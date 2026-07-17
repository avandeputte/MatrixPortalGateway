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
#include "aafont.h"            // anti-aliased JetBrains Mono faces for the clock
#include <math.h>
#include <esp_random.h>
#include <string.h>

extern void rtcFormatTime(char* out, size_t outLen);   // local time string from rtc.cpp

volatile uint8_t gEffect      = EFFECT_NONE;
volatile uint8_t gEffectSpeed = 4;

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
static int      cellN = 0, cellW = 0, cellH = 0, cellScale = 1;
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

uint8_t effectByName(const char* name) {
  if (!name) return EFFECT_NONE;
  if (!strcmp(name, "plasma"))    return EFFECT_PLASMA;
  if (!strcmp(name, "fire"))      return EFFECT_FIRE;
  if (!strcmp(name, "matrix"))    return EFFECT_MATRIX;
  if (!strcmp(name, "fliporama")) return EFFECT_FLIPORAMA;
  if (!strcmp(name, "clock"))     return EFFECT_CLOCK;
  if (!strcmp(name, "life"))      return EFFECT_LIFE;
  return EFFECT_NONE;
}
const char* effectName(uint8_t e) {
  switch (e) { case EFFECT_PLASMA:    return "plasma";
               case EFFECT_FIRE:      return "fire";
               case EFFECT_MATRIX:    return "matrix";
               case EFFECT_FLIPORAMA: return "fliporama";
               case EFFECT_CLOCK:     return "clock";
               case EFFECT_LIFE:      return "life";
               default:               return "none"; }
}
// One source of truth for the advertised set -- used by GET /api/canvas and /api/capabilities.
const char* effectListJson() {
  return "[\"plasma\",\"fire\",\"matrix\",\"fliporama\",\"clock\",\"life\"]";
}

// ---- flap cells (fliporama + clock) -------------------------------------------------------------

// The largest bundled face that fits a cw x ch cell with a little gutter.
static const Font1252* pickFace(int cw, int ch) {
  if (cw >= 11 && ch >= 21) return &FONT_10x20;
  if (cw >= 10 && ch >= 19) return &FONT_9x18;
  if (cw >= 9  && ch >= 14) return &FONT_8x13;
  if (cw >= 7  && ch >= 14) return &FONT_6x13;
  if (cw >= 7  && ch >= 11) return &FONT_6x10;
  if (cw >= 7  && ch >= 10) return &FONT_6x9;
  return &FONT_5x8;
}

// Draw font rows [rowFrom,rowTo) of the glyph for ASCII `ch` at (px,py) in (r,g,b), each font
// pixel scaled to a cellScale x cellScale block (so big clock digits stay crisp).
static void drawGlyphRows(int px, int py, const Font1252* f, uint8_t ch,
                          int rowFrom, int rowTo, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t gi = FONT1252_INDEX[ch];
  if (gi == 0xFF) return;
  if (rowFrom < 0) rowFrom = 0;
  if (rowTo > f->height) rowTo = f->height;
  const int s = cellScale;
  for (int row = rowFrom; row < rowTo; row++) {
    uint16_t bits = font1252Row(*f, gi, (uint8_t)row);
    for (int col = 0; col < f->width; col++)
      if (bits & (uint16_t)(0x8000 >> col)) {
        if (s == 1) panelPixel(px + col, py + row, r, g, b);
        else        panelFillRect(px + col * s, py + row * s, s, s, r, g, b);
      }
  }
}

// One flap cell i: a TRUE-BLACK flap with bright warm ink and a subtle split seam -- the classic
// Solari look. (The old dim-grey card washed out to near-white at large sizes.) The glyph is
// scaled by cellScale and split at its mid-line for the flip phases.
static void drawFlapCell(int i) {
  const int s = cellScale;
  const int cx = cellX[i], cy = cellY[i], cw = cellW, ch = cellH;
  const Font1252* f = cellFace;
  const int gx = cx + (cw - f->width * s) / 2;
  const int gy = cy + (ch - f->height * s) / 2;
  const int hh = f->height / 2;                 // font rows in the top half
  const uint8_t R = 245, G = 240, B = 230;      // warm flap ink
  panelFillRect(cx, cy, cw, ch, 0, 0, 0);       // the flap: true black
  const uint8_t cur = cellCur[i], nxt = cellNxt[i];
  const int fl = cellFlip[i];
  if      (fl == 0) drawGlyphRows(gx, gy, f, cur, 0, f->height, R, G, B);      // idle: whole glyph
  else if (fl > 4)  drawGlyphRows(gx, gy, f, cur, hh, f->height, R, G, B);     // A: top gone
  else if (fl > 2) { drawGlyphRows(gx, gy, f, nxt, 0, hh, R, G, B);            // B: new top...
                     drawGlyphRows(gx, gy, f, cur, hh, f->height, R, G, B); }  //    ...old bottom
  else              drawGlyphRows(gx, gy, f, nxt, 0, f->height, R, G, B);      // C: whole new glyph
  panelHLine(cx, gy + hh * s, cw, 30, 30, 40);  // the split seam, a subtle dark line
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
  cellFace = pickFace(cellW, cellH);
  cellScale = 1;
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
    int nTrigger = 1 + cellN / 24;              // a few new flips each gated step
    for (int k = 0; k < nTrigger; k++) {
      int i = (int)(rnd() % (uint32_t)cellN);
      if (!cellFlip[i]) { cellNxt[i] = (uint8_t)randGlyph(); cellFlip[i] = (uint8_t)FLIP_LEN; }
    }
  }
  panelClear();
  for (int i = 0; i < cellN; i++) drawFlapCell(i);
  panelShow();
}

// ---- split-flap clock ---------------------------------------------------------------------------

// Read local time, hand back the six digits of HH:MM:SS (rtcFormatTime always ends in HH:MM:SS).
static void clockDigits(char d[6]) {
  char tb[24]; rtcFormatTime(tb, sizeof(tb));
  size_t n = strlen(tb);
  const char* t = (n >= 8) ? tb + n - 8 : "00:00:00";
  d[0] = t[0]; d[1] = t[1]; d[2] = t[3]; d[3] = t[4]; d[4] = t[6]; d[5] = t[7];
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
// One glyph, pen baseline at (px,by): each covered pixel is the ink colour scaled by its coverage,
// so edges are smooth on the panel instead of the hard 1-bit steps of a bitmap font.
static void aaGlyph(int px, int by, const AAFont* f, char c, uint8_t r, uint8_t g, uint8_t b) {
  const AAGlyph* gl = aaFind(f, c);
  if (!gl || !gl->w) return;
  const uint8_t* cov = f->cov + gl->off;
  const int ox = px + gl->xoff, oy = by + gl->yoff;
  for (int yy = 0; yy < gl->h; yy++)
    for (int xx = 0; xx < gl->w; xx++) {
      uint8_t a = cov[yy * gl->w + xx];
      if (a) panelPixel(ox + xx, oy + yy,
                        (uint8_t)(r * a / 255), (uint8_t)(g * a / 255), (uint8_t)(b * a / 255));
    }
}
static void aaText(int px, int by, const AAFont* f, const char* s, uint8_t r, uint8_t g, uint8_t b) {
  for (; *s; s++) { aaGlyph(px, by, f, *s, r, g, b); px += aaAdvance(f, *s); }
}

// Local date as "MM/DD" (rtcFormatTime gives "YYYY-MM-DD HH:MM:SS"; no clock yet -> "--/--").
static void clockDate(char* out, size_t n) {
  char tb[24]; rtcFormatTime(tb, sizeof(tb));
  if (strlen(tb) >= 10 && tb[4] == '-' && tb[7] == '-')
    snprintf(out, n, "%c%c/%c%c", tb[5], tb[6], tb[8], tb[9]);
  else snprintf(out, n, "--/--");
}

static void initClock(int, int) {}          // the clock keeps no state -- see renderClock

// A clean digital clock: big anti-aliased HH:MM (JetBrains Mono) in a slowly drifting rainbow,
// with the date bottom-left and the seconds bottom-right in a small face. Real typography, not a
// 7-segment or flap look.
static void renderClock() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  char dg[6]; clockDigits(dg);
  panelClear();
  const int hb = (int)(fxTick / 3);                         // the rainbow drifts slowly along the digits
  uint8_t r, g, b;

  if (H < 48) {                                             // short panel (e.g. 128x32): one row
    const char hms[9] = { dg[0],dg[1],':',dg[2],dg[3],':',dg[4],dg[5],0 };
    const AAFont* f = &AAFONT_MED;
    const AAGlyph* zero = aaFind(f, '0');
    const int capH = zero ? zero->h : f->asc;
    const int by = (H - capH) / 2 + capH;
    int x = (W - aaTextW(f, hms)) / 2; if (x < 0) x = 0;
    for (int i = 0; hms[i]; i++) {
      hsv((uint8_t)(hb + i * 20), r, g, b);
      aaGlyph(x, by, f, hms[i], r, g, b);
      x += aaAdvance(f, hms[i]);
    }
    panelShow();
    return;
  }

  // Tall panel: big HH:MM, date bottom-left, seconds bottom-right (mockup layout #1).
  const char hm[6] = { dg[0], dg[1], ':', dg[2], dg[3], 0 };
  const char ss[3] = { dg[4], dg[5], 0 };
  char date[8]; clockDate(date, sizeof(date));
  const AAFont* big = &AAFONT_BIG;
  const AAFont* sm  = &AAFONT_SMALL;
  const AAGlyph* zero = aaFind(big, '0');
  const int capH  = zero ? zero->h : big->asc;
  const int byBig = (H - sm->lineH - capH) / 2 + capH;      // baseline: caps centred above the row
  int x = (W - aaTextW(big, hm)) / 2; if (x < 0) x = 0;
  for (int i = 0; hm[i]; i++) {
    hsv((uint8_t)(hb + i * 26), r, g, b);
    aaGlyph(x, byBig, big, hm[i], r, g, b);
    x += aaAdvance(big, hm[i]);
  }
  const int byS = H - 2;
  aaText(2, byS, sm, date, 120, 120, 140);                  // date left, muted
  aaText(W - aaTextW(sm, ss) - 2, byS, sm, ss, 90, 180, 235);  // seconds right, cool blue
  panelShow();
}

// ---- Conway's Game of Life ----------------------------------------------------------------------

static void seedLife(int W, int H) {
  if (!fxBuf) return;
  for (int i = 0; i < W * H; i++) fxBuf[i] = (rnd() % 100 < 28) ? 1 : 0;   // ~28% alive
}

static void stepLife(int W, int H) {
  uint8_t* cur = fxBuf;
  uint8_t* nxt = fxBuf + W * H;
  uint32_t pop = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int n = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          int xx = x + dx, yy = y + dy;                 // toroidal wrap
          if (xx < 0) xx = W - 1; else if (xx >= W) xx = 0;
          if (yy < 0) yy = H - 1; else if (yy >= H) yy = 0;
          if (cur[yy * W + xx]) n++;
        }
      uint8_t a = cur[y * W + x], na;
      if (a) na = (n == 2 || n == 3) ? (uint8_t)(a < 255 ? a + 1 : 255) : 0;   // survive (age) / die
      else   na = (n == 3) ? 1 : 0;                                            // birth
      nxt[y * W + x] = na;
      if (na) pop++;
    }
  memcpy(cur, nxt, W * H);
  // A settled or dead board is dull -- reseed once it stops changing for a while.
  if (pop == 0 || pop == lifePop) { if (++lifeStale > 40) { seedLife(W, H); lifeStale = 0; } }
  else lifeStale = 0;
  lifePop = pop;
}

static void renderLife(int W, int H) {
  int gate = 8 - gEffectSpeed / 2; if (gate < 1) gate = 1;
  if (fxTick % gate == 0) stepLife(W, H);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t a = fxBuf[y * W + x];
      if (!a) { panelPixel(x, y, 0, 0, 0); continue; }
      if (a <= 2) panelPixel(x, y, 180, 255, 190);              // newborn: bright white-green
      else { uint8_t g = (uint8_t)(120 + (a > 135 ? 135 : a));  // older: steadier green, glowing
             panelPixel(x, y, 0, g, a > 90 ? 60 : 0); }
    }
  panelShow();
}

// ---- lifecycle ----------------------------------------------------------------------------------

void effectReset(uint8_t type) {
  fxBuildLUTs();
  fxFrame = 0; fxTick = 0;
  const int W = gPanel.panelW, H = gPanel.panelH;
  const int need = 2 * W * H;
  if (fxCap < need) {                     // (re)allocate the shared grid in PSRAM
    if (fxBuf) free(fxBuf);
    fxBuf = (uint8_t*)ps_malloc(need);
    if (!fxBuf) fxBuf = (uint8_t*)malloc(need);
    fxCap = fxBuf ? need : 0;
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
    case EFFECT_CLOCK:     initClock(W, H);    break;
    case EFFECT_LIFE:      lifePop = 0; lifeStale = 0; seedLife(W, H); break;
    default: break;
  }
}

static void renderPlasma() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const uint32_t t = fxFrame;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t a = sinT[(uint8_t)(x * 2 + t)];
      uint8_t b = sinT[(uint8_t)(y * 3 - t)];
      uint8_t c = sinT[(uint8_t)((x + y) + (t >> 1))];
      uint8_t d = sinT[(uint8_t)((x - y) + t)];
      uint8_t idx = (uint8_t)(((int)a + b + c + d) >> 2);
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
  // Doom-style spread: carry each cell up one row with a random sideways DRIFT and a random
  // decay. That asymmetry -- not a symmetric blur -- is what breaks the sheet into flame tongues.
  for (int x = 0; x < W; x++)
    for (int y = 1; y < H; y++) {
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
    for (int k = 0; k < MTRAIL; k++) {
      int yy = hy - k;
      if (yy < 0 || yy >= H) continue;
      if (k == 0) panelPixel(x, yy, 200, 255, 200);      // bright near-white head
      else {
        uint8_t g = (uint8_t)(230 * (MTRAIL - k) / MTRAIL);
        panelPixel(x, yy, 0, g, 0);
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
