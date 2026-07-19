// canvas.cpp -- on-device animation loop + scrolling ticker. See canvas.h.
#include <Arduino.h>
#include "canvas.h"
#include "common.h"
#include "panel.h"
#include "display.h"
#include "effects.h"
#include "font1252.h"
#include <string.h>
#include <FFat.h>
#include <AnimatedGIF.h>   // GIF import (v2.1): decode an upload into the animation store
#include <new>             // placement-new the decoder state into PSRAM

extern bool sfFsReady;   // FATFS mounted (modules.h)

volatile bool gAnimActive   = false;
volatile bool gTickerActive = false;

// Both modes clear the others so only one autonomous renderer owns the panel at a time. gEffect /
// gEffectReq live in effects.h; gCanvasMode in common.h. Effects, animation and the ticker are all
// mutually exclusive and all released by dispReturnToWall().
static void claimPanel(volatile bool& mineActive) {
  gEffect = EFFECT_NONE; gEffectReq = EFFECT_REQ_IDLE;
  gAnimActive = false;
  if (!gTickerOverlay) gTickerActive = false;   // an overlay ticker rides on top; only an
                                                // exclusive ticker is a competing owner
  gCanvasMode = false;            // an autonomous renderer supersedes the raw-canvas takeover
  mineActive = true;             // set last, after the conflicting modes are cleared
}

// ---- animation loop ----------------------------------------------------------------------------

#define ANIM_MAX_BYTES  (8192u * 1024u)   // most PSRAM the frame store may claim. The Waveshare
                                          // board has 16 MB of octal PSRAM; 8 MB is ~256 rgb565
                                          // frames on a 256x64 panel (was 1.5 MB / ~48 on the
                                          // MatrixPortal's 2 MB quad part)

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

// ---- frame transitions (v2.1) ------------------------------------------------------------------
// A full-frame canvas PUT can present via a short on-device transition instead of a hard
// cut: the incoming frame is staged in PSRAM, the outgoing frame is captured from the
// framebuffer, and the tween is rendered here. Set once via POST /api/canvas/transition;
// applies to subsequent full-frame PUTs (rect/qoi/anim are untouched).
volatile uint8_t  gTransType = 0;      // 0 none, 1 crossfade, 2 wipe, 3 slide
volatile uint16_t gTransMs   = 400;

static uint8_t* stageBuf = nullptr;    // incoming frame, as-uploaded (rgb888 or rgb565 BE)
static uint8_t* oldBuf   = nullptr;    // outgoing frame, rgb888 via panelReadback
static size_t   stageCap = 0, oldCap = 0, stageOff = 0;
static uint8_t  stageBpp = 3;

bool canvasStageBegin(uint8_t bpp) {
  const size_t need    = (size_t)gPanel.panelW * gPanel.panelH * bpp;
  const size_t needOld = (size_t)gPanel.panelW * gPanel.panelH * 3;
  if (stageCap < need) {
    if (stageBuf) free(stageBuf);
    stageBuf = (uint8_t*)ps_malloc(need);
    stageCap = stageBuf ? need : 0;
  }
  if (oldCap < needOld) {
    if (oldBuf) free(oldBuf);
    oldBuf = (uint8_t*)ps_malloc(needOld);
    oldCap = oldBuf ? needOld : 0;
  }
  if (!stageBuf || !oldBuf) return false;
  stageBpp = bpp; stageOff = 0;
  panelReadback(oldBuf, false);          // capture the outgoing frame before any new pixels
  return true;
}

void canvasStageFeed(const uint8_t* data, size_t n) {
  if (!stageBuf) return;
  const size_t total = (size_t)gPanel.panelW * gPanel.panelH * stageBpp;
  if (stageOff + n > total) n = total - stageOff;
  memcpy(stageBuf + stageOff, data, n);
  stageOff += n;
}

static inline void stagePx(int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
  const size_t i = ((size_t)y * gPanel.panelW + x) * stageBpp;
  if (stageBpp == 3) { r = stageBuf[i]; g = stageBuf[i+1]; b = stageBuf[i+2]; return; }
  const uint16_t v = ((uint16_t)stageBuf[i] << 8) | stageBuf[i+1];
  r = (uint8_t)(((v >> 11) & 0x1F) << 3);
  g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
  b = (uint8_t)(( v        & 0x1F) << 3);
}

