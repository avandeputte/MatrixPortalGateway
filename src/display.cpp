#include "gateway.h"
#include "panel.h"
#include "effects.h"

// display.cpp -- the HUB75 flap wall: geometry, the flap renderer, and the reel task.
// All output goes through panel.h; this file never sees a pixel format, a bitplane or a
// brightness value. See panel.h for which backend is compiled in.

PanelGeometry gPanel = {0};

static volatile bool dispDirty = true;
static uint8_t       lastBright = 0;   // last value pushed to the backend

// The reel's seven colour flaps, in reel order: r o y g b p w.
static const uint8_t COLOUR_RGB[SF_COLOUR_FLAPS][3] = {
  {255,   0,   0},   // r  red
  {255,  96,   0},   // o  orange
  {255, 200,   0},   // y  yellow
  {  0, 210,  60},   // g  green
  {  0,  90, 255},   // b  blue
  {180,   0, 220},   // p  purple
  {255, 255, 255},   // w  white
};
static const uint8_t GLYPH_RGB[3] = {255, 244, 224};   // warm split-flap white

// A colour on its way to the panel. Full intensity: the backend applies panelBright.
struct Ink { uint8_t r, g, b; };

// Fade toward black by pct/255. This is NOT brightness -- it is only used to sit the
// mid-flip fold below the glyph it crosses. Brightness belongs to the backend.
static inline Ink fade(const uint8_t rgb[3], uint8_t pct) {
  return { (uint8_t)((uint32_t)rgb[0] * pct / 255),
           (uint8_t)((uint32_t)rgb[1] * pct / 255),
           (uint8_t)((uint32_t)rgb[2] * pct / 255) };
}

// One flap face, snapshotted out from under vmMutex so the (slow) drawing loop
// never holds the module lock.
// One flap face. `glyph` is an index into the font's row table -- NOT a character byte:
// the pictographs have no byte at all, which is the whole reason they exist here. `ink`
// is the flap's own colour (a heart is red); {0,0,0} means "use the normal text ink".
// `colour` >= 0: the flap IS a colour swatch. `tint` >= 0: it is a glyph drawn in that
// colour -- the pictographs. Both index the SAME palette, which is the point: a heart is
// the red flap's red.
struct FaceSnap { int16_t glyph; int8_t colour; int8_t tint; };
struct CellSnap { FaceSnap cur, next; uint8_t phase; };
static CellSnap snap[VM_MAX_MODULES];

// Snapshot one flap: which glyph it draws, whether it is a colour swatch, and whether it
// brings its own ink (the pictographs do -- a white heart is not a heart).
static inline void snapFace(FaceSnap& f, int flap) {
  f.colour = vmFlapIsColour(flap) ? (int8_t)(flap - VM_COLOUR_BASE) : -1;
  f.glyph  = (int16_t)vmFlapGlyph(flap);
  f.tint   = (int8_t)vmFlapTint(flap);
}

// ---- file-private forward declarations ----
static void drawCell(int col, int row, const CellSnap& c);
static void drawFace(int cx, int cy, const FaceSnap& f, Ink ink, int rowFrom, int rowTo);

