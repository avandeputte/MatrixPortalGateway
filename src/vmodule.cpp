#include "gateway.h"
#include <esp_mac.h>
#include <esp_heap_caps.h>

// vmodule.cpp -- the emulated split-flap modules. See vmodule.h for the reel
// layout and for what is deliberately NOT emulated (everything mechanical).
//
// Two concerns: (1) building the reel; (2) vmDispatch, which parses a protocol
// frame and moves the addressed reels. Nothing answers: the query commands
// ('v'/'A') and their reply pipeline were removed in v1.24 -- no client ever
// sent them, and a drawn wall self-describes through /api/capabilities.
//
// There is NO persistence. Every field of every module is deterministic from
// the configured grid (id = wall slot, reel homed at boot), so /vmods.dat --
// which the physical modules need for their ids and calibration -- stored
// nothing here that could actually vary. vmInit deletes a leftover file from
// older firmware.
//
// Concurrency: everything here runs under vmMutex, taken by the caller
// (frameSend) or by vmTick itself. vmDispatch is reached
// only from frameSend, which holds txMutex, so its static scratch buffer has a
// single writer.

// ---- file-private forward declarations ----
static void   vmSetTarget(VModule& m, int idx);


/* ----------------------------------------------------------
   Reel, identity, defaults
---------------------------------------------------------- */

/* The reel lives in reel.h -- Arduino-free, so the native test compiles the same code.
   These are the thin bindings the rest of the firmware calls. */
static char sReel[SF_MAX_FLAPS];

void vmBuildReel() {
  int n = reelBuild(sReel);
  // If this fires, the CP1252 repertoire or the folding rule moved and SF_MAX_FLAPS is now
  // a lie. Shout: a reel with the wrong glyph count puts the colour flaps at the wrong
  // index, and every colour on the wall would be silently wrong.
  if (n != SF_CHAR_FLAPS)
    printf("[VM] FATAL: reel has %d glyph flaps, expected %d\n", n, SF_CHAR_FLAPS);
}

const char* vmReel()          { return sReel; }
char vmFlapCharAt(int i)      { return reelCharAt(sReel, i); }
uint32_t vmFlapCodepointAt(int i) { return reelCodepointAt(sReel, i); }
int  vmFlapIndexOf(char c)    { return reelIndexOf(sReel, c); }
int  vmFlapGlyph(int i)       { return reelGlyph(sReel, i); }
int  vmFlapTint(int i)        { return reelTint(i); }
// The index-addressed path, for POST /api/display/cells: no folding, no colour-stealing,
// and it reaches the lowercase and pictograph flaps the legacy protocol cannot name.
int  vmFlapIndexOfCodepoint(uint32_t cp) { return reelIndexOfCodepoint(sReel, cp); }

/* ----------------------------------------------------------
   The flip
   ----------------------------------------------------------
   A rendering effect, not a mechanism. The reel walks forward to the target flap
   so the panel shows the split-flap cascade, but cfg.flapMax caps how many flaps
   one change draws: a longer jump starts the walk that many flaps short of the
   destination instead of trudging the whole way round. There is no travel time to
   respect and no position to lose.
---------------------------------------------------------- */

static void vmSetTarget(VModule& m, int idx) {
  if (idx < 0 || idx >= SF_MAX_FLAPS) return;     // out of range: ignored, as in fw
  if (idx == m.curIndex) { m.target = -1; m.flipPhase = 0; return; }
  int dist = (idx - m.curIndex + SF_MAX_FLAPS) % SF_MAX_FLAPS;
  int cap  = cfg.flapMax ? cfg.flapMax : DEFAULT_FLAP_MAX;
  if (dist > cap) m.curIndex = (int16_t)((idx - cap + SF_MAX_FLAPS) % SF_MAX_FLAPS);
  m.target     = (int16_t)idx;
  m.flipPhase  = 0;
  m.nextStepMs = millis();
}

bool vmTick(uint32_t now) {
  if (!vmods) return false;
  if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  bool moved = false;
  uint16_t half = (cfg.flapMs ? cfg.flapMs : DEFAULT_FLAP_MS) / 2;
  if (!half) half = 1;

  for (int i = 0; i < vmCount; i++) {
    VModule& m = vmods[i];

    if (m.target < 0) continue;                          // at rest
    if ((int32_t)(now - m.nextStepMs) < 0) continue;     // not time yet

    // One flap = two half-steps: a mid-flip frame, then the settled frame. The
    // mid-flip frame is what the display draws as top-of-next over bottom-of-now.
    if (m.flipPhase == 0) {
      m.flipPhase  = 1;
      m.nextStepMs = now + half;
    } else {
      m.curIndex   = (int16_t)((m.curIndex + 1) % SF_MAX_FLAPS);
      m.flipPhase  = 0;
      m.nextStepMs = now + half;
      if (m.curIndex == m.target) m.target = -1;          // arrived
    }
    moved = true;
  }
  if (vmMutex) xSemaphoreGive(vmMutex);
  return moved;
}

