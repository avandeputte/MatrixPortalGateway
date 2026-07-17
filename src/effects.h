// effects.h -- on-device panel effects that render at the panel's native frame rate.
//
// These sidestep the raw-canvas API entirely: instead of a client pushing frames over HTTP (which
// tops out around 8 fps, one TCP connection per frame), taskDisplay renders the effect itself, so
// the panel runs at ~70 fps with nothing on the network. An effect owns the panel the way the reel
// wall does -- it is a third display mode alongside the wall and raw canvas (gCanvasMode).
#pragma once
#include <stdint.h>

enum EffectType : uint8_t {
  EFFECT_NONE      = 0,
  EFFECT_PLASMA    = 1,
  EFFECT_FIRE      = 2,
  EFFECT_MATRIX    = 3,
  EFFECT_FLIPORAMA = 4,   // the whole board flips random glyphs -- "the wall is alive"
  EFFECT_CLOCK     = 5,   // split-flap clock: digits flip over on the tick
  EFFECT_LIFE      = 6,   // Conway's Game of Life
};

// The running effect (EFFECT_NONE = off, wall/canvas as normal) and its 1..10 speed. Read by
// taskDisplay every frame; set by POST /api/canvas/effect.
extern volatile uint8_t gEffect;
extern volatile uint8_t gEffectSpeed;

uint8_t     effectByName(const char* name);   // "plasma"/"fire"/"matrix"/"none" -> id (NONE if unknown)
const char* effectName(uint8_t e);            // id -> canonical name
const char* effectListJson();                 // the effect names as a JSON array, e.g. ["plasma",...]

// Prepare per-effect state for a fresh run (seed drops, clear the heat buffer, build LUTs once).
void effectReset(uint8_t type);
// Render and present one frame of `type`. Runs in taskDisplay, like dispRender().
void effectRender(uint8_t type);