// Present the staged frame through the configured transition. Runs on taskWeb; each
// tween step is paced by panelShow()'s one-frame yield, and the web watchdog is fed.
void canvasStagePresent() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const uint8_t type = gTransType;
  // Steps at ~30 fps for the configured duration, bounded sane.
  int steps = (int)((uint32_t)gTransMs / 33);
  if (steps < 2) steps = 2;
  if (steps > 30) steps = 30;
  uint8_t r, g, b;
  if (type == 1) {                        // crossfade
    for (int s = 1; s <= steps; s++) {
      const int t = 255 * s / steps;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          const uint8_t* o = oldBuf + ((size_t)y * W + x) * 3;
          stagePx(x, y, r, g, b);
          panelPixel(x, y, (uint8_t)((o[0] * (255 - t) + r * t) >> 8),
                           (uint8_t)((o[1] * (255 - t) + g * t) >> 8),
                           (uint8_t)((o[2] * (255 - t) + b * t) >> 8));
        }
      panelShow();
      wdgWebMs = millis();
    }
  } else if (type == 2) {                 // wipe, left to right
    panelCloneToBack();                   // keep the old frame under the un-wiped part
    for (int s = 1; s <= steps; s++) {
      const int xTo = W * s / steps;
      for (int y = 0; y < H; y++)
        for (int x = 0; x < xTo; x++) { stagePx(x, y, r, g, b); panelPixel(x, y, r, g, b); }
      panelShow();
      panelCloneToBack();
      wdgWebMs = millis();
    }
  } else if (type == 3) {                 // slide: new frame pushes the old off to the left
    for (int s = 1; s <= steps; s++) {
      const int dx = W * s / steps;       // how far everything has moved left
      for (int y = 0; y < H; y++) {
        for (int x = 0; x < W - dx; x++) {           // remaining slice of the old frame
          const uint8_t* o = oldBuf + ((size_t)y * W + (x + dx)) * 3;
          panelPixel(x, y, o[0], o[1], o[2]);
        }
        for (int x = W - dx; x < W; x++) {           // incoming slice of the new frame
          stagePx(x - (W - dx), y, r, g, b);
          panelPixel(x, y, r, g, b);
        }
      }
      panelShow();
      wdgWebMs = millis();
    }
  }
  // Land exactly on the new frame regardless of type (also the type==0 direct path).
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) { stagePx(x, y, r, g, b); panelPixel(x, y, r, g, b); }
  panelShow();
}

// ---- animation library (v2.1) ------------------------------------------------------------------
// Named animations on FATFS: /anim/<name>.mpg holds the raw 14-byte MPGA header + frames,
// byte-identical to the PUT /api/canvas/anim wire format. The PSRAM store above is the one
// playback cache; play-by-name streams the file back through the same Begin/Feed/Commit path
// the HTTP upload uses, so the two entry points cannot drift.

