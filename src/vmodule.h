// vmodule.h -- the emulated split-flap module.
//
// One VModule stands in for one ATtiny1616 module that would otherwise hang off
// the RS-485 bus. It speaks the whole wire protocol, byte for byte.
//
// The MECHANISM is not emulated. There is no stepper, no Hall sensor and no
// EEPROM, and nothing that can be out of tune. Every module is a flawless,
// perfectly calibrated one, and that assumption is what makes the emulation
// small:
//
//   * No calibration state. Home offset and steps-per-revolution are the
//     compile-time nominal values below, the same for every module and never
//     stored. The flap-position map is always empty, which is what a module with
//     no fine calibration reports anyway. The calibration commands ('o', 't',
//     's', 'g', 'w', 'e' and the 'mXW' restore's calibration fields) are accepted
//     and acknowledged -- none of them has a reply -- and then discarded. A 'd' or
//     'A' dump always reports the nominal values and an empty map.
//   * No position to lose. The reel always knows where it is, so 'h' (home)
//     simply shows flap 0, the blank.
//   * Nothing to measure. 'c' (calibrate) answers immediately with a correctly
//     formed frame reporting the result a flawless module gives. A controller
//     that sweeps the wall sees every module pass.
//
// What the reel does do is FLIP. Changing the displayed flap cascades forward
// through the reel one flap at a time, so the panel shows the split-flap effect
// that is the entire point of the display. That is a rendering effect, not a
// simulation: cfg.flapMax bounds how many flaps one change draws -- a longer jump
// starts the walk that many flaps short of the destination rather than trudging the
// whole way round, so the cascade stays the same length however big the reel is.
// Set flapMax to 1 for an instant cut.
//
// The reel
// --------
// 163 flaps: EVERY printable Windows-1252 glyph the font can draw, then the seven
// colours. A physical reel has 64 leaves because it is a physical object; this one is
// drawn, so there is nothing to ration and every character has somewhere to land.
//
//   index 0        ' '                   blank (the home position)
//   index 0..155   the CP1252 repertoire, in code-point order, MINUS the lowercase
//                  letters:  !"#$%&'()*+,-./0-9:;<=>?@A-Z[\]^_`{|}~ €‚ƒ„…†‡ˆ‰Š‹ŒŽ...
//                  ...¡¢£¤¥¦§¨©ª«¬®¯°±²³´µ¶·¸¹º»¼½¾¿ À-Þ ß ÷
//   index 156..162 r o y g b p w         COLOUR flaps (VM_COLOUR_BASE ..)
//
// NO LOWERCASE, and that is load-bearing. The colour flaps are addressed by the
// lowercase letters r o y g b p w, so if the reel carried lowercase, vmFlapIndexOf()
// would resolve 'r' to the letter and every colour command ever written would quietly
// start printing letters. Lowercase folds to uppercase instead (cp1252ToUpper), which
// is what a real reel -- printed in capitals -- does anyway.
//
// The reel is SHARED and FIXED. It is not stored per module and it is not configurable:
// a drawn reel that can already render everything has nothing left to reconfigure, so
// the 'N' command and the flap-set editor are gone. It is built once at boot by
// vmBuildReel() from isFlapByte() + cp1252IsLower(), rather than typed out, so it cannot
// drift from the font or from the folding rule.

#ifndef MPGW_VMODULE_H
#define MPGW_VMODULE_H

#include "common.h"

// Ceiling on the emulated wall (grid rows x cols). 192 covers every layout a 256px-wide
// chain can produce -- 32 columns of 8px cells x 6 rows -- with headroom. It is bounded
// from above by the protocol (module ids are 0..254) and from below by what it costs: the
// module array and the display snapshot are both INTERNAL RAM, the same pool the panel's
// framebuffer and the WiFi stack are already fighting over. At 40 bytes a module that is
// ~7.7 KB here, which is affordable; it was not when a module carried its own 64-byte flap
// table (see v1.5).
#define VM_MAX_MODULES   192
#define VM_FW_VERSION    31    // module firmware version the emulation reports
#define VM_SN_CHARS      20    // serial number length, uppercase hex

// What a perfectly tuned module reports. Constants, not state: these are the real
// firmware's defaults, and no command can move them.
#define VM_HOME_OFFSET   2832
#define VM_TOTAL_STEPS   4096

// The reel's colour flaps: the LAST seven, always. They are addressed by the LOWERCASE
// letters r o y g b p w, which is exactly why the reel carries no lowercase -- see the
// flap-set block in common.h. vmFlapIndexOf() checks them before it scans the glyphs, so
// 'r' can only ever be red.
// VM_COLOUR_BASE and the colour codes live in reel.h, beside the reel they index.

