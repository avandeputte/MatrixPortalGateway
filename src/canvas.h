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

// ---- scrolling ticker --------------------------------------------------------------------------
extern volatile bool gTickerActive;

// Replace the ticker text/colour/speed and start it. speed is 1..20 px per step (~30 steps/s).
void canvasTickerSet(const char* text, uint8_t r, uint8_t g, uint8_t b, int speed);
void canvasTickerRender();   // taskDisplay: draw + advance one step
void canvasTickerStop();

#endif // MPGW_CANVAS_H