PanelGeometry dispPlan(uint16_t panelW, uint16_t panelH, uint8_t cols, uint8_t rows) {
  PanelGeometry g = {0};
  if (panelW < 32 || panelW > PANEL_MAX_W) panelW = DEFAULT_PANEL_W;
  // Only 1/8, 1/16 and 1/32 scan heights exist on HUB75, and the address-line
  // count is log2(height) - 1.
  if (panelH != 16 && panelH != 32 && panelH != 64) panelH = DEFAULT_PANEL_H;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  // Every cell must hold the SMALLEST bundled face plus a one-pixel gutter, so that
  // neighbouring glyphs never touch -- 5x8, so 6x9 px. Shrink the wall until it
  // does, rather than render glyphs that spill into their neighbours. A 15-column
  // wall therefore does not fit a 64px panel (64/15 = 4 px per cell) and is quietly
  // reduced to 10 columns; the boot log says so, and setup() sizes the module count
  // from the RESULT of this function, never from the raw config.
  const uint8_t minW = FONT1252_ALL[FONT1252_COUNT - 1]->width  + 1;
  const uint8_t minH = FONT1252_ALL[FONT1252_COUNT - 1]->height + 1;
  while (cols > 1 && panelW / cols < minW) cols--;
  while (rows > 1 && panelH / rows < minH) rows--;
  // ...and never build a wall bigger than the module ceiling.
  while ((int)cols * rows > VM_MAX_MODULES) { if (cols > rows) cols--; else rows--; }

  g.panelW  = panelW;  g.panelH = panelH;
  g.cols    = cols;    g.rows   = rows;
  g.cellW   = (uint8_t)(panelW / cols);
  g.cellH   = (uint8_t)(panelH / rows);
  g.originX = (uint8_t)((panelW - g.cellW * cols) / 2);
  g.originY = (uint8_t)((panelH - g.cellH * rows) / 2);
  g.font    = font1252Best(g.cellW, g.cellH);
  g.ready   = false;
  return g;
}

void dispInit() {
  // setup() has already planned the geometry (vmInit needs the module count that
  // falls out of it). Re-plan only if something skipped that step.
  if (!gPanel.cols) gPanel = dispPlan(cfg.panelW, cfg.panelH, cfg.gridCols, cfg.gridRows);

#if PANEL_DISABLE
  /* Diagnostic A/B: bring the whole gateway up with the panel never started -- no
     framebuffer, no GDMA, no LCD_CAM clock, no panel current draw. Everything else
     (WiFi, web, MQTT, the emulated bus, the 45 virtual modules) runs exactly as
     normal; the wall is simply not driven.

     This exists to answer one question: is the radio's misbehaviour caused by the
     panel? The panel's GDMA streams ~20 MB/s continuously out of the same internal
     SRAM the WiFi stack allocates from, and its current draw sits on top of the
     radio's TX spikes. Both can produce association failures (4WAY_HANDSHAKE_TIMEOUT)
     and lost frames at a signal strength that should be flawless.

     If WiFi is solid with this set and flaky without it, the fault is the panel --
     power first (give it its own 5 V supply, don't feed it from the board's USB),
     then bandwidth (lower LCD_CLK_HZ in panel.cpp, or cfg.panelBitDepth). If it is
     flaky BOTH ways, the panel is exonerated and the problem is the AP or the RF
     environment. Build with -DPANEL_DISABLE=1. */
  printf("[PANEL] PANEL_DISABLE=1 -- panel not started (WiFi/RF A/B test build)\n");
  gPanel.ready = false;
  return;
#endif

  uint8_t depth = cfg.panelBitDepth;
  if (depth < 1 || depth > 6) depth = DEFAULT_BIT_DEPTH;

  gPanel.ready = panelBegin(gPanel.panelW, gPanel.panelH, depth);
  panelSetColourOrder(cfg.panelBGR);   // the panel's own wiring, not something we can detect
  if (!gPanel.ready) {
    // Headless is a legitimate state: the web UI, MQTT and the whole emulated bus still
    // work, so report the fault and carry on rather than refusing to boot.
    printf("[PANEL] no output -- running headless\n");
    return;
  }

  const PanelInfo& p = panelInfo();
  lastBright = cfg.panelBright ? cfg.panelBright : DEFAULT_BRIGHTNESS;
  panelSetBrightness(lastBright);
  panelClear();
  panelShow();

  printf("[PANEL] %ux%u, %ux%u wall, %ux%u cells, %s font, %u bitplanes\n",
         gPanel.panelW, gPanel.panelH, gPanel.cols, gPanel.rows,
         gPanel.cellW, gPanel.cellH, dispFontName(), p.depth);
  printf("[PANEL] %u KB internal, refresh ~%u Hz, heap now %u\n",
         (unsigned)(p.bytes / 1024), (unsigned)p.refreshHz, (unsigned)ESP.getFreeHeap());

#if PANEL_BOOT_TEST
  dispTestPattern();
#endif
}

