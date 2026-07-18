// modules.cpp -- the module protocol: how a character, an index or a home becomes a frame.
//
// There is no module REGISTRY here any more, and that absence is the point. A registry is
// how the physical gateway learns what is out on its wire, where modules can be added,
// removed, renumbered or simply fail to answer. On this board the wall is DRAWN: vmods[] is
// created whole by vmInit() from rows x cols, every module exists, none can vanish, and
// vmods[i].curIndex is the flap on show -- not a report about it. A second copy of that
// truth could only ever be a worse one, and it was: it stored a BYTE where the truth is a
// flap index, so a quiet-time cycle folded "Hello world" into "HELLo worLD" and lost every
// pictograph on the wall. See sfSetQuietTime() below.

#include "gateway.h"

// Send one flap character. `c` is a single Windows-1252 byte: ASCII 0x20-0x7E, or a high
// byte (euro/accents/smart punctuation) -- see charset.h. Bytes that aren't a valid flap
// glyph (controls, undefined slots, NBSP/soft-hyphen) are dropped.
//
// The reel's glyph section is printed in capitals -- like a real one -- so lowercase must
// fold to its uppercase flap or it will not resolve. The seven colour codes are the
// exception: r o y g b p w are lowercase BY PROTOCOL, not letters, and folding them would
// turn red into the letter R.
//
// That fold is the legacy wire protocol's, and it is inescapable there: one byte per
// character, and seven of the letters already mean colours. The reel HAS lowercase flaps
// (and pictograph flaps) -- they are simply unreachable from this path. POST /api/display/cells
// addresses them by index, which is the only reason that endpoint exists. See reel.h.
//
// cp1252ToUpper() is the single definition of the fold (charset.cpp), and the reel is BUILT
// by excluding exactly what cp1252IsLower() accepts -- so the two cannot drift apart.
void sfSendChar(int addr, char c) {
  uint8_t b = (uint8_t)c;
  if (!strchr("roygbpw", (char)b)) b = cp1252ToUpper(b);
  if (!isFlapByte(b)) return;                                 // not a valid flap glyph
  char buf[24];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*-%c\n", b);
  else          snprintf(buf, sizeof(buf), "m%d-%c\n", addr, b);
  frameSendStr(buf);
}

// Display by flap index. addr=-1 = broadcast. The only way to reach a lowercase or
// pictograph flap.
void sfSendIndex(int addr, int idx) {
  char buf[24];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*+%d\n", idx);
  else          snprintf(buf, sizeof(buf), "m%d+%d\n", addr, idx);
  frameSendStr(buf);
}

// Home one module or all (addr=-1) -- flap 0, which is blank.
void sfHome(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*h\n");
  else          snprintf(buf, sizeof(buf), "m%dh\n", addr);
  frameSendStr(buf);
}

// Send a text string across a sequence of module IDs starting at startAddr.
//
// `text` arrives as UTF-8 (web UI / MQTT / JSON). Transcode it to the single-byte flap
// encoding first, so one displayed glyph -- including a euro sign or an accented letter,
// which are multi-byte in UTF-8 -- maps to exactly one module. Unrepresentable code points
// are dropped.
void sfSendText(int startAddr, const char* text) {
  char enc[SF_MAX_TEXT + 1];
  size_t len = utf8ToFlap(text, enc, sizeof(enc));
  // No pacing here. The physical gateway spaced these frames on the wire; on the
  // frame link a send is a function call, and a delay() would block the CALLING
  // task (taskWeb or taskNetwork) for 10 ms x strlen -- up to 2.5 s of frozen
  // HTTP/MQTT per text. The cascade the eye sees is the flip animation's job.
  for (size_t i = 0; i < len; i++) sfSendChar((int)(startAddr + i), enc[i]);
}

