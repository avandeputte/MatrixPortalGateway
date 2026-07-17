#include "gateway.h"
#include <esp_mac.h>
#include <esp_heap_caps.h>

// vmodule.cpp -- the emulated split-flap modules. See vmodule.h for the reel
// layout and for what is deliberately NOT emulated (everything mechanical).
//
// Four concerns: (1) building the default reel and the bogus serial numbers;
// (2) persisting each module's protocol-visible state to /vmods.dat;
// (3) vmDispatch, which parses a bus frame and applies it to every addressed
// module; (4) vmRenderReply, which turns a queued reply intent back into
// protocol bytes.
//
// Concurrency: everything here runs under vmMutex, taken by the caller
// (vbusDeliver, vbusPoll) or by vmTick/vmSave themselves. vmDispatch is reached
// only from rs485Send, which holds txMutex, so its static scratch buffer has a
// single writer.

// ---- file-private forward declarations ----
static void   vmApplyDefaults(VModule& m);
static void   vmMakeSerial(int index, char* out);
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

// A deterministic, obviously-fake serial number: 20 uppercase hex chars over
// {0xFA, 0x5E, <the board's 6 MAC bytes>, <module index>, <crc8>}. It reads
// "FA5E..." -- fabricated -- so a serial from this emulator is never mistaken for
// an ATtiny SIGROW read, while still being unique per board and stable across reboots.
// Reported by the 'v' and 'A' replies, exactly as a real module reports its own.
static void vmMakeSerial(int index, char* out) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint8_t b[10] = { 0xFA, 0x5E, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    (uint8_t)index, 0 };
  uint8_t crc = 0;                         // CRC-8/ATM (poly 0x07)
  for (int i = 0; i < 9; i++) {
    crc ^= b[i];
    for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                                                   : (uint8_t)(crc << 1);
  }
  b[9] = crc;
  for (int i = 0; i < 10; i++) sprintf(out + i * 2, "%02X", b[i]);
  out[VM_SN_CHARS] = 0;
}

static void vmApplyDefaults(VModule& m) {
  m.autoHome = true;      // the reel is shared now: there is nothing else to default
}



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

/* ----------------------------------------------------------
   Persistence -- the protocol-visible state, in /vmods.dat
---------------------------------------------------------- */

struct PersistedVModule {
  uint8_t  id;
  bool     provisioned;
  char     sn[VM_SN_CHARS + 1];
  bool     autoHome;
  uint8_t  bootCount;
  int16_t  curIndex;
};

struct VModulesFileHeader { unsigned long magic; int count; };

void vmSave() {
  if (!sfFsReady || !vmods) return;
  File f = FFat.open(VMODULES_TMP, "w");
  if (!f) { DBG("[VM] open for write failed\n"); return; }
  VModulesFileHeader hdr = { VMODULES_MAGIC, vmCount };
  f.write((const uint8_t*)&hdr, sizeof(hdr));

  // Copy each record under the lock, then write it outside. Holding vmMutex for
  // the whole ~11 KB flash write would stall vbusDeliver -- and therefore every
  // command the gateway sends -- for as long as the write takes. A record torn
  // across the boundary is harmless: this is persistence, not a transaction.
  static PersistedVModule rec;   // one record at a time; taskNetwork's stack is 8 KB
  for (int i = 0; i < vmCount; i++) {
    if (vmMutex) xSemaphoreTake(vmMutex, portMAX_DELAY);
    const VModule& m = vmods[i];
    memset(&rec, 0, sizeof(rec));
    rec.id = m.id; rec.provisioned = m.provisioned;
    strlcpy(rec.sn, m.sn, sizeof(rec.sn));
    rec.autoHome = m.autoHome;
    rec.bootCount = m.bootCount; rec.curIndex = m.curIndex;
    if (vmMutex) xSemaphoreGive(vmMutex);
    f.write((const uint8_t*)&rec, sizeof(rec));
  }
  f.close();
  // Temp-file-then-rename, so a crash mid-write cannot corrupt the good copy.
  FFat.remove(VMODULES_FILE);
  FFat.rename(VMODULES_TMP, VMODULES_FILE);
  DBG("[VM] saved %d modules\n", vmCount);
}

void vmLoad() {
  if (!sfFsReady || !vmods) return;
  if (!FFat.exists(VMODULES_FILE)) { DBG("[VM] no saved state\n"); return; }
  File f = FFat.open(VMODULES_FILE, "r");
  if (!f) return;
  VModulesFileHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != VMODULES_MAGIC) {
    DBG("[VM] bad magic -- ignoring saved state\n");
    f.close();
    return;
  }
  // The wall may have been resized since the last boot. Restore what overlaps;
  // any extra modules keep the freshly-built defaults vmInit gave them.
  static PersistedVModule rec;
  int n = (hdr.count < vmCount) ? hdr.count : vmCount;
  int loaded = 0;
  for (int i = 0; i < n; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    VModule& m = vmods[i];
    m.id = rec.id; m.provisioned = rec.provisioned;
    strlcpy(m.sn, rec.sn, sizeof(m.sn));
    m.bootCount = rec.bootCount;

    // No reel to restore: it is shared, complete and rebuilt at boot. The old file
    // carried a per-module copy, which was its own hazard -- a firmware whose default
    // reel had changed would silently resurrect the OLD one on every provisioned board.
    // That whole class of bug is gone with the field. (VMODULES_MAGIC is bumped, so a
    // file from a build that still had it is discarded rather than misread.)
    m.autoHome = rec.autoHome;
    m.curIndex = (rec.curIndex >= 0 && rec.curIndex < SF_MAX_FLAPS) ? rec.curIndex : 0;
    loaded++;
  }
  f.close();
  DBG("[VM] restored %d of %d modules\n", loaded, vmCount);
}