// Hand the panel back to the reel wall: cancel any pending/running effect and release the raw
// canvas, then mark the wall dirty so it repaints. The effect is only cleared, never reset here,
// so this is safe to call from any task (the render task owns effectReset). Called when a display
// command arrives (so content the user sends is actually shown), on canvas release, and by Quiet
// Time. A no-op when the wall already owns the panel.
void dispReturnToWall() {
  if (gEffect == EFFECT_NONE && gEffectReq == EFFECT_REQ_IDLE && !gCanvasMode) return;
  gEffectReq  = EFFECT_REQ_IDLE;   // cancel a pending start
  gEffect     = EFFECT_NONE;       // stop rendering the current effect (taskDisplay just stops)
  gCanvasMode = false;             // release the raw canvas
  dispMarkDirty();                 // repaint the wall
}

// Show black and halt output. Used while a firmware image streams in: writing flash
// disables the instruction cache on both cores. Reversible -- see dispResume().
void dispBlank() { if (gPanel.ready) panelStop(); }

// Restart output and repaint. Called when an upload fails.
void dispResume() { if (gPanel.ready) { panelResume(); dispMarkDirty(); } }

// Terminal: black, halted, marked not-ready. Only once a reboot is certain.
void dispStop() { dispBlank(); gPanel.ready = false; }

// A geometry probe, drawn straight to the panel with nothing else on it. Everything
// here is in PANEL coordinates, not wall coordinates, so it is independent of the module
// grid, the font and the reels. See PANEL_BOOT_TEST in common.h for how to read it.
void dispTestPattern() {
  if (!gPanel.ready) return;
  const int W = gPanel.panelW, H = gPanel.panelH;

  panelClear();
  // 1px border: any scan/address error breaks or duplicates it.
  panelHLine(0, 0,     W, 255, 255, 255);
  panelHLine(0, H - 1, W, 255, 255, 255);
  panelVLine(0,     0, H, 255, 255, 255);
  panelVLine(W - 1, 0, H, 255, 255, 255);
  // Seam marker at the chain boundary: a gap or a jog here is the ribbon, not us.
  panelVLine(W / 2, 0, H, 40, 40, 40);
  // Diagonal: stair-steps or doubles if the row mapping is wrong.
  for (int x = 0; x < W; x++) panelPixel(x, (int)((long)x * (H - 1) / (W - 1)), 40, 40, 40);
  // Corners, clockwise from top-left: R G B W. Wrong colours = wrong RGB pin order.
  panelFillRect(1,     1,     3, 3, 255, 0, 0);
  panelFillRect(W - 4, 1,     3, 3, 0, 255, 0);
  panelFillRect(W - 4, H - 4, 3, 3, 0, 0, 255);
  panelFillRect(1,     H - 4, 3, 3, 255, 255, 255);
  panelShow();
  printf("[PANEL] boot test pattern up for 4s (PANEL_BOOT_TEST=1 in common.h)\n");
  delay(4000);
  panelClear();
  panelShow();
}

void dispMarkDirty() { dispDirty = true; }

const char* dispFontName() {
  static char name[8] = "-";
  if (gPanel.font) snprintf(name, sizeof(name), "%ux%u", gPanel.font->width, gPanel.font->height);
  return name;
}

