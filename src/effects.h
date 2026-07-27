// effects.h -- on-device panel effects that render at the panel's native frame rate.
//
// These sidestep the raw-canvas API entirely: instead of a client pushing frames over HTTP (which
// tops out around 8 fps, one TCP connection per frame), taskDisplay renders the effect itself, so
// the panel runs at ~70 fps with nothing on the network. An effect owns the panel the way the reel
// wall does -- one more display mode alongside the wall, the raw canvas, and the animation/ticker players (gCanvasMode).
#pragma once
#include <stdint.h>

// One row per effect: X(EnumSuffix, "wire-name"). The enum, effectByName(), effectName() and
// effectListJson() are all generated from this table, so adding an effect is a single edit here
// (plus its render/reset code and the openapi enum) instead of four hand-synced lists.
#define EFFECT_TABLE(X) \
  X(PLASMA,    "plasma")    \
  X(FIRE,      "fire")      \
  X(MATRIX,    "matrix")    \
  X(FLIPORAMA, "fliporama") \
  X(CLOCK,     "clock")     \
  X(LIFE,      "life")      \
  X(SPECTRUM,  "spectrum")  \
  X(SOUNDWALL, "soundwall") \
  X(MAZE,      "maze")      \
  X(RIPPLE,    "ripple")

enum EffectType : uint8_t {
  EFFECT_NONE = 0,
#define EFFECT_ENUM(sym, name) EFFECT_##sym,
  EFFECT_TABLE(EFFECT_ENUM)
#undef EFFECT_ENUM
};

// The running effect (EFFECT_NONE = off) and its 1..10 speed, read by taskDisplay every frame.
extern volatile uint8_t gEffect;
extern volatile uint8_t gEffectSpeed;
// A pending START request. Any task sets it to an EFFECT_* id; taskDisplay picks it up, runs
// effectReset() and starts rendering -- so effect state is only ever mutated on the render task,
// never from another core under an in-flight effectRender(). 0xFF = no request pending.
extern volatile uint8_t gEffectReq;
static const uint8_t EFFECT_REQ_IDLE = 0xFF;

// Optional per-start overrides from /api/canvas/effect. -1 means "use the effect's own default", so
// an unparameterised start looks exactly as before. hue is 0..255 around the colour wheel (matrix
// rain, plasma tint, Life cells); density is 1..100 (Life seed %, flip-o-rama churn rate).
extern volatile int gEffectHue;
extern volatile int gEffectDensity;
// "audio":true on /api/canvas/effect: fire/matrix/plasma read the microphone features
// (audio.h) and modulate speed/intensity. No-op when the ES7210 is absent.
extern volatile bool gEffectAudioMod;

uint8_t     effectByName(const char* name);   // wire name -> id (EFFECT_NONE if unknown / "none")
const char* effectName(uint8_t e);            // id -> canonical name ("none" for EFFECT_NONE)
const char* effectListJson();                 // all names as a JSON array, e.g. ["plasma",...]

// Self-describing effects (v3.4, the "effectDefs" capability token): one def per
// effect declaring exactly the params it consumes -- key, type, range, optional
// default, display label -- so a client can build its effect UI dynamically instead
// of hard-coding each option. Single source of truth: the param VOCABULARY (one
// entry per knob) and the per-effect index lists live next to the effect table in
// effects.cpp; a static_assert keeps them in lockstep with EFFECT_TABLE, and both
// the legacy flat "effectParams" union and the new "effectDefs" JSON derive from
// them at first use.
const char* effectDefsJson();          // [{id,name,params:[{key,type,...}]}...]

// Anti-aliased Orbitron text for the ops "text" aa path (v3.5): size picks the face
// (>=30 -> 34 px, >=18 -> 24 px, else 13 px), input is folded to the faces' charset
// (uppercase A-Z, digits, :.-+%/); unknown glyphs are skipped. align: 0 left, 1
// centre, 2 right about x. y is the TOP of the glyphs. Returns the drawn width.
int aaTextDraw(int x, int y, int size, const char* s, int align,
               uint8_t r, uint8_t g, uint8_t b);
const char* effectParamsUnionJson();   // legacy flat union, e.g. ["hue","density","audio"]

void effectReset(uint8_t type);   // prepare per-effect state; called only on taskDisplay
void effectRender(uint8_t type);  // render + present one frame; runs on taskDisplay
// EFFECT_SOUNDWALL is the one effect that does NOT own the panel as pixels: it drives
// the FLAP WALL (vmodule targets) and leaves the normal wall renderer running.
// taskDisplay calls this instead of effectRender for it, every display tick.
void effectSoundwallTick();