void vmInit(int count) {
  if (count < 1) count = 1;
  if (count > VM_MAX_MODULES) count = VM_MAX_MODULES;
  // INTERNAL RAM, deliberately -- not gwPsramAlloc like the other three arrays.
  // taskDisplay walks this array a hundred times a second (vmTick), on core 1, which
  // is where the display task runs. This board's PSRAM is quad SPI: a
  // cache miss stalls the pipeline for most of a microsecond and the ISR cannot be
  // taken until the load retires, so the OE window for the short bitplanes wanders
  // and the panel flickers. The monitor ring and the MQTT queue stay in PSRAM -- nothing
  // on the display path touches them. ~2 KB for a 45-module wall.
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
    m.id          = (uint8_t)i;
    m.provisioned = true;
    vmMakeSerial(i, m.sn);
    vmApplyDefaults(m);
  }
  vmLoad();

  for (int i = 0; i < count; i++) {
    VModule& m = vmods[i];
    // A /vmods.dat written by a build that still had de-provisioning can hold a
    // module parked at id 255. Nothing advertises and nothing assigns IDs now, so
    // that module would answer no frame ever again. Re-adopt it at its wall slot.
    if (!m.provisioned || m.id == 255) {
      m.id = (uint8_t)i;
      m.provisioned = true;
    }
    m.bootCount  = (uint8_t)(m.bootCount + 1);   // wraps at 255, like the real one
    m.target     = -1;
    m.pendFlap   = -1;
    m.flipPhase  = 0;
    m.nextStepMs = millis();
    // HOME AT BOOT, if the module says to -- which by default it does.
    //
    // 'a' sets autoHome and 'A' reports it, and a REAL module with the flag set drives its
    // reel to the blank flap on power-up. This emulation used to store the flag, report it,
    // and then quietly ignore it, on the reasoning that a drawn reel has no position to
    // lose. True -- but it does have a position to REMEMBER, and /vmods.dat faithfully
    // restored it, so a reboot brought the wall up still showing whatever half-finished
    // sentence was on it when the power went. That is not "no position to lose", it is
    // stale content presented as current.
    //
    // Honouring the flag is both what a mechanical module does and what anyone actually
    // wants: come up blank, and let whatever drives the wall draw the first real frame.
    // A module whose autoHome has been cleared over the bus keeps the old behaviour.
    if (m.autoHome) m.curIndex = 0;
    if (m.curIndex < 0 || m.curIndex >= SF_MAX_FLAPS) m.curIndex = 0;
  }
  vmDirty = true;   // persist the bumped boot counters
  printf("[VM] %d virtual modules, %d flaps each (%d glyphs + %d colours), sn %s..\n",
         vmCount, SF_MAX_FLAPS, SF_CHAR_FLAPS, SF_COLOUR_FLAPS, vmods[0].sn);
}

/* ----------------------------------------------------------
   Command dispatch
---------------------------------------------------------- */

// By-serial frames: "mX<letter><serial>[...]". Every module inspects them and only
// the one whose serial matches acts.
static void vmDispatchBySerial(const char* p, size_t len, uint32_t now) {
  if (!len) return;
  char cmd = p[0];
  const char* sn = p + 1;
  // Every mX command carries the 20-char serial first; anything shorter is not a
  // command at all.
  if (strlen(sn) < VM_SN_CHARS) return;

  for (int i = 0; i < vmCount; i++) {
    VModule& m = vmods[i];
    if (strncmp(sn, m.sn, VM_SN_CHARS) != 0) continue;
    switch (cmd) {
      case 'A': vbusQueue((uint8_t)i, VR_ALL, 0, now + VBUS_REPLY_MS); return;
      // 'N' (set the flap set) is gone: the reel is shared, complete and fixed.
      default: return;
    }
  }
}

