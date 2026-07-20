// canvas.h -- on-device canvas modes that taskDisplay renders autonomously, so a client uploads
// once and disconnects instead of streaming every frame over HTTP.
//
//   * Animation  -- a short loop of full-panel frames held in PSRAM, played back at a set fps.
//   * Ticker     -- one line of text scrolling right-to-left.
//
// Both are third-class display modes alongside effects and the raw canvas: taskDisplay owns the
// panel while they run, and dispReturnToWall() (a split-flap command, canvas release, Quiet Time)
// stops them. The 2 MB PSRAM the framebuffer can't use is exactly where the animation frames live.
#ifndef MPGW_CANVAS_H
#define MPGW_CANVAS_H

#include <stdint.h>
#include <stddef.h>

struct Font1252;   // font1252.h -- only pointers cross this header

// ---- animation loop ----------------------------------------------------------------------------
// Playback is live while true; read by taskDisplay every frame.
extern volatile bool gAnimActive;

// Upload is streamed, not buffered whole: Begin() sizes and reserves the PSRAM store from the
// parsed header, Feed() appends raw frame bytes, Commit() starts playback. Begin/Commit return 0 on
// success or an HTTP status (400 bad, 413 too big, 503 no RAM). The web task calls these while the
// render task is parked (gCanvasMode), so reserving/writing the buffer races nothing.
int  canvasAnimBegin(uint8_t fmt, uint8_t fps, bool loop, uint16_t w, uint16_t h, uint16_t frames);
void canvasAnimFeed(const uint8_t* data, size_t n);
int  canvasAnimCommit();
void canvasAnimRender();     // taskDisplay: present the current frame, advance at the set fps
void canvasAnimStop();       // stop playback; the PSRAM store is kept for the next upload
uint16_t canvasAnimCount();  // frames currently loaded (for the upload reply)

// ---- animation library (v2.1) ------------------------------------------------------------------
// Named animations persisted to FATFS (/anim/<name>.mpg, the raw MPGA container) with the
// PSRAM store as the playback cache. All return 0 or an HTTP status code.
#define ANIM_NAME_MAX 24
int  canvasAnimSave(const char* name);           // write the CURRENT store to /anim/<name>.mpg
int  canvasAnimLoadPlay(const char* name);       // load a named file into the store and play it
int  canvasAnimDelete(const char* name);
void canvasAnimList(void (*sink)(const char*));  // stream a JSON array of {name,bytes,frames,...}
bool canvasAnimNameOk(const char* name);         // 1..24 chars of [a-z0-9_-]

// ---- sprite atlas (v2.1) -----------------------------------------------------------------------
// One tile sheet in PSRAM (PUT /api/canvas/atlas), blitted by the ops path's "sprite" op. Kept
// across uses; replaced by the next upload. Same streamed Begin/Feed/Commit shape as the
// animation store; Begin/Commit return 0 or an HTTP status (400 bad, 413 too big, 503 no RAM).
// ---- named atlas library (v3.1) ----
// Up to ATLAS_MAX_SHEETS resident named sheets under one shared PSRAM budget, LRU-evicted,
// optionally persisted to /atlas/<name>.mpta and lazy-loaded on bind. Uploads build into a
// NEW allocation and publish at Commit (double-buffer), so a bound sheet is never blitted
// half-written and there is no "atlasReady=false" blind window.
#define ATLAS_MAX_SHEETS       16
#define ATLAS_NAME_MAX         32
#define ATLAS_MAX_SHEET_BYTES  (2048u * 1024u)
#define ATLAS_TOTAL_BUDGET     (4096u * 1024u)
bool canvasAtlasNameOk(const char* name);                  // ^[a-z0-9._-]{1,32}$
int  canvasAtlasBegin(const char* name, uint8_t fmt, uint16_t tileW, uint16_t tileH, uint16_t tiles);
void canvasAtlasFeed(const uint8_t* data, size_t n);
int  canvasAtlasCommit();                                  // publish: swap in, evict LRU over budget
// Blit tile i of the bound sheet at (x,y) via panelPixel, skipping the transparent colour
// (rgb565 0xF81F / rgb888 magenta). False when the handle is stale or i is out of range.
int  canvasAtlasBind(const char* name);                    // handle, or -1 (lazy FS load inside)
int  canvasAtlasBoundHandle();                             // the sticky bind, -1 = none
bool canvasAtlasBlitFrom(int handle, uint16_t i, int x, int y);
int  canvasAtlasSave(const char* name);                    // 0 / 404 / 507 / 503
int  canvasAtlasDelete(const char* name);                  // 0 / 404
void canvasAtlasListJson(void (*sink)(const char*));       // [{name,tiles,w,h,fmt,bytes,resident,persisted},…]
const uint8_t* canvasAtlasData(const char* name, uint8_t hdr[12], size_t* bytes);   // resident sheet bytes + its MPTA header
void canvasAtlasStateJson(char* out, size_t cap);          // {"bound":…,"loaded":[…]} for GET /api/canvas

