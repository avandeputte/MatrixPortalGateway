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
  uint32_t bytes;        // framebuffers + descriptors, all internal DMA-capable RAM
  uint32_t refreshHz;    // computed; nothing can steal these clocks
};

// Bring the panel up. Returns false and leaves panelInfo().ok clear on failure --
// the gateway then runs headless, which is a legitimate state.
bool panelBegin(uint16_t width, uint16_t height, uint8_t depth);
const PanelInfo& panelInfo();

// 0..255, applied as the OE duty cycle. Call it whenever cfg.panelBright moves.
void panelSetBrightness(uint8_t b);

// ---- drawing: back buffer, 8-bit RGB, no brightness applied by the caller ----
void panelClear();
void panelPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void panelHLine(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b);
void panelVLine(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b);
void panelFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

// Present the back buffer: re-point the live descriptor chain's tail at the other chain,
// then wait one frame so the caller cannot draw into a buffer GDMA is still reading.
void panelShow();

// Halt output. Used before writing flash: the instruction cache goes away on both cores
// and a half-driven HUB75 latches garbage. panelResume() undoes it.
void panelStop();
void panelResume();

#endif // MPGW_PANEL_H
