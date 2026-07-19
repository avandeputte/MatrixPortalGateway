// display.h -- the HUB75 panel: geometry, the flap renderer, and the reel task.
//
// Output goes through panel.h. Nothing in this file knows a pixel format, a bitplane
// or a brightness value -- see panel.h for why that seam is where it is.
//
// Geometry falls out of the panel size and the module grid:
//
//     cellW = panelW / gridCols        cellH = panelH / gridRows
//
// and the renderer then asks font1252Best() for the roomiest CP1252 face that
// fits with a one-pixel gutter. A 15x3 wall on a 128x32 chain gives 8x10 cells and
// the 6x9 face; the same wall on a 128x64 gives 8x21 cells and the 6x13 face.
// Any leftover pixels become an even margin, so the wall is centred. The gutter
// keeps neighbouring glyphs from touching; nothing is drawn in it.
//
// Each cell draws one flap face. A flap is either a glyph or, at reel indices
// [colourBase, colourBase+colourCount), a solid colour swatch. A settled flap is
// just its face -- no seam, no bezel. Mid-flip the cell shows the top half of the *incoming* flap above
// the bottom half of the outgoing one, split by a bright fold -- which is exactly
// what a split-flap does, and why the reel animation reads correctly even in an
// 8x10 cell.
//
// PSRAM is not an option for the framebuffer, even the Waveshare board's fast
// octal part: the LCD_CAM GDMA chain streams from internal SRAM, and putting a
// continuous 5 MHz pixel read on the PSRAM/cache bus WiFi also uses is the
// contention this driver exists to avoid. That bounds bitDepth and panel size
// together. panel.cpp allocates with MALLOC_CAP_DMA, which is internal by
// definition, so this is no longer something a careless malloc() can get wrong.

#ifndef MPGW_DISPLAY_H
#define MPGW_DISPLAY_H

#include "common.h"
#include "font1252.h"

// Resolved once at boot from cfg. Reported by GET /api/status so the UI can show
// what the firmware actually decided.
struct PanelGeometry {
  uint16_t panelW, panelH;
  uint8_t  cols, rows;
  uint8_t  cellW, cellH;
  uint8_t  originX, originY;     // centring margin
  const Font1252* font;
  bool     ready;                // the DMA driver started successfully
};

extern PanelGeometry gPanel;

// Compute the grid for a panel + module grid, clamping anything unusable. Pure --
// no hardware touched -- so config validation can call it before committing.
PanelGeometry dispPlan(uint16_t panelW, uint16_t panelH, uint8_t cols, uint8_t rows);

// Construct and start the panel driver from cfg. Safe to call once, from setup().
// Logs and leaves gPanel.ready false if the panel cannot start; the gateway then
// runs headless rather than refusing to boot.
void dispInit();

// Redraw every cell from the modules' current reel state and push a frame. Returns
// false without painting if the panel is not up or the module lock was busy -- the
// caller should leave its repaint pending and try again rather than drop the frame.
bool dispRender();

// Draw a geometry probe (border, diagonal, chain seam, RGB corners) for four seconds.
// Called from dispInit() when PANEL_BOOT_TEST is 1. Panel coordinates, no wall, no font.
void dispTestPattern();

// Force a redraw on the next display-task iteration (config change, boot).
void dispMarkDirty();

// Drop any running/pending effect and release the raw canvas, then repaint the reel wall. Safe
// from any task. Called when a split-flap command arrives, on canvas release, and by Quiet Time.
void dispReturnToWall();
// True while the panel is showing PIXELS rather than the flap wall -- raw canvas, an
// effect, an on-device animation or a ticker. The display-state JSON reports it as
// "mode":"pixels" so the dashboard preview knows to render the framebuffer readback
// instead of flap cells (v3.0.1).
bool dispPixelsMode();

// Blit font rows [rowFrom,rowTo) of the glyph for CP1252 byte `ch` at (px,py),
// solid colour, bit 15 = leftmost column. The one shared glyph blitter -- the
// ticker (canvas.cpp), the effects (fliporama/clock) and the canvas text op all
// draw through it. Rows clamp to the face height, so (0, 255) means "whole glyph".
void dispDrawGlyph1252(int px, int py, const Font1252* f, uint8_t ch,
                       int rowFrom, int rowTo, uint8_t r, uint8_t g, uint8_t b);

// Show black and halt the panel's GDMA output for the duration of an OTA upload.
// Reversible -- a failed upload calls dispResume().
void dispBlank();

// Restart the panel output and repaint. Pairs with dispBlank().
void dispResume();

// Black, halted, and marked not-ready. Call only once a reboot is certain.
void dispStop();

// Human-readable face name for the status page, e.g. "6x9".
const char* dispFontName();

#endif // MPGW_DISPLAY_H