bool canvasAnimNameOk(const char* name) {
  if (!name || !*name) return false;
  size_t n = strlen(name);
  if (n > ANIM_NAME_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}

static void animPath(const char* name, char* out, size_t outLen) {
  snprintf(out, outLen, "/anim/%s.mpg", name);
}

// Write the CURRENT store back out as a named file. The header is reconstructed from the
// playback fields, so whatever is loaded -- HTTP upload or a previous load -- can be saved.
int canvasAnimSave(const char* name) {
  if (!canvasAnimNameOk(name))            return 400;
  if (!sfFsReady)                         return 503;
  if (!animBuf || !animTotal || !animCount) return 409;   // nothing loaded to save
  FFat.mkdir("/anim");                                    // idempotent
  char tmp[48], path[48];
  snprintf(tmp, sizeof(tmp), "/anim/%s.tmp", name);
  animPath(name, path, sizeof(path));
  File f = FFat.open(tmp, "w");
  if (!f) return 507;                                     // out of FATFS space / fd
  uint8_t hdr[14] = { 'M','P','G','A', 1, animFmt,
                      (uint8_t)(1000u / (animIntervalMs ? animIntervalMs : 66)),
                      (uint8_t)(animLoop ? 1 : 0),
                      (uint8_t)(animW >> 8), (uint8_t)animW,
                      (uint8_t)(animH >> 8), (uint8_t)animH,
                      (uint8_t)(animCount >> 8), (uint8_t)animCount };
  bool ok = f.write(hdr, sizeof(hdr)) == sizeof(hdr);
  // 8 KB slices keep taskWeb's watchdog fed via the caller between requests; FFat
  // write speed makes a 6 MB save take a few seconds.
  for (size_t off = 0; ok && off < animTotal; off += 8192) {
    size_t c = (animTotal - off < 8192) ? (animTotal - off) : 8192;
    ok = f.write(animBuf + off, c) == c;
    wdgWebMs = millis();
  }
  f.close();
  if (!ok) { FFat.remove(tmp); return 507; }
  FFat.remove(path);                                      // replace atomically-ish
  FFat.rename(tmp, path);
  return 0;
}

// Load /anim/<name>.mpg into the PSRAM store and start playback. Streams through the
// same Begin/Feed/Commit path as the HTTP upload.
int canvasAnimLoadPlay(const char* name) {
  if (!canvasAnimNameOk(name)) return 400;
  if (!sfFsReady)              return 503;
  char path[48];
  animPath(name, path, sizeof(path));
  File f = FFat.open(path, "r");
  if (!f) return 404;
  uint8_t hdr[14];
  if (f.read(hdr, 14) != 14 || memcmp(hdr, "MPGA", 4) != 0 || hdr[4] != 1) { f.close(); return 400; }
  uint16_t w  = (hdr[8]  << 8) | hdr[9];
  uint16_t h  = (hdr[10] << 8) | hdr[11];
  uint16_t fr = (hdr[12] << 8) | hdr[13];
  int rc = canvasAnimBegin(hdr[5], hdr[6], hdr[7] & 1, w, h, fr);
  if (rc) { f.close(); return rc; }
  static uint8_t buf[8192];        // single caller (taskWeb / boot); off the stack
  size_t got;
  while ((got = f.read(buf, sizeof(buf))) > 0) {
    canvasAnimFeed(buf, got);
    wdgWebMs = millis();
  }
  f.close();
  return canvasAnimCommit();       // 400 on a truncated file
}

int canvasAnimDelete(const char* name) {
  if (!canvasAnimNameOk(name)) return 400;
  if (!sfFsReady)              return 503;
  char path[48];
  animPath(name, path, sizeof(path));
  if (!FFat.exists(path))      return 404;
  return FFat.remove(path) ? 0 : 507;
}

// Stream the library as a JSON array. Each entry re-reads its file header for the
// metadata, so the list is always the truth on disk.
void canvasAnimList(void (*sink)(const char*)) {
  sink("[");
  if (sfFsReady) {
    File dir = FFat.open("/anim");
    bool first = true;
    if (dir && dir.isDirectory()) {
      File f;
      while ((f = dir.openNextFile())) {
        const char* fn = f.name();                 // basename on FFat
        size_t len = strlen(fn);
        if (len > 4 && strcmp(fn + len - 4, ".mpg") == 0) {
          char name[ANIM_NAME_MAX + 1] = {0};
          size_t n = len - 4;
          if (n <= ANIM_NAME_MAX) {
            memcpy(name, fn, n);
            uint8_t hdr[14] = {0};
            size_t sz = f.size();
            f.read(hdr, 14);
            char row[160];
            snprintf(row, sizeof(row),
              "%s{\"name\":\"%s\",\"bytes\":%u,\"frames\":%u,\"w\":%u,\"h\":%u,"
              "\"fps\":%u,\"loop\":%s}",
              first ? "" : ",", name, (unsigned)sz,
              (unsigned)((hdr[12] << 8) | hdr[13]),
              (unsigned)((hdr[8] << 8) | hdr[9]), (unsigned)((hdr[10] << 8) | hdr[11]),
              (unsigned)hdr[6], (hdr[7] & 1) ? "true" : "false");
            sink(row);
            first = false;
          }
        }
        f.close();
        wdgWebMs = millis();
      }
    }
    if (dir) dir.close();
  }
  sink("]");
}

// ---- sprite atlas (v2.1) -----------------------------------------------------------------------
// One tile sheet in PSRAM: tiles*tileW*tileH pixels back-to-back, rgb565 BE or rgb888, as
// uploaded (PUT /api/canvas/atlas). The ops path blits tiles with the "sprite" op. Kept across
// uses; the next upload replaces it. Streamed through the same Begin/Feed/Commit shape as the
// animation store, so the web handler stays a mirror of the anim one.

#define ATLAS_MAX_BYTES  (2048u * 1024u)   // most PSRAM the tile sheet may claim

static uint8_t*  atlasBuf = nullptr;       // tiles back-to-back in PSRAM, as uploaded
static size_t    atlasCap = 0;             // bytes currently allocated
static uint16_t  atlasW = 0, atlasH = 0, atlasTiles = 0;
static uint8_t   atlasFmt = 2;             // 2 = rgb565 BE, 3 = rgb888
static size_t    atlasTileBytes = 0;
static size_t    atlasTotal = 0, atlasWriteOff = 0;
static bool      atlasReady = false;       // false while an upload is (re)filling the buffer

int canvasAtlasBegin(uint8_t fmt, uint16_t tileW, uint16_t tileH, uint16_t tiles) {
  if (fmt != 2 && fmt != 3)          return 400;
  if (!tileW || !tileH || !tiles)    return 400;
  const size_t tileBytes = (size_t)tileW * tileH * fmt;
  const size_t total     = tileBytes * tiles;
  if (total > ATLAS_MAX_BYTES)       return 413;
  atlasReady = false;                        // an in-flight ops batch must not blit a half sheet
  if (atlasCap < total) {                    // (re)allocate; a 2 MB sheet only ever fits PSRAM
    if (atlasBuf) free(atlasBuf);
    atlasBuf = (uint8_t*)ps_malloc(total);
    atlasCap = atlasBuf ? total : 0;
    if (!atlasBuf) return 503;
  }
  atlasW = tileW; atlasH = tileH; atlasTiles = tiles; atlasFmt = fmt;
  atlasTileBytes = tileBytes; atlasTotal = total; atlasWriteOff = 0;
  return 0;
}

void canvasAtlasFeed(const uint8_t* data, size_t n) {
  if (!atlasBuf) return;
  if (atlasWriteOff + n > atlasTotal) n = atlasTotal - atlasWriteOff;   // ignore trailing overrun
  memcpy(atlasBuf + atlasWriteOff, data, n);
  atlasWriteOff += n;
}

int canvasAtlasCommit() {
  if (!atlasBuf || atlasWriteOff != atlasTotal) return 400;             // short upload
  atlasReady = true;
  return 0;
}

bool canvasAtlasInfo(uint16_t& tiles, uint16_t& w, uint16_t& h) {
  if (!atlasReady) return false;
  tiles = atlasTiles; w = atlasW; h = atlasH;
  return true;
}

// Blit tile i at (x,y). Magenta is the transparent colour -- 0xF81F in rgb565, (255,0,255) in
// rgb888 -- so sprites can carry holes. panelPixel clamps, so an off-panel sprite just clips.
bool canvasAtlasBlit(uint16_t i, int x, int y) {
  if (!atlasReady || i >= atlasTiles) return false;
  const uint8_t* t = atlasBuf + (size_t)i * atlasTileBytes;
  for (int row = 0; row < atlasH; row++)
    for (int col = 0; col < atlasW; col++) {
      const uint8_t* p = t + ((size_t)row * atlasW + col) * atlasFmt;
      if (atlasFmt == 3) {
        if (p[0] == 255 && p[1] == 0 && p[2] == 255) continue;          // transparent
        panelPixel(x + col, y + row, p[0], p[1], p[2]);
      } else {
        const uint16_t v = ((uint16_t)p[0] << 8) | p[1];                // big-endian rgb565
        if (v == 0xF81F) continue;                                      // transparent
        panelPixel(x + col, y + row, (uint8_t)(((v >> 11) & 0x1F) << 3),
                                     (uint8_t)(((v >> 5)  & 0x3F) << 2),
                                     (uint8_t)((v & 0x1F) << 3));
      }
    }
  return true;
}

// ---- GIF import (v2.1) -------------------------------------------------------------------------
// PUT /api/canvas/gif: the whole file is buffered in PSRAM by the web handler, then decoded here
// into the SAME animation store the MPGA upload fills -- Begin/Feed/Commit, so playback, save and
// boot-anim all work on an imported GIF exactly as on a native upload. Each GIF frame is
// composited onto a persistent full-panel rgb565 canvas (smaller GIFs centred on black), because
// GIF frames are deltas: transparency keeps what is underneath, and the disposal method decides
// what survives into the next frame.

static uint16_t* gifCanvas = nullptr;      // full-panel compose surface, rgb565 BE entries
static size_t    gifCanvasCap = 0;         // uint16 entries allocated
static int       gifOx = 0, gifOy = 0;     // centring offset of the GIF canvas on the panel
// The current frame's rectangle + disposal, captured by the draw callback and applied
// AFTER the frame has been fed (disposal describes what happens between frames).
static int       gifFrX = 0, gifFrY = 0, gifFrW = 0, gifFrH = 0;
static uint8_t   gifFrDisp = 0;

// AnimatedGIF line callback: 8-bit palette indices + a big-endian rgb565 palette (begin() below
// asks for BE, so entries memcpy straight into the store's byte order). Transparent pixels keep
// whatever the previous frame left on the canvas -- that is how GIF deltas accumulate.
static void gifDrawLine(GIFDRAW* d) {
  gifFrX = d->iX; gifFrY = d->iY; gifFrW = d->iWidth; gifFrH = d->iHeight;
  gifFrDisp = d->ucDisposalMethod;
  const int y = gifOy + d->iY + d->y;
  if (y < 0 || y >= gPanel.panelH) return;
  uint16_t* row = gifCanvas + (size_t)y * gPanel.panelW;
  const int x0 = gifOx + d->iX;
  for (int i = 0; i < d->iWidth; i++) {
    const int x = x0 + i;
    if (x < 0 || x >= gPanel.panelW) continue;
    const uint8_t px = d->pPixels[i];
    if (d->ucHasTransparency && px == d->ucTransparent) continue;
    row[x] = d->pPalette[px];
  }
}

int canvasGifImport(uint8_t* data, size_t len, uint16_t* frames, uint8_t* fps,
                    const char** errMsg) {
  *frames = 0; *fps = 0;
  *errMsg = "Not a decodable GIF";
  // The decoder state is ~24 KB (LZW tables, palettes, line buffer) -- placement-new it into
  // PSRAM rather than pile it onto the internal heap the panel and lwIP already contend for.
  void* mem = ps_malloc(sizeof(AnimatedGIF));
  if (!mem) { *errMsg = "Out of memory"; return 503; }
  AnimatedGIF* gif = new (mem) AnimatedGIF();
  gif->begin(GIF_PALETTE_RGB565_BE);
  int rc = 400;
  do {
    if (!gif->open(data, (int)len, gifDrawLine)) break;
    const int cw = gif->getCanvasWidth(), ch = gif->getCanvasHeight();
    if (cw < 1 || ch < 1) break;
    if (cw > gPanel.panelW || ch > gPanel.panelH) { *errMsg = "GIF larger than the panel"; break; }
    // First pass: frame count and total duration, for the store size and playback rate.
    GIFINFO info;
    if (!gif->getInfo(&info) || info.iFrameCount < 1) break;
    if (info.iFrameCount > 0xFFFF) { *errMsg = "GIF exceeds the animation store"; rc = 413; break; }
    const uint16_t fc = (uint16_t)info.iFrameCount;
    long avgMs = (info.iDuration > 0) ? info.iDuration / info.iFrameCount : 100;
    if (avgMs < 1) avgMs = 1;
    long f = 1000 / avgMs;                       // clamp to what the render loop can pace
    *fps = (uint8_t)(f < 1 ? 1 : (f > 30 ? 30 : f));
    // The compose canvas: full panel, persistent across frames so deltas accumulate.
    const size_t entries = (size_t)gPanel.panelW * gPanel.panelH;
    if (gifCanvasCap < entries) {
      if (gifCanvas) free(gifCanvas);
      gifCanvas = (uint16_t*)ps_malloc(entries * 2);
      gifCanvasCap = gifCanvas ? entries : 0;
      if (!gifCanvas) { *errMsg = "Out of memory"; rc = 503; break; }
    }
    memset(gifCanvas, 0, entries * 2);           // black; a smaller GIF is centred on it
    gifOx = (gPanel.panelW - cw) / 2;
    gifOy = (gPanel.panelH - ch) / 2;
    rc = canvasAnimBegin(2, *fps, true, gPanel.panelW, gPanel.panelH, fc);
    if (rc) {
      *errMsg = (rc == 413) ? "GIF exceeds the animation store" : "Out of memory";
      break;
    }
    // Second pass: decode, compose, feed. getInfo left the file position at the end,
    // so rewind first. playFrame: 1 = more frames, 0 = last frame done, <0 = error.
    gif->reset();
    uint16_t fed = 0;
    while (fed < fc) {
      gifFrW = 0;
      const int more = gif->playFrame(false, nullptr, nullptr);
      if (more < 0) break;                       // truncated: Commit's short-upload 400 answers
      if (more == 0 && gif->getLastError() != GIF_SUCCESS &&
          gif->getLastError() != GIF_EMPTY_FRAME) break;
      canvasAnimFeed((const uint8_t*)gifCanvas, entries * 2);
      fed++;
      // Disposal happens BETWEEN frames: method 2 clears the frame's rectangle back to
      // black; 0/1 (and the rare 3) keep the canvas, so the next delta lands on top.
      if (gifFrDisp == 2 && gifFrW > 0) {
        for (int yy = 0; yy < gifFrH; yy++) {
          const int y = gifOy + gifFrY + yy;
          if (y < 0 || y >= gPanel.panelH) continue;
          for (int xx = 0; xx < gifFrW; xx++) {
            const int x = gifOx + gifFrX + xx;
            if (x >= 0 && x < gPanel.panelW) gifCanvas[(size_t)y * gPanel.panelW + x] = 0;
          }
        }
      }
      wdgWebMs = millis();                       // a long GIF must not trip the web watchdog
      if (more == 0) break;
    }
    *frames = fed;
    rc = canvasAnimCommit();                     // 400 when the decode came up short
    if (rc) *errMsg = "Truncated or undecodable GIF";
  } while (false);
  gif->close();
  gif->~AnimatedGIF();
  free(mem);
  return rc;
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

volatile bool gTickerOverlay = false;
static bool tickBand = true;

// Overlay draw pass, installed as the panelShow() hook while an overlay ticker runs: a
// lower-third band + the scrolled text, drawn into every presented frame regardless of
// who rendered it. Uses only pixel-level calls; never calls panelShow.
static void tickerOverlayDraw() {
  // Advance the scroll here, time-gated: the hook runs once per presented frame,
  // whoever presents it (effect at ~70 fps, animation, canvas push, wall repaint),
  // so the overlay animates without the base renderer's cooperation.
  uint32_t now = millis();
  if (now - tickLastMs >= 33) {
    tickLastMs = now;
    tickScroll += tickSpeed;
    if (tickScroll > tickTextW + gPanel.panelW) tickScroll = 0;
  }
  const Font1252* f = tickFont;
  const int gw = f->width + 1;
  const int bandH = f->height + 2;
  const int by = gPanel.panelH - bandH;
  if (tickBand) panelFillRect(0, by, gPanel.panelW, bandH, 0, 0, 0);
  const int y0 = by + 1;
  const int startX = gPanel.panelW - tickScroll;
  for (size_t i = 0; tickText[i]; i++) {
    int gx = startX + (int)i * gw;
    if (gx >= gPanel.panelW) break;
    if (gx + f->width < 0) continue;
    dispDrawGlyph1252(gx, y0, f, (uint8_t)tickText[i], 0, 255, tickR, tickG, tickB);
  }
}

void canvasTickerSet(const char* text, uint8_t r, uint8_t g, uint8_t b, int speed,
                     bool overlay, bool band, const Font1252* font) {
  gTickerActive = false;                     // stop the renderer reading half-updated state
  panelSetOverlay(nullptr);
  gTickerOverlay = false;
  // UTF-8 -> CP1252 at the door (v3.0.1): the renderer walks bytes, so a raw copy drew
  // multi-byte characters as garbage pairs and inflated tickTextW.
  utf8ToCp1252(text ? text : "", tickText, sizeof(tickText));
  tickR = r; tickG = g; tickB = b; tickBand = band;
  tickSpeed = speed < 1 ? 1 : (speed > 20 ? 20 : speed);
  tickFont = font ? font : tickerFace();     // v2.1: an uploaded face, or the panel-sized default
  const int gw = tickFont->width + 1;        // 1 px between glyphs
  tickTextW = (int)strlen(tickText) * gw;
  tickScroll = 0; tickLastMs = 0;
  if (overlay) {
    // Composite over whatever is presenting: no panel claim, no mode change.
    gTickerOverlay = true;
    gTickerActive  = true;
    panelSetOverlay(tickerOverlayDraw);
  } else {
    claimPanel(gTickerActive);
  }
}

void canvasTickerStop() {
  if (gTickerOverlay) return;                // overlay survives wall/page changes
  gTickerActive = false;
}

void canvasTickerStopForce() {
  panelSetOverlay(nullptr);
  gTickerOverlay = false;
  gTickerActive  = false;
}

// Overlay mode: advance the scroll on the same ~30 steps/s clock the exclusive ticker
// uses. When the base layer is the idle wall nothing would repaint (and so nothing would
// present the moved overlay), so mark the wall dirty each step.
void canvasTickerTick(uint32_t now) {
  // The scroll itself advances inside tickerOverlayDraw (per presented frame). This
  // tick exists for one case: the IDLE wall, which presents nothing on its own --
  // mark it dirty at the scroll cadence so repaints keep coming.
  if (!gTickerOverlay || !gTickerActive) return;
  (void)now;
  dispMarkDirty();
}

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
    dispDrawGlyph1252(gx, y0, f, (uint8_t)tickText[i], 0, 255, tickR, tickG, tickB);
  }
  panelShow();
  tickScroll += tickSpeed;
  if (tickScroll > tickTextW + gPanel.panelW) tickScroll = 0;   // whole line has passed: wrap
}

