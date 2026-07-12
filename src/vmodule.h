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
// simulation: cfg.flapMax (<= FLAP_ANIM_MAX = 64) bounds how many flaps one
// change draws, so jumping from 'z' to 'a' takes about a second rather than
// walking 200 flaps. Set flapMax to 1 for an instant cut.
//
// The reel
// --------
// 64 flaps -- the same count as a real module (the count is protocol; see common.h).
// The default is a GERMAN layout (VM_DEFAULT_REEL in vmodule.cpp):
//
//   ` ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜß0123456789!@#&€-=;q:'.,/?*roygbpw`
//
//   index 0        ' '              blank
//   index 1..26    A..Z
//   index 27..30   Ä Ö Ü ß
//   index 31..40   0..9
//   index 41..56   ! @ # & € - = ; q : ' . , / ? *
//   index 57..63   r o y g b p w    COLOUR flaps (VM_COLOUR_BASE .. +SF_COLOUR_FLAPS)
//
// The colours sit LAST. vmFlapIndexOf() is a first-match scan, and the only lowercase
// bytes on the reel are the seven colour codes and 'q' -- so `m5-r` selects the red
// flap and never the letter 'R' (a distinct byte at index 18). sfSendChar() also
// uppercases ASCII (except the colour codes), so lowercase letters resolve to their
// uppercase flap. 'q' (index 49) is the double-quote flap; the renderer draws it as `"`.
//
// Which flaps are colour swatches is a property of the reel POSITION, not the
// character: [colourBase, colourBase + colourCount). The 'N' command reassigns
// characters, not physical flaps, so reconfiguring the char set leaves the colour
// flaps where they are.

#ifndef MPGW_VMODULE_H
#define MPGW_VMODULE_H

#include "common.h"

#define VM_MAX_MODULES   128   // ceiling on the emulated wall (grid rows x cols)
#define VM_FW_VERSION    31    // module firmware version the emulation reports
#define VM_SN_CHARS      20    // serial number length, uppercase hex

// What a perfectly tuned module reports. Constants, not state: these are the real
// firmware's defaults, and no command can move them.
#define VM_HOME_OFFSET   2832
#define VM_TOTAL_STEPS   4096

// The reel's colour flaps: the LAST seven, as on the real reel. (The 64-flap reel has
// no lowercase besides the colour
// codes and 'q', so the real reel's ordering is safe again -- and it is what the
// physical modules ship, which is the whole point of matching it.)
#define VM_COLOUR_BASE   (SF_MAX_FLAPS - SF_COLOUR_FLAPS)   // 57
extern const char VM_COLOUR_CHARS[SF_COLOUR_FLAPS + 1];   // "roygbpw"

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
  bool     flapCharsCustom;       // false = the compile-time default reel
  uint8_t  flapCount;             // ACTIVE flaps, 1..SF_MAX_FLAPS
  uint8_t  flapCharsLen;          // bytes used in flapChars
  uint8_t  colourBase;            // first colour-flap index
  uint8_t  colourCount;           // number of colour flaps (0 = none)
  uint8_t  bootCount;             // wraps at 255, reported by 'Q'
  // Length-counted, not NUL-terminated: an 'N' command may set an arbitrary reel,
  // and the real firmware reserves 0xFF as its "unused flap" sentinel, so a byte
  // count is the only representation that round-trips every legal reel.
  // NOTE: this array is embedded in the on-disk record (vmSave/vmLoad). Changing
  // SF_MAX_FLAPS changes sizeof(VModule) and therefore the record layout -- bump
  // VMODULES_MAGIC when you do, or a stale file deserialises into garbage.
  char     flapChars[SF_MAX_FLAPS];

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

// The character at flap `i` on this module's reel, or 0 if `i` is past the set.
char vmFlapCharAt(const VModule& m, int i);
// First flap index carrying `c`, or -1. First-match, so colours beat letters.
int  vmFlapIndexOf(const VModule& m, char c);
// True when flap `i` is a colour swatch rather than a glyph.
static inline bool vmFlapIsColour(const VModule& m, int i) {
  return m.colourCount && i >= m.colourBase && i < m.colourBase + m.colourCount;
}

#endif // MPGW_VMODULE_H