void vmInit(int count) {
  if (count < 1) count = 1;
  if (count > VM_MAX_MODULES) count = VM_MAX_MODULES;
  // INTERNAL RAM, deliberately -- not gwPsramAlloc like the large buffers.
  // taskDisplay walks this array a hundred times a second (vmTick) on the core
  // the panel driver's setup ran on. This board's PSRAM is quad SPI: a cache
  // miss stalls the pipeline for most of a microsecond, and in the old
  // ISR-driven driver that visibly wandered the OE window (an idle wall that
  // shimmers -- see platformio.ini). Today's driver refreshes by GDMA with no
  // CPU involvement, but the array is small (~40 B/module) and hot, so it stays
  // internal. The command log and the MQTT queue stay in PSRAM -- nothing on
  // the display path touches them. ~2 KB for a 45-module wall.
  size_t vmBytes = sizeof(VModule) * (size_t)count;
  vmods = (VModule*) heap_caps_malloc(vmBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!vmods) { printf("[VM] FATAL: cannot allocate %d modules\n", count); return; }
  memset(vmods, 0, vmBytes);
  printf("[MEM] virtual modules in internal RAM (%u bytes)\n", (unsigned)vmBytes);
  vmCount = count;

  for (int i = 0; i < count; i++) {
    VModule& m = vmods[i];
    memset(&m, 0, sizeof(m));
    // Every module carries its wall position as its ID, from the moment the board
    // is flashed. There is no advertise/adopt protocol any more, so this is the
    // only place an ID is ever assigned.
    m.id         = (uint8_t)i;
    m.target     = -1;
    m.pendFlap   = -1;
    m.flipPhase  = 0;
    m.nextStepMs = millis();
    // HOME AT BOOT, unconditionally -- what a mechanical module with auto-home
    // does, and what anyone actually wants: come up blank, and let whatever
    // drives the wall draw the first real frame. (Restoring the pre-reboot wall
    // from flash was tried and rejected: it presented stale content as current.)
    m.curIndex   = 0;
  }
  // Older firmware persisted module state to /vmods.dat. Every field is
  // deterministic now, so the file is meaningless -- delete a leftover one.
  if (sfFsReady && FFat.exists("/vmods.dat")) FFat.remove("/vmods.dat");
  printf("[VM] %d virtual modules, %d flaps each (%d glyphs + %d colours + %d lowercase + %d pictographs)\n",
         vmCount, SF_MAX_FLAPS, SF_CHAR_FLAPS, SF_COLOUR_FLAPS, SF_LOWER_FLAPS,
         SF_EMOJI_FLAPS);
}

/* ----------------------------------------------------------
   Command dispatch
---------------------------------------------------------- */

void vmDispatch(const uint8_t* frame, size_t len) {
  if (!vmods || len < 2 || frame[0] != 'm') return;

  // Single writer: frameSend serialises every send through txMutex, and that is
  // the only path here.
  static char buf[TX_MAX_BYTES + 1];
  size_t n = (len < TX_MAX_BYTES) ? len : TX_MAX_BYTES;
  memcpy(buf, frame, n);
  buf[n] = 0;
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  if (n < 2) return;

  const char* p = buf + 1;

  bool bcast = false;
  int  addr  = -1;
  if (*p == '*') {                       // m* (v7+) or m** (v6)
    bcast = true; p++;
    if (*p == '*') p++;
  } else if (isdigit((unsigned char)*p)) {
    long v = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; if (v > 254) return; }
    addr = (int)v;
  } else {
    return;                              // malformed address
  }
  if (!*p) return;
  char cmd = *p++;
  const char* payload = p;

  // A command that changes what the wall shows (character, flap index, home) means the user wants
  // the split-flap wall, not an on-device effect or raw canvas -- so hand the panel back. A no-op
  // (one volatile compare) unless one is actually running; only the first frame of a cascade acts.
  if (cmd == '-' || cmd == '+' || cmd == 'h') dispReturnToWall();

  // Resolve the display payload ONCE, outside the module loop: on a broadcast the
  // reel scan / strtol are loop-invariant, and this runs under both txMutex and
  // vmMutex. -1 means "no valid payload -> the case below does nothing".
  int showIdx = -1;
  if (cmd == '-' && payload[0]) {
    int r = vmFlapIndexOf(payload[0]);
    showIdx = (r < 0) ? 0 : r;   // unknown character homes, as of fw v31
  } else if (cmd == '+' && isdigit((unsigned char)payload[0])) {
    showIdx = (int)strtol(payload, NULL, 10);
  }

  for (int i = 0; i < vmCount; i++) {
    VModule& m = vmods[i];
    if (!bcast && m.id != addr) continue;

    switch (cmd) {
      // ---- display ----
      case '-':     // show character (resolved to showIdx above)
      case '+':     // show flap index; out of range is ignored by vmSetTarget
        if (showIdx >= 0) vmSetTarget(m, showIdx);
        break;
      case 'h': vmSetTarget(m, 0); break;
      default: break;                    // unknown command: silently ignored
    }
    if (!bcast) break;                   // a direct frame targets exactly one module
  }
}