// Draw the part of one flap face lying in local rows [rowFrom, rowTo). A colour
// flap is a solid swatch inset by the seam gutter; a character flap is its glyph,
// clipped to the same row band.
static void drawFace(int cx, int cy, const FaceSnap& f, Ink ink, int rowFrom, int rowTo) {
  if (rowFrom >= rowTo) return;
  const Font1252& fn = *gPanel.font;
  // dispPlan guarantees the face fits, but centring maths on an undersized cell would
  // otherwise produce a negative offset and bleed into the neighbouring module. Clamp
  // instead, and clip the columns.
  int padX = (gPanel.cellW - fn.width)  / 2; if (padX < 0) padX = 0;
  int padY = (gPanel.cellH - fn.height) / 2; if (padY < 0) padY = 0;
  if (f.colour >= 0) {
    // A colour flap fills the exact footprint a glyph would -- same box, so colours and
    // characters line up cell for cell and the seam gutter is left clear on every side
    // (the old full-height fill painted over the bottom seam). Clip to the [rowFrom,rowTo)
    // band so the flip animation still splits it at the fold.
    int a = padY > rowFrom ? padY : rowFrom;
    int b = (padY + fn.height) < rowTo ? (padY + fn.height) : rowTo;
    if (a < b) panelFillRect(cx + padX, cy + a, fn.width, b - a, ink.r, ink.g, ink.b);
    return;
  }
  // Draw by GLYPH INDEX. The flap already knows which glyph it carries -- reelGlyph()
  // resolved that, and it is the only thing that knows a pictograph's bitmap lives past
  // the CP1252 block rather than being reachable through FONT1252_INDEX.
  if (f.glyph < 0) return;                      // no glyph: a blank flap
  uint8_t gi = (uint8_t)f.glyph;
  int gx = cx + padX, gy = cy + padY;
  int wide = (fn.width < gPanel.cellW) ? fn.width : gPanel.cellW;
  for (int r = 0; r < fn.height; r++) {
    int local = padY + r;
    if (local < rowFrom || local >= rowTo) continue;
    uint16_t bits = font1252Row(fn, gi, (uint8_t)r);
    if (!bits) continue;
    for (int c = 0; c < wide; c++)
      if (bits & (0x8000 >> c)) panelPixel(gx + c, gy + r, ink.r, ink.g, ink.b);
  }
}

// The faint seam around every module -- the emulated wall's split-flap gaps. Drawn BEFORE
// the faces (see dispRender) so a glyph or a colour swatch always overwrites any pixel it
// shares with a seam: the grid only ever fills the gutters between cells. Its faintness is
// cfg.gridColor scaled by cfg.gridBright, deliberately NOT panelBright -- the backend's
// global brightness dims the seam and the glyphs by the same factor, so the seam has to be
// dim on its own to read as a hairline. gridBright 0 draws nothing.
static void drawGrid() {
  if (!cfg.gridBright) return;
  const uint8_t rgb[3] = { (uint8_t)(cfg.gridColor >> 16),
                           (uint8_t)(cfg.gridColor >> 8),
                           (uint8_t)(cfg.gridColor) };
  Ink k = fade(rgb, cfg.gridBright);
  const int x0 = gPanel.originX, y0 = gPanel.originY;
  const int wallW = gPanel.cols * gPanel.cellW;   // cells tile exactly; margins are outside
  const int wallH = gPanel.rows * gPanel.cellH;
  // Verticals: 2px straddling every cell boundary, so each glyph sits centred between a
  // pair of them. The cell has a 1px gutter on each side (the glyph is centred with an
  // even horizontal margin), and a boundary's two columns are the right gutter of the cell
  // to its left and the left gutter of the cell to its right -- both glyph-free. The pair
  // is what makes a character look centred; a single 1px line leaves it hugging one side.
  // (Horizontals stay 1px below: the cell is only a pixel taller than the font, so there
  // is just one gutter row to put them in.) Clamp keeps the outer pair on the panel.
  for (int c = 0; c <= gPanel.cols; c++) {
    int xb = x0 + c * gPanel.cellW;
    for (int d = -1; d <= 0; d++) {
      int x = xb + d;
      if (x < 0) x = 0; else if (x >= gPanel.panelW) x = gPanel.panelW - 1;
      panelVLine(x, y0, wallH, k.r, k.g, k.b);
    }
  }
  // Horizontals: between-row seams sit on each cell's BOTTOM row -- the glyph is top-aligned
  // when the cell is only a pixel taller than the font (padY rounds down), so its one spare
  // row is at the bottom and the seam clears the glyph. The bottom frame is the last of
  // those. The top frame has no such gutter above the first row, so put it one row up into
  // the panel margin when there is one; that also makes the vertical rhythm regular (a line
  // above and below every glyph). With no margin it falls back onto y0, hidden behind the
  // glyphs it grazes.
  panelHLine(x0, y0 > 0 ? y0 - 1 : y0, wallW, k.r, k.g, k.b);
  for (int r = 1; r <= gPanel.rows; r++)
    panelHLine(x0, y0 + r * gPanel.cellH - 1, wallW, k.r, k.g, k.b);
}