// ---- GIF import (v2.1) -------------------------------------------------------------------------
// Decode a whole GIF (already buffered in PSRAM) into the animation store above, composited to
// full-panel rgb565 frames, and start playback. Returns 0 or an HTTP status; *errMsg names the
// failure for the reply. The caller frees the GIF buffer; frames/fps report what was imported.
int canvasGifImport(uint8_t* data, size_t len, uint16_t* frames, uint8_t* fps, const char** errMsg);

// ---- uploadable fonts (v2.1) -------------------------------------------------------------------
// One custom Font1252 face in PSRAM (PUT /api/canvas/font, name "custom"), persistable to FATFS
// as /fonts/<name>.fnt -- same naming rules and rc conventions as the animation library. The
// blob is "MPFT"(4) ver(1)=1 width(1) height(1) ascent(1) then 216 glyphs of `height` big-endian
// uint16 rows in FONT1252_INDEX order (tools/fontpack.py packs one from a BDF).
#define CANVAS_FONT_MAX_BYTES (64u * 1024u)
int  canvasFontInstall(const uint8_t* blob, size_t len);   // parse a blob into the PSRAM slot
bool canvasFontInfo(uint8_t& w, uint8_t& h, uint8_t& ascent);   // false when none loaded
int  canvasFontSave(const char* name);           // write the CURRENT slot to /fonts/<name>.fnt
int  canvasFontDelete(const char* name);
void canvasFontList(void (*sink)(const char*));  // stream a JSON array of {name,bytes,w,h,ascent}
// Resolve a "font" field: "custom" is the slot as-is, any other name loads /fonts/<name>.fnt
// into the slot first. NULL when unavailable -- the caller falls back to a built-in face.
const Font1252* canvasFontByName(const char* name);

// ---- frame transitions (v2.1) ------------------------------------------------------------------
// Configured once (POST /api/canvas/transition), applied by the full-frame PUT path.
extern volatile uint8_t  gTransType;   // 0 none, 1 crossfade, 2 wipe, 3 slide
extern volatile uint16_t gTransMs;
bool canvasStageBegin(uint8_t bpp);            // alloc staging + capture the outgoing frame
void canvasStageFeed(const uint8_t* d, size_t n);
void canvasStagePresent();                     // tween old -> staged, then land on it

// ---- scrolling ticker --------------------------------------------------------------------------
extern volatile bool gTickerActive;

// Replace the ticker text/colour/speed and start it. speed is 1..20 px per step (~30 steps/s).
// overlay=true (v2.1) COMPOSITES the ticker over whatever else is presenting -- the wall,
// an effect, an animation, the raw canvas -- as a lower-third band, instead of owning the
// panel. An overlay ticker survives wall page changes; only an explicit stop ({"text":""}),
// Quiet Time, or a non-overlay ticker replaces it. font (v2.1) overrides the panel-sized
// built-in face -- pass canvasFontByName()'s result, or NULL for the default.
void canvasTickerSet(const char* text, uint8_t r, uint8_t g, uint8_t b, int speed,
                     bool overlay = false, bool band = true, const Font1252* font = nullptr);
void canvasTickerRender();     // taskDisplay: exclusive mode -- draw + advance one step
void canvasTickerTick(uint32_t now);  // taskDisplay: overlay mode -- advance scroll, repaint idle wall
void canvasTickerStop();              // stops an exclusive ticker; overlay survives (see force)
void canvasTickerStopForce();         // stops any ticker, overlay included (Quiet Time, explicit)
extern volatile bool gTickerOverlay;

#endif // MPGW_CANVAS_H
