// effects.cpp -- plasma, fire and matrix-rain renderers. See effects.h.
//
// Everything is integer/LUT work so a full 128x64 (or 256x64) frame fits comfortably inside one
// panel refresh: a 256-entry sine table and two 256-entry palettes, plus a PSRAM heat buffer for
// fire and small per-column arrays for the rain. panelShow() paces us to the panel's frame rate.
#include <Arduino.h>
#include "effects.h"
#include "display.h"
#include "panel.h"
#include <math.h>
#include <esp_random.h>
#include <string.h>

volatile uint8_t gEffect      = EFFECT_NONE;
volatile uint8_t gEffectSpeed = 4;

#define FX_MAXW 256                       // widest panel the firmware supports

static bool     fxReady = false;          // LUTs built
static uint8_t  sinT[256];                // (sin*0.5+0.5)*255
static uint8_t  plasmaPal[256][3];        // full-hue rainbow
static uint8_t  firePal[256][3];          // black -> red -> orange -> yellow -> white
static uint32_t fxFrame = 0;              // animation phase; advances by gEffectSpeed each frame

// Fire heat, one byte per pixel, row-major. PSRAM so it never eats the WiFi stack's heap.
static uint8_t* fireBuf = nullptr;
static int      fireCap = 0;

// Matrix rain: a falling head per column, position in 1/16-row fixed point.
static int32_t  mHead[FX_MAXW];
static uint8_t  mSpeed[FX_MAXW];          // base fall rate, 1/16 rows/frame
static const int MTRAIL = 14;

// Fast xorshift PRNG. esp_random() reads a hardware register and is far too slow to call once
// per pixel (fire needs ~8k/frame); seeded once from it in fxBuildLUTs().
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
    if      (i < 72)  { r = (uint8_t)(i * 255 / 71);       g = 0;                              b = 0; }
    else if (i < 184) { r = 255;                           g = (uint8_t)((i - 72) * 255 / 111); b = 0; }
    else              { r = 255;                           g = 255;                            b = (uint8_t)((i - 184) * 255 / 71); }
    firePal[i][0] = r; firePal[i][1] = g; firePal[i][2] = b;
  }
  fxRng = esp_random() | 1u;              // seed the fast PRNG once from real entropy
  fxReady = true;
}

uint8_t effectByName(const char* name) {
  if (!name) return EFFECT_NONE;
  if (!strcmp(name, "plasma")) return EFFECT_PLASMA;
  if (!strcmp(name, "fire"))   return EFFECT_FIRE;
  if (!strcmp(name, "matrix")) return EFFECT_MATRIX;
  return EFFECT_NONE;
}
const char* effectName(uint8_t e) {
  switch (e) { case EFFECT_PLASMA: return "plasma";
               case EFFECT_FIRE:   return "fire";
               case EFFECT_MATRIX: return "matrix";
               default:            return "none"; }
}
// One source of truth for the advertised set -- used by GET /api/canvas and /api/capabilities.
const char* effectListJson() { return "[\"plasma\",\"fire\",\"matrix\"]"; }

void effectReset() {
  fxBuildLUTs();
  fxFrame = 0;
  const int W = gPanel.panelW, H = gPanel.panelH;
  const int need = W * H;
  if (fireCap < need) {                   // (re)allocate the heat buffer in PSRAM
    if (fireBuf) free(fireBuf);
    fireBuf = (uint8_t*)ps_malloc(need);
    if (!fireBuf) fireBuf = (uint8_t*)malloc(need);
    fireCap = fireBuf ? need : 0;
  }
  if (fireBuf) memset(fireBuf, 0, need);
  const int cols = W < FX_MAXW ? W : FX_MAXW;
  for (int x = 0; x < cols; x++) {        // stagger the rain so columns don't march in lockstep
    mHead[x]  = -(int32_t)(rnd() % (uint32_t)(H * 16));
    mSpeed[x] = (uint8_t)(4 + (rnd() % 12));
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
  if (!fireBuf) { panelShow(); return; }
  // Bottom row is the source: a flickering orange-yellow base (160..223), with the occasional
  // white-hot spark. Keeping the base below the white tip is what stops it becoming a white slab.
  for (int x = 0; x < W; x++)
    fireBuf[(H - 1) * W + x] = (rnd() & 15) ? (uint8_t)(160 + (rnd() & 63)) : 255;
  // Doom-style spread: carry each cell up one row with a random sideways DRIFT and a random
  // decay. That asymmetry -- not a symmetric blur -- is what breaks the sheet into flame tongues.
  // Reading row y to write row y-1 while y increases means row y still holds last frame's value,
  // so heat climbs one row per frame.
  for (int x = 0; x < W; x++)
    for (int y = 1; y < H; y++) {
      int pixel = fireBuf[y * W + x];
      if (pixel == 0) { fireBuf[(y - 1) * W + x] = 0; continue; }
      uint32_t r = rnd();
      int nx = x + (int)(r & 3) - 1;                  // drift -1..+2
      if (nx < 0) nx = 0; else if (nx >= W) nx = W - 1;
      int decay = 1 + (int)((r >> 2) & 7);            // cool 1..8 per row
      int v = pixel - decay;
      fireBuf[(y - 1) * W + nx] = (uint8_t)(v < 0 ? 0 : v);
    }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t h = fireBuf[y * W + x];
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
  switch (type) {
    case EFFECT_PLASMA: renderPlasma(); break;
    case EFFECT_FIRE:   renderFire();   break;
    case EFFECT_MATRIX: renderMatrix(); break;
    default: break;
  }
}