void vmDispatch(const uint8_t* frame, size_t len, uint32_t now) {
  if (!vmods || len < 2 || frame[0] != 'm') return;

  // Single writer: rs485Send serialises every send through txMutex, and that is
  // the only path here.
  static char buf[TX_MAX_BYTES + 1];
  size_t n = (len < TX_MAX_BYTES) ? len : TX_MAX_BYTES;
  memcpy(buf, frame, n);
  buf[n] = 0;
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  if (n < 2) return;

  const char* p = buf + 1;
  if (*p == 'X') { vmDispatchBySerial(p + 1, n - 2, now); return; }

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

  // 'v' and 'A' accept an optional "<lo>-<hi>" id range on a broadcast, so a
  // controller can poll a large bus in retryable batches.
  int lo = 0, hi = 254;
  if (bcast && (cmd == 'v' || cmd == 'A') && isdigit((unsigned char)*payload)) {
    char* e;
    long a = strtol(payload, &e, 10);
    if (*e == '-') { lo = (int)a; hi = (int)strtol(e + 1, NULL, 10); }
  }

  for (int i = 0; i < vmCount; i++) {
    VModule& m = vmods[i];
    if (bcast) { if (!m.provisioned) continue; }
    else       { if (!m.provisioned || m.id != addr) continue; }

    switch (cmd) {
      // ---- display ----
      case '-': {   // show character. An unknown character homes, as of fw v31.
        if (!payload[0]) break;
        int idx = vmFlapIndexOf(payload[0]);
        vmSetTarget(m, idx < 0 ? 0 : idx);
        break;
      }
      case '+':     // show flap index; out of range is ignored
        if (isdigit((unsigned char)payload[0])) vmSetTarget(m, (int)strtol(payload, NULL, 10));
        break;
      case 'h': vmSetTarget(m, 0); break;

      // 'N' (set the flap count / character set) is GONE. The reel is shared, carries
      // every glyph the font can draw, and is not configurable -- so an 'N' frame now
      // falls through to the default below and is ignored, exactly like any other
      // command this module does not implement.
      // ---- queries ----
      // 'v' and 'A' answer a broadcast too. Each module gets its own slot so the
      // train stays in ID order; the slots are milliseconds, not the hundreds a
      // real half-duplex bus needs to dodge collisions.
      case 'v':
        if (bcast && (m.id < lo || m.id > hi)) break;
        vbusQueue((uint8_t)i, VR_VER, 0,
                  now + VBUS_REPLY_MS + (bcast ? (uint32_t)(m.id - lo) * VBUS_SLOT_MS : 0));
        break;
      case 'A':
        if (bcast && (m.id < lo || m.id > hi)) break;
        vbusQueue((uint8_t)i, VR_ALL, 0,
                  now + VBUS_REPLY_MS + (bcast ? (uint32_t)(m.id - lo) * VBUS_SLOT_MS : 0));
        break;
      default: break;                    // unknown command: silently ignored
    }
    if (!bcast) break;                   // a direct frame targets exactly one module
  }
}

/* ----------------------------------------------------------
   Replies
   ----------------------------------------------------------
   Correctly formed protocol frames reporting a flawless module, every time: the
   Hall sensor sees exactly one clean pulse per revolution, the mechanism turns
   exactly VM_TOTAL_STEPS every revolution with zero spread, the supply is
   nominal, the EEPROM verifies, and the flap map is empty (uncalibrated, which is
   what a module with no fine calibration reports).
---------------------------------------------------------- */

size_t vmRenderReply(uint8_t mod, VmReplyKind kind, int32_t arg,
                     uint8_t* out, size_t outSize) {
  if (mod >= vmCount || outSize < 32) return 0;
  const VModule& m = vmods[mod];
  char*  o = (char*)out;
  size_t n = 0;

  switch (kind) {
    case VR_VER:
      n = snprintf(o, outSize, "m%dv:%d:%d:%s\n", m.id, VM_FW_VERSION, m.id, m.sn);
      break;

    case VR_ALL: {
      // m<id>A:<ver>:<id>:<sn>:<ho>:<ts>:<autoHome>:<curIndex>:<map>:<count>:<chars>
      // with <map> empty.
      n = snprintf(o, outSize, "m%dA:%d:%d:%s:%d:%d:%d:%d::%u:",
                   m.id, VM_FW_VERSION, m.id, m.sn, VM_HOME_OFFSET, VM_TOTAL_STEPS,
                   m.autoHome ? 1 : 0, m.curIndex, SF_LEGACY_FLAPS);
      // Report the LEGACY reel only -- the 163 flaps this protocol can actually name.
      //
      // It must stop there, and not merely as a matter of taste: the flaps past 163 are
      // the lowercase letters and the pictographs, and a pictograph HAS NO BYTE. Copying
      // the whole reel would splice fourteen NUL bytes into the middle of an ASCII frame
      // and the parser at the other end would read a truncated string. The extra flaps are
      // simply not part of this protocol; they are reachable by index, and a controller
      // that wants them uses the index-addressed API.
      //
      // The 163 bytes bring the frame to ~225 -- well inside TX_MAX_BYTES. The clamp stays
      // regardless: never half-write a reel.
      size_t cl = SF_LEGACY_FLAPS;
      if (n + cl + 2 > outSize) cl = (outSize > n + 2) ? outSize - n - 2 : 0;
      memcpy(o + n, vmReel(), cl);
      n += cl;
      if (n + 1 < outSize) o[n++] = '\n';
      break;
    }
    default: return 0;
  }
  if (n >= outSize) n = outSize - 1;
  return n;
}