// A colour flap is its swatch; a pictograph is its own ink; everything else is the warm
// split-flap white the letters are printed in.
static inline Ink faceInk(const FaceSnap& f) {
  if (f.colour >= 0) return fade(COLOUR_RGB[f.colour], 255);   // the flap IS a swatch
  if (f.tint   >= 0) return fade(COLOUR_RGB[f.tint],   255);   // a tinted glyph: the heart
  return fade(GLYPH_RGB, 255);                                 // a letter: warm white
}

static void drawCell(int col, int row, const CellSnap& c) {
  int cx = gPanel.originX + col * gPanel.cellW;
  int cy = gPanel.originY + row * gPanel.cellH;

  Ink cur = faceInk(c.cur);

  // Settled: nothing but the face. A real flap shows no fold once it has landed.
  if (c.phase == 0) {
    drawFace(cx, cy, c.cur, cur, 0, gPanel.cellH);
    return;
  }

  // Mid-flip: the incoming flap's top half is already exposed above the fold, the
  // outgoing flap's bottom half is still below it. The fold sits on the glyph's own
  // mid-line, not the cell's, so it reads as the crease across the character however
  // much vertical margin the cell has.
  const Font1252& fn = *gPanel.font;
  int padX = (gPanel.cellW - fn.width)  / 2; if (padX < 0) padX = 0;
  int padY = (gPanel.cellH - fn.height) / 2; if (padY < 0) padY = 0;
  int seam = padY + fn.height / 2;
  if (seam < 1) seam = 1;
  if (seam > gPanel.cellH - 2) seam = gPanel.cellH - 2;

  Ink nxt  = faceInk(c.next);
  Ink fold = fade(GLYPH_RGB, 150);
  drawFace(cx, cy, c.next, nxt, 0, seam);
  drawFace(cx, cy, c.cur,  cur, seam + 1, gPanel.cellH);
  // The crease spans the face's own footprint, so it lines up with the glyph box and the
  // colour swatch rather than the full cell.
  panelHLine(cx + padX, cy + seam, fn.width, fold.r, fold.g, fold.b);
}

bool dispRender() {
  if (!gPanel.ready || !vmods) return false;

  // Snapshot under the lock; draw outside it. Drawing touches thousands of
  // pixels and must not stall vmDispatch on the bus path.
  if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  // Clear the flag BEFORE reading state, not after drawing: a dispMarkDirty() that
  // lands mid-repaint (a brightness change from the web task) must survive to force
  // one more frame, or an idle wall would never pick it up. Clearing it only once
  // the lock is held means a timed-out repaint above leaves the flag set and the
  // next tick retries, rather than dropping the frame on the floor.
  dispDirty = false;
  int n = vmCount;
  if (n > VM_MAX_MODULES) n = VM_MAX_MODULES;
  for (int i = 0; i < n; i++) {
    const VModule& m = vmods[i];
    int cur  = (m.curIndex >= 0 && m.curIndex < SF_MAX_FLAPS) ? m.curIndex : 0;
    int next = (cur + 1) % SF_MAX_FLAPS;
    snap[i].phase = m.flipPhase;
    snapFace(snap[i].cur,  cur);
    snapFace(snap[i].next, next);
  }
  if (vmMutex) xSemaphoreGive(vmMutex);

  uint8_t want = cfg.panelBright ? cfg.panelBright : DEFAULT_BRIGHTNESS;
  if (want != lastBright) { panelSetBrightness(want); lastBright = want; }

  panelClear();
  drawGrid();                           // behind the faces: it only fills the gutters
  for (int i = 0; i < n; i++) {
    int col = i % gPanel.cols;
    int row = i / gPanel.cols;
    if (row >= gPanel.rows) break;      // more modules than cells: nothing to draw
    drawCell(col, row, snap[i]);
  }
  panelShow();
  return true;
}