// ---- uploadable fonts (v2.1) -------------------------------------------------------------------
// One custom face in PSRAM, presented as an ordinary Font1252 so dispDrawGlyph1252/font1252Row
// draw it unchanged (they read `rows` through a plain pointer -- PSRAM is just memory to them).
// The MPFT blob carries the 216 CP1252 glyphs only; the table is still sized FONT1252_TOTAL
// glyphs with the pictograph slots zeroed, because font1252Row's bound is FONT1252_TOTAL and a
// shorter table would let a pictograph index read past the end. Named faces persist to FATFS as
// /fonts/<name>.fnt (the raw blob), mirroring the animation library's save/load/delete/list.

static uint16_t* fontRows = nullptr;       // FONT1252_TOTAL*height rows, native-endian, in PSRAM
static size_t    fontRowsCap = 0;          // uint16 entries allocated
static Font1252  fontCustom = { 0, 0, 0, nullptr };
static bool      fontLoaded = false;

// Parse an MPFT blob into the slot. 0 on success, else the HTTP status for the reply.
int canvasFontInstall(const uint8_t* blob, size_t len) {
  if (len < 8 || memcmp(blob, "MPFT", 4) != 0 || blob[4] != 1) return 400;
  const uint8_t w = blob[5], h = blob[6], a = blob[7];
  if (!w || w > 16 || !h || a > h)                             return 400;
  if (len != 8 + (size_t)FONT1252_GLYPHS * h * 2)              return 400;
  const size_t entries = (size_t)FONT1252_TOTAL * h;
  if (fontRowsCap < entries) {               // (re)allocate; growing frees the old table, so
    fontLoaded = false;                      // nothing may still point at it --
    if (tickFont == &fontCustom) tickFont = tickerFace();   // -- least of all a running ticker
    if (fontRows) free(fontRows);
    fontRows = (uint16_t*)ps_malloc(entries * 2);
    fontRowsCap = fontRows ? entries : 0;
    if (!fontRows) return 503;
  }
  memset(fontRows, 0, entries * 2);          // pictograph slots stay blank, not garbage
  const uint8_t* p = blob + 8;
  for (size_t i = 0; i < (size_t)FONT1252_GLYPHS * h; i++, p += 2)
    fontRows[i] = (uint16_t)((p[0] << 8) | p[1]);            // big-endian rows on the wire
  fontCustom.width = w; fontCustom.height = h; fontCustom.ascent = a;
  fontCustom.rows  = fontRows;
  fontLoaded = true;
  return 0;
}