// Set Quiet Time on/off.
//
// RISING edge (off -> on): the wall is BLANKED. Each module's current flap is snapshotted
// into its pending slot, then one broadcast home drives every reel to the blank flap -- the
// same operation as the Home All button. The snapshot is what makes the blanking safe to
// undo: the falling edge replays it.
//
// ORDER MATTERS. The home must go out BEFORE gQuietTime is raised: frameSend suppresses
// display motion ('-', '+' and 'h') while quiet is on, so blanking after the flag would
// suppress the very frame that does the blanking.
//
// FALLING edge (on -> off): every module goes back to its pending flap -- what the host last
// asked for while quiet, or what it was showing when quiet began.
//
// THE SNAPSHOT IS A FLAP INDEX, NOT A CHARACTER, and that is the whole point of it. It used
// to live in the module registry as a byte, and the restore re-sent that byte through
// sfSendChar() -- straight back through the uppercase fold above. A wall reading
// "Hello world!er?" came out of a quiet-time cycle reading "HELLo worLD!Er?", and a
// pictograph, which has no byte at all, did not come back at all. A byte cannot name a flap
// on a 237-flap reel. An index can.
//
// Safe to call from any task. vmMutex is NEVER held across frameSend: vlinkDeliver re-takes
// it and would self-deadlock. Snapshot under the lock, send outside it.
void sfSetQuietTime(bool on) {
  const bool was = gQuietTime;

  if (!was && on) {
    if (vmods && vmMutex && xSemaphoreTake(vmMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < vmCount; i++)
        vmods[i].pendFlap = vmods[i].curIndex;   // exactly the flap on show
      xSemaphoreGive(vmMutex);
    }
    sfHome(-1);          // unlocked, and quiet is still off -- so this is not suppressed
    dispReturnToWall();  // drop any effect/raw-canvas so the blanked wall is what the panel shows
    printf("[QUIET] on -- wall blanked (home all)\n");
  }

  gQuietTime = on;       // only now: the blanking frame above had to get out first

  if (was && !on) {
    struct Pend { uint8_t id; int16_t flap; };
    static Pend list[VM_MAX_MODULES];
    int n = 0;
    if (vmods && vmMutex && xSemaphoreTake(vmMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < vmCount && n < VM_MAX_MODULES; i++) {
        if (vmods[i].pendFlap < 0) continue;
        list[n].id   = vmods[i].id;
        list[n].flap = vmods[i].pendFlap;
        vmods[i].pendFlap = -1;
        n++;
      }
      xSemaphoreGive(vmMutex);
    }
    for (int i = 0; i < n; i++) sfSendIndex(list[i].id, list[i].flap);   // BY INDEX: exact
    if (n) printf("[QUIET] off -- restored %d module(s) to their pending flap\n", n);
  }
  printf("[QUIET] Quiet Time %s\n", on ? "ENABLED" : "disabled");
}

/* ----------------------------------------------------------
   Filesystem
   ----------------------------------------------------------
   The registry is gone, but FATFS is not: /compset.gz holds the companion's
   settings blob. (The reels persist nothing -- see vmodule.cpp.)
---------------------------------------------------------- */
// Mount the FATFS partition. Format on first use if needed.
void sfFsInit(bool forceFormat) {
  // Panic-recovery (see main.cpp): when the supervisor has seen too many crash reboots in a row,
  // the FATFS is probably what keeps crashing the boot, so wipe it before mounting. FFat.format()
  // must run while UNMOUNTED, so it goes first -- before any begin().
  if (forceFormat) {
    printf("[RECOVERY] reformatting FATFS -- discarding possibly-corrupt state\n");
    FFat.format();
  }
  // Try to mount WITHOUT auto-format first (fast path on every normal boot).
  if (FFat.begin(false)) {
    sfFsReady = true;
    DBG("[MOD] FATFS mounted (%lu KB free)\n",
        (unsigned long)(FFat.freeBytes() / 1024));
    return;
  }
  // First boot after flashing: the partition is unformatted. Formatting is a long blocking
  // flash operation -- log it clearly so the delay is expected, and so the watchdog boot
  // grace period covers it.
  printf("[MOD] FATFS not formatted -- formatting now (one-time, may take a while)...\n");
  if (FFat.begin(true)) {       // true = format if mount fails
    sfFsReady = true;
    printf("[MOD] FATFS formatted and mounted (%lu KB free)\n",
           (unsigned long)(FFat.freeBytes() / 1024));
  } else {
    sfFsReady = false;
    printf("[MOD] FATFS mount/format failed -- persistence disabled\n");
  }
}