// The reel task. vmTick advances every reel by at most one half-flap and tells us
// whether anything moved; we only repaint when it did. An idle wall costs one
// vmTick per tick and no show().
//
// The tick is the reel TIMER's resolution, not the frame rate: vmTick is a handful of
// comparisons, while a repaint redraws every cell and then calls show(), which
// re-encodes the whole canvas into bitplanes and busy-waits for the refresh ISR to swap
// buffers. Those differ by orders of magnitude, so the repaint gets its own ceiling. At
// DEFAULT_FLAP_MS a turning reel half-steps every 30 ms (~33 Hz) and never reaches it;
// the ceiling is there so a reel set to the minimum flapMs coalesces frames instead of
// pinning the panel at 100 Hz.
static const uint32_t DISP_MIN_FRAME_MS = 20;   // 50 Hz ceiling on show()

void taskDisplay(void* pv) {
  const TickType_t period = pdMS_TO_TICKS(10);   // reel-timer resolution
  uint32_t lastFrameMs = 0;
  bool     pending     = true;                   // paint once at boot
  while (true) {
    uint32_t now = millis();
    wdgDispMs = now;
    // A firmware image is streaming in. Don't step reels, don't repaint -- just keep
    // the watchdog fed. Standing down voluntarily (rather than being suspended) means
    // we can never be frozen holding vmMutex.
    if (gOtaInProgress) { gDispParked = true; vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    // Raw-canvas mode: HTTP handlers own the panel. Stand down (like OTA) so we never write
    // the back buffer from under them, and never repaint the wall over their canvas. Raising
    // gDispParked AFTER the check -- reached only once any in-flight repaint above has returned
    // -- is the acknowledgement the take-over waits for before it draws its first frame.
    if (gCanvasMode) { gDispParked = true; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    gDispParked = false;                  // rendering again: the panel is ours until we next park
    // Pick up a pending effect start HERE, on the render task: effectReset() (which frees/reallocs
    // the PSRAM grid and rewrites the cell arrays) then never runs under an in-flight effectRender()
    // on the web task's core -- the source of the old cellN=0 divide-by-zero / use-after-free race.
    if (gEffectReq != EFFECT_REQ_IDLE) {
      uint8_t req = gEffectReq; gEffectReq = EFFECT_REQ_IDLE;
      effectReset(req);
      gEffect = req;
    }
    // On-device effect owns the panel: render a frame at the panel's native rate (panelShow
    // inside effectRender paces us to one refresh), instead of the reel wall.
    if (gEffect != EFFECT_NONE) {
      effectRender(gEffect);
      wdgDispMs = millis();
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    if (vmTick(now)) pending = true;
    // dispRender clears dispDirty itself, but only once it is committed to
    // painting -- so a repaint it declined still leaves work for the next tick.
    if ((pending || dispDirty) && (uint32_t)(now - lastFrameMs) >= DISP_MIN_FRAME_MS) {
      if (dispRender()) { pending = false; lastFrameMs = now; }
    }
    wdgDispMs = millis();
    vTaskDelay(period);
  }
}
