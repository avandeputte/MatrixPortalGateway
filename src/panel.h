// panel.h -- the HUB75 output layer.
//
// One implementation: panel.cpp, a direct ESP32-S3 LCD_CAM + GDMA driver. All thirteen
// HUB75 signals live in the 16-bit data word and GDMA walks a circular descriptor chain,
// so nothing in the refresh path runs on the CPU -- no ISR for WiFi or a flash write to
// delay -- the usual cause of HUB75 shimmer.
//
// The API takes 8-bit RGB and hides everything else. In particular the CALLER never
// applies brightness: panelSetBrightness() is the OE duty cycle inside the driver, so
// dimming the wall costs no colour levels.

#ifndef MPGW_PANEL_H
#define MPGW_PANEL_H

#include <stdint.h>
#include "common.h"

struct PanelInfo {
  bool     ok;           // output is running
  uint16_t width, height;
  uint8_t  depth;        // bitplanes actually in use
  uint32_t bytes;        // framebuffers + descriptors (fbPsram: framebuffers are in PSRAM,
                         // descriptors stay internal -- see panelFbInPsram())
  uint32_t refreshHz;    // computed; nothing can steal these clocks
};

// Bring the panel up. Returns false and leaves panelInfo().ok clear on failure --
// the gateway then runs headless, which is a legitimate state.
bool panelBegin(uint16_t width, uint16_t height, uint8_t depth, bool fbPsram = false);
const PanelInfo& panelInfo();
bool panelFbInPsram();   // v3.11: true when the framebuffer is running from PSRAM

// 0..255, applied as the OE duty cycle. Call it whenever cfg.panelBright moves.
void panelSetBrightness(uint8_t b);

// Some HUB75 panels are wired BGR, not RGB. Set true to swap red and blue on the way out.
// Takes effect on the next frame; nothing is cached.
void panelSetColourOrder(bool bgr);

// ---- drawing: back buffer, 8-bit RGB, no brightness applied by the caller ----
void panelClear();
void panelPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void panelHLine(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b);
void panelVLine(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b);
void panelFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
// Row blitters (v3.1): one horizontal run of n pixels at (x,y) -- ~4-6x the per-pixel
// path for frame-shaped draws. 565 is big-endian, as every wire format here is.
void panelBlitRow888(int x, int y, int n, const uint8_t* rgb);
void panelBlitRow565(int x, int y, int n, const uint8_t* be565);
void panelLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b);   // Bresenham
void panelCircle(int cx, int cy, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b);  // outline/disc
void panelTriangle(int x0, int y0, int x1, int y1, int x2, int y2, bool fill, uint8_t r, uint8_t g, uint8_t b);
void panelRoundRect(int x, int y, int w, int h, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b);
void panelEllipse(int cx, int cy, int a, int b, bool fill, uint8_t r, uint8_t g, uint8_t bc);  // semi-axes a,b
// Shift the live frame into the back buffer by (dx,dy); vacated pixels get the fill colour. For a
// marquee: scroll, draw the newly-revealed edge, then panelShow(). Does not drift on repeat.
void panelScroll(int dx, int dy, uint8_t fr, uint8_t fg, uint8_t fb);
// Clip rectangle (v3.5, the ops "clip" op): x0/y0 inclusive, x1/y1 exclusive. Applies
// to panelPixel and the fill/blit fast paths -- i.e. every drawing primitive -- until
// cleared. canvasOpsRun clears it at batch start and end, so no other path ever
// inherits a stale clip. panelClear() intentionally ignores it.
void panelSetClip(int x0, int y0, int x1, int y1);
void panelClearClip();
// Compositing (v3.8): set a blend mode (0 over, 1 add, 2 multiply, 3 screen, 4 max) and
// alpha (0..255); panelPixel then composites over the back buffer until cleared. AA
// drawing uses this with coverage as alpha. The ops layer resets it per op.
void panelSetBlend(uint8_t mode, uint8_t alpha);
void panelClearBlend();
// Offscreen layers (v3.9): panelLayerBegin() redirects every drawing primitive into a
// full-panel RGBA shadow; panelLayerComposite() blends that group back at (ox,oy) with one
// group blend mode + alpha (group opacity). panelLayerDiscard() drops it undrawn. The ops
// layer discards any open layer at batch end so it can't leak into another draw path.
bool panelLayerBegin();
void panelLayerComposite(int ox, int oy, uint8_t mode, uint8_t galpha);
void panelLayerDiscard();
// Output-stage stall watchdog (v3.5): taskDisplay calls this ~1/s; a frozen GDMA
// descriptor pointer (the silent black-panel wedge) is detected and the output
// stage restarted in place. No-op while parked (panelStop) or not ok.
void panelHealthTick();

// Present the back buffer: re-point the live descriptor chain's tail at the other chain,
// then wait one frame so the caller cannot draw into a buffer GDMA is still reading.
void panelShow();

// Sync the back buffer to what is currently on screen, so a partial update (a rectangle, one
// changed region) can be drawn on top of it instead of on a stale frame. Call before drawing a
// partial region, then panelShow(). No-op if the panel is down.
void panelCloneToBack();

// Read the live frame (whatever is on screen, any mode) back into `out` as raw pixels, row-major,
// top-left: W*H*3 rgb888, or W*H*2 big-endian rgb565 if rgb565. Colours are quantised to the panel
// bit depth (a true screenshot); brightness is not reflected. Read-only. out must hold W*H*(2 or 3).
void panelReadback(uint8_t* out, bool rgb565);

// Overlay hook (v2.1): called at the top of panelShow(), drawing into the frame
// that is about to be presented. Every presenter -- wall, effect, animation,
// raw canvas -- gets the overlay without knowing it exists. The hook must only
// use panelPixel/panelFillRect etc. and must NOT call panelShow. NULL disables.
void panelSetOverlay(void (*fn)(void));

// Halt output (dark panel). Used during OTA flash writes -- not because the GDMA
// refresh needs the CPU (it does not), but for the panel-current and memory-bandwidth headroom
// while the upload runs. panelResume() undoes it.
void panelStop();
void panelResume();

// Halt output AND free the framebuffers/descriptors (v2.2.2). Hands the panel's
// internal DMA RAM (38-102 KB depending on geometry) back to the heap for the
// duration of a web OTA -- on a 256x64 board the upload otherwise starts with
// ~40 KB free and the TCP window exhausts it. Marks the panel not-ready; undo
// with panelBegin() (a successful OTA reboots instead and re-inits normally).
void panelRelease();

#endif // MPGW_PANEL_H
