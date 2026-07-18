// canvas.cpp -- on-device animation loop + scrolling ticker. See canvas.h.
#include <Arduino.h>
#include "canvas.h"
#include "common.h"
#include "panel.h"
#include "display.h"
#include "effects.h"
#include "font1252.h"
#include <esp_heap_caps.h>
#include <string.h>

volatile bool gAnimActive   = false;
volatile bool gTickerActive = false;

// Both modes clear the others so only one autonomous renderer owns the panel at a time. gEffect /
// gEffectReq live in effects.h; gCanvasMode in common.h. Effects, animation and the ticker are all
// mutually exclusive and all released by dispReturnToWall().
static void claimPanel(volatile bool& mineActive) {
  gEffect = EFFECT_NONE; gEffectReq = EFFECT_REQ_IDLE;
  gAnimActive = false; gTickerActive = false;
  gCanvasMode = false;            // an autonomous renderer supersedes the raw-canvas takeover
  mineActive = true;             // set last, after the conflicting modes are cleared
}

// ---- animation loop ----------------------------------------------------------------------------

#define ANIM_MAX_BYTES  (1536u * 1024u)   // most PSRAM the frame store may claim (~48 rgb565 frames)

static uint8_t*  animBuf = nullptr;        // frames back-to-back in PSRAM, as uploaded
static size_t    animCap = 0;              // bytes currently allocated
static uint16_t  animW = 0, animH = 0, animCount = 0;
static uint8_t   animFmt = 2;              // 2 = rgb565, 3 = rgb888
static bool      animLoop = true;
static size_t    animFrameBytes = 0;
static size_t    animTotal = 0, animWriteOff = 0;
static uint16_t  animIdx = 0;
static uint32_t  animIntervalMs = 66, animLastMs = 0;

uint16_t canvasAnimCount() { return animCount; }

int canvasAnimBegin(uint8_t fmt, uint8_t fps, bool loop, uint16_t w, uint16_t h, uint16_t frames) {
  if (fmt != 2 && fmt != 3)                  return 400;
  if (w != gPanel.panelW || h != gPanel.panelH) return 400;   // full-panel frames only
  if (frames < 1 || fps < 1 || fps > 60)     return 400;
  const size_t frameBytes = (size_t)w * h * fmt;
  const size_t total      = frameBytes * frames;
  if (total > ANIM_MAX_BYTES)                return 413;
  if (animCap < total) {                     // (re)allocate the PSRAM store; parked, so this is safe
    if (animBuf) free(animBuf);
    animBuf = (uint8_t*)ps_malloc(total);
    if (!animBuf) animBuf = (uint8_t*)malloc(total);
    animCap = animBuf ? total : 0;
    if (!animBuf) return 503;
  }
  animW = w; animH = h; animCount = frames; animFmt = fmt; animLoop = loop;
  animFrameBytes = frameBytes; animTotal = total; animWriteOff = 0;
  animIntervalMs = 1000u / fps;
  return 0;
}

void canvasAnimFeed(const uint8_t* data, size_t n) {
  if (!animBuf) return;
  if (animWriteOff + n > animTotal) n = animTotal - animWriteOff;   // ignore any trailing overrun
  memcpy(animBuf + animWriteOff, data, n);
  animWriteOff += n;
}

int canvasAnimCommit() {
  if (!animBuf || animWriteOff != animTotal) return 400;            // short upload
  animIdx = 0; animLastMs = 0;
  claimPanel(gAnimActive);
  return 0;
}

void canvasAnimStop() { gAnimActive = false; }   // buffer kept for the next upload

void canvasAnimRender() {
  if (!animBuf || !animCount) { gAnimActive = false; dispMarkDirty(); return; }
  uint32_t now = millis();
  if (now - animLastMs < animIntervalMs) { vTaskDelay(pdMS_TO_TICKS(2)); return; }
  animLastMs = now;
  const uint8_t* f = animBuf + (size_t)animIdx * animFrameBytes;
  const int W = animW, H = animH;
  if (animFmt == 3) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const uint8_t* p = f + ((size_t)y * W + x) * 3;
        panelPixel(x, y, p[0], p[1], p[2]);
      }
  } else {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        const uint8_t* p = f + ((size_t)y * W + x) * 2;
        uint16_t v = ((uint16_t)p[0] << 8) | p[1];               // big-endian rgb565
        panelPixel(x, y, (uint8_t)(((v >> 11) & 0x1F) << 3),
                         (uint8_t)(((v >> 5)  & 0x3F) << 2),
                         (uint8_t)((v & 0x1F) << 3));
      }
  }
  panelShow();
  if (++animIdx >= animCount) {
    if (animLoop) animIdx = 0;
    else { gAnimActive = false; dispMarkDirty(); }               // one-shot: back to the wall
  }
}

// ---- scrolling ticker --------------------------------------------------------------------------

static char            tickText[192];
static uint8_t         tickR = 255, tickG = 255, tickB = 255;
static int             tickSpeed = 2;                 // px per step
static int             tickScroll = 0, tickTextW = 0;
static const Font1252* tickFont = &FONT_6x10;
static uint32_t        tickLastMs = 0;

static const Font1252* tickerFace() {
  int H = gPanel.panelH;
  if (H >= 60) return &FONT_10x20;
  if (H >= 28) return &FONT_8x13;
  return &FONT_6x9;
}

static void drawGlyph1252(int px, int py, const Font1252* f, char c, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t gi = FONT1252_INDEX[(uint8_t)c];
  if (gi == 0xFF) return;
  for (int row = 0; row < f->height; row++) {
    uint16_t bits = font1252Row(*f, gi, (uint8_t)row);
    for (int col = 0; col < f->width; col++)
      if (bits & (uint16_t)(0x8000 >> col)) panelPixel(px + col, py + row, r, g, b);
  }
}

void canvasTickerSet(const char* text, uint8_t r, uint8_t g, uint8_t b, int speed) {
  gTickerActive = false;                     // stop the renderer reading half-updated state
  strlcpy(tickText, text ? text : "", sizeof(tickText));
  tickR = r; tickG = g; tickB = b;
  tickSpeed = speed < 1 ? 1 : (speed > 20 ? 20 : speed);
  tickFont = tickerFace();
  const int gw = tickFont->width + 1;        // 1 px between glyphs
  tickTextW = (int)strlen(tickText) * gw;
  tickScroll = 0; tickLastMs = 0;
  claimPanel(gTickerActive);
}

void canvasTickerStop() { gTickerActive = false; }

void canvasTickerRender() {
  uint32_t now = millis();
  if (now - tickLastMs < 33) { vTaskDelay(pdMS_TO_TICKS(2)); return; }   // ~30 steps/s
  tickLastMs = now;
  const Font1252* f = tickFont;
  const int gw = f->width + 1;
  const int y0 = (gPanel.panelH - f->height) / 2;
  const int startX = gPanel.panelW - tickScroll;      // enters from the right, scrolls left
  panelClear();
  for (size_t i = 0; tickText[i]; i++) {
    int gx = startX + (int)i * gw;
    if (gx >= gPanel.panelW) break;                    // this glyph and the rest are off the right
    if (gx + f->width < 0) continue;                   // already off the left
    drawGlyph1252(gx, y0, f, tickText[i], tickR, tickG, tickB);
  }
  panelShow();
  tickScroll += tickSpeed;
  if (tickScroll > tickTextW + gPanel.panelW) tickScroll = 0;   // whole line has passed: wrap
}