bool canvasFontInfo(uint8_t& w, uint8_t& h, uint8_t& ascent) {
  if (!fontLoaded) return false;
  w = fontCustom.width; h = fontCustom.height; ascent = fontCustom.ascent;
  return true;
}

static void fontPath(const char* name, char* out, size_t outLen) {
  snprintf(out, outLen, "/fonts/%s.fnt", name);
}

// Write the CURRENT slot back out as a named file. The blob is reconstructed from the parsed
// table -- header from the face fields, rows re-encoded big-endian -- like canvasAnimSave.
int canvasFontSave(const char* name) {
  if (!canvasAnimNameOk(name)) return 400;   // same 1..24 [a-z0-9_-] rule as the anim library
  if (!sfFsReady)              return 503;
  if (!fontLoaded)             return 409;   // nothing loaded to save
  FFat.mkdir("/fonts");                      // idempotent
  char tmp[48], path[48];
  snprintf(tmp, sizeof(tmp), "/fonts/%s.tmp", name);
  fontPath(name, path, sizeof(path));
  File f = FFat.open(tmp, "w");
  if (!f) return 507;                        // out of FATFS space / fd
  uint8_t hdr[8] = { 'M','P','F','T', 1, fontCustom.width, fontCustom.height, fontCustom.ascent };
  bool ok = f.write(hdr, sizeof(hdr)) == sizeof(hdr);
  const size_t total = (size_t)FONT1252_GLYPHS * fontCustom.height;   // uint16 rows to write
  uint8_t chunk[512];
  for (size_t off = 0; ok && off < total; ) {
    size_t n = 0;
    while (n + 2 <= sizeof(chunk) && off < total) {
      chunk[n++] = (uint8_t)(fontRows[off] >> 8);
      chunk[n++] = (uint8_t)fontRows[off];
      off++;
    }
    ok = f.write(chunk, n) == n;
    wdgWebMs = millis();
  }
  f.close();
  if (!ok) { FFat.remove(tmp); return 507; }
  FFat.remove(path);                         // replace atomically-ish
  FFat.rename(tmp, path);
  return 0;
}