// The one reel every module shows: 156 CP1252 glyphs then the 7 colours. Built once at
// boot by vmBuildReel(); read-only thereafter.
extern const char* vmReel();

// A virtual module. flapChars dominates the footprint (~270 B per module). The array
// lives in INTERNAL RAM, not PSRAM: taskDisplay walks it at 100 Hz and quad PSRAM is
// too slow to sit on that path -- see vmInit().
struct VModule {
  // ---- identity (persisted) ----
  uint8_t  id;                    // 0..254, or 255 when unprovisioned
  bool     provisioned;
  char     sn[VM_SN_CHARS + 1];   // 20 uppercase hex chars, NUL-terminated

  // ---- configuration the protocol reads and writes (persisted) ----
  bool     autoHome;              // set by 'a', reported by 'A'; nothing acts on it
  uint8_t  bootCount;             // wraps at 255, reported by 'Q'
  // NOTE: there is no flap table here any more. Every module shows the SAME reel -- the
  // whole CP1252 repertoire (see vmBuildReel) -- so it lives once, in a shared constant,
  // instead of 163 bytes per module. That is the point of the change: a drawn reel can
  // render everything, so there is nothing to configure and nothing to store. It also
  // took sizeof(VModule) from 108 bytes to 40, which is 5 KB of internal RAM back on a
  // 75-module wall -- the same RAM the panel's framebuffer and WiFi are fighting over.

  // ---- runtime ----
  int16_t  curIndex;              // the flap on show; always valid
  int16_t  target;                // where the reel is flipping to; -1 = at rest
  uint8_t  flipPhase;             // 0 = settled, 1 = mid-flip (see display.cpp)
  uint32_t nextStepMs;            // millis() of the next flap advance
  bool     dirty;                 // needs persisting to /vmods.dat
};

// ---- owned globals (defined in globals.cpp) ----
extern VModule* vmods;
extern int      vmCount;
extern SemaphoreHandle_t vmMutex;
extern StaticSemaphore_t vmMutexBuf;
extern volatile bool     vmDirty;          // some module needs persisting
extern volatile unsigned long vmDirtyMs;

// Allocate and populate `count` modules (PSRAM), restoring /vmods.dat if present.
// Modules not found in the file are created pre-provisioned with IDs 0..count-1
// and a deterministic bogus serial number. Call after sfFsInit().
void vmInit(int count);

// Persist / restore the modules' protocol-visible state. Debounced by the
// network task.
void vmSave();
void vmLoad();

// Feed one complete frame from the emulated bus to every module. Called by
// vbusDeliver on the sender's task, so it must not block. Replies are queued (see
// vbus.h), never sent inline.
void vmDispatch(const uint8_t* frame, size_t len, uint32_t now);

// Advance every reel by at most one half-flap, and queue advertisements from any
// unprovisioned module. Called from the display task ~ every frame. Returns true
// if anything moved (so the panel needs a redraw).
bool vmTick(uint32_t now);

// Build the shared reel. Call once, before vmInit(). Idempotent.
void vmBuildReel();

// The character at flap `i`, or 0 if `i` is off the reel.
char vmFlapCharAt(int i);
// The flap index carrying `c`, or -1 if the reel cannot show it.
//
// Resolution order, and it matters:
//   1. the colour codes r o y g b p w -- lowercase by protocol, not letters;
//   2. 'q', the legacy alias splitflap-os uses for the double-quote flap (the classic
//      reel had no lowercase, so its char map stole that byte). The reel now carries a
//      real '"', so the alias is honoured rather than dropped;
//   3. cp1252ToUpper(c), because the reel is printed in capitals like a real one;
//   4. a scan of the 156 glyph flaps -- never the colours, which step 1 already owns.
int  vmFlapIndexOf(char c);
// The index-addressed resolver: a Unicode code point -> a flap, WITHOUT folding case and
// WITHOUT treating r/o/y/g/b/p/w as colours. Reaches the lowercase and pictograph flaps.
int  vmFlapIndexOfCodepoint(uint32_t cp);
// The font glyph a flap draws, or -1 (a colour flap has no glyph).
int  vmFlapGlyph(int i);
// The colour flap a pictograph is drawn in (0..6), or -1 for the normal text ink.
int  vmFlapTint(int i);
// True when flap `i` is a colour swatch rather than a glyph.
static inline bool vmFlapIsColour(int i) {
  return i >= VM_COLOUR_BASE && i < VM_COLOUR_BASE + SF_COLOUR_FLAPS;
}

#endif // MPGW_VMODULE_H