// Load /fonts/<name>.fnt into the slot. The blob is small (<= 64 KB), so it is read whole
// and handed to the same Install path the HTTP upload uses -- the two cannot drift.
static int canvasFontLoad(const char* name) {
  if (!canvasAnimNameOk(name)) return 400;
  if (!sfFsReady)              return 503;
  char path[48];
  fontPath(name, path, sizeof(path));
  File f = FFat.open(path, "r");
  if (!f) return 404;
  const size_t sz = f.size();
  if (sz < 8 || sz > CANVAS_FONT_MAX_BYTES) { f.close(); return 400; }
  uint8_t* blob = (uint8_t*)ps_malloc(sz);
  if (!blob) blob = (uint8_t*)malloc(sz);
  if (!blob) { f.close(); return 503; }
  const bool ok = f.read(blob, sz) == sz;
  f.close();
  const int rc = ok ? canvasFontInstall(blob, sz) : 400;
  free(blob);
  return rc;
}

int canvasFontDelete(const char* name) {
  if (!canvasAnimNameOk(name)) return 400;
  if (!sfFsReady)              return 503;
  char path[48];
  fontPath(name, path, sizeof(path));
  if (!FFat.exists(path))      return 404;
  return FFat.remove(path) ? 0 : 507;
}

// Stream the font library as a JSON array; each entry re-reads its file header, so the
// list is always the truth on disk (like canvasAnimList).
void canvasFontList(void (*sink)(const char*)) {
  sink("[");
  if (sfFsReady) {
    File dir = FFat.open("/fonts");
    bool first = true;
    if (dir && dir.isDirectory()) {
      File f;
      while ((f = dir.openNextFile())) {
        const char* fn = f.name();                 // basename on FFat
        size_t len = strlen(fn);
        if (len > 4 && strcmp(fn + len - 4, ".fnt") == 0) {
          char name[ANIM_NAME_MAX + 1] = {0};
          size_t n = len - 4;
          if (n <= ANIM_NAME_MAX) {
            memcpy(name, fn, n);
            uint8_t hdr[8] = {0};
            size_t sz = f.size();
            f.read(hdr, 8);
            char row[128];
            snprintf(row, sizeof(row),
              "%s{\"name\":\"%s\",\"bytes\":%u,\"w\":%u,\"h\":%u,\"ascent\":%u}",
              first ? "" : ",", name, (unsigned)sz,
              (unsigned)hdr[5], (unsigned)hdr[6], (unsigned)hdr[7]);
            sink(row);
            first = false;
          }
        }
        f.close();
        wdgWebMs = millis();
      }
    }
    if (dir) dir.close();
  }
  sink("]");
}

const Font1252* canvasFontByName(const char* name) {
  if (!name || !*name) return nullptr;
  if (strcmp(name, "custom") == 0) return fontLoaded ? &fontCustom : nullptr;
  if (canvasFontLoad(name) != 0)   return nullptr;   // a library face loads into the slot first
  return &fontCustom;
}
