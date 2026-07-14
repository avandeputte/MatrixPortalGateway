#include "gateway.h"



// modules.cpp -- split-flap module registry and protocol.
// Three concerns: (1) the in-RAM registry of known modules (sfUpsert/sfFindById
// ... under sfMutex) and its FATFS persistence; (2) command builders that emit
// the ASCII bus protocol via rs485Send (sfSendChar, sfHome, sfSendText, ...);
// (3) sfParseResponse, which decodes every inbound frame (version, combined 'A')
// and updates the registry or the capture mailbox.
// ---- file-private forward declarations ----
static SFModule* sfFindBySN(const char* sn);
static SFModule* sfUpsert(uint8_t id, const char* sn);
static bool sfApplyVersionFields(uint8_t id, const char* fwCopy, const char* sn);
static bool sfValidSN(const char* sn);
static inline void sfTouch(SFModule* m);
static void sfTrackCharLocked(int addr, char c);

/* ----------------------------------------------------------
   Split-flap protocol helpers
---------------------------------------------------------- */

// Find or create a module registry entry by ID
SFModule* sfFindById(uint8_t id) {
  for (int i = 0; i < sfModuleCount; i++)
    if (sfModules[i].id == id) return &sfModules[i];
  return NULL;
}

// Find module by serial number.
//
// The NULL guard is not defensive padding: sfUpsert() is called as sfUpsert(id, NULL) from
// the version-reply path, and it forwards that NULL here whenever id == 255. Nothing on this
// firmware ever produces id 255 -- the virtual modules take their cell index -- so the only
// thing standing between this and strcmp(x, NULL) was an invariant nobody had written down.
// -Wall found it. Write the guard, not the invariant.
static SFModule* sfFindBySN(const char* sn) {
  if (!sn) return NULL;
  for (int i = 0; i < sfModuleCount; i++)
    if (strcmp(sfModules[i].serialNum, sn) == 0) return &sfModules[i];
  return NULL;
}

// Add or update a module entry
static SFModule* sfUpsert(uint8_t id, const char* sn) {
  SFModule* m = (id != 255) ? sfFindById(id) : sfFindBySN(sn);
  bool isNew = false;
  if (!m) {
    if (sfModuleCount >= MAX_MODULES) return NULL;
    m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id = id;
    m->flapIndex = -1;
    m->flapChar  = 0;
    isNew = true;
  }
  if (sn && sn[0]) strlcpy(m->serialNum, sn, sizeof(m->serialNum));
  m->lastSeen = millis();
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
  if (isNew) sfModulesDirty = true;  // new module -> persist
  return m;
}
// Mount the FATFS partition. Format on first use if needed.
void sfFsInit() {
  // Try to mount WITHOUT auto-format first (fast path on every normal boot).
  if (FFat.begin(false)) {
    sfFsReady = true;
    DBG("[MOD] FATFS mounted (%lu KB free)\n",
        (unsigned long)(FFat.freeBytes() / 1024));
    return;
  }
  // First boot after flashing: the partition is unformatted. Formatting a
  // ~10MB partition is a long blocking flash operation -- log it clearly so
  // the delay is expected, and so the watchdog boot grace period covers it.
  printf("[MOD] FATFS not formatted -- formatting now (one-time, may take a while)...\n");
  if (FFat.begin(true)) {       // true = format if mount fails
    sfFsReady = true;
    printf("[MOD] FATFS formatted and mounted (%lu KB free)\n",
           (unsigned long)(FFat.freeBytes() / 1024));
  } else {
    sfFsReady = false;
    printf("[MOD] FATFS mount/format failed -- module persistence disabled\n");
  }
}

// Save the current registry to the FATFS file.
void sfModulesSave() {
  if (!sfFsReady) return;
  // Build a compact array of durable records under sfMutex.
  static PersistedModule recs[MAX_MODULES];  // static: avoid large stack frame
  int n = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
    const SFModule& m = sfModules[i];
    recs[n].id            = m.id;
    strlcpy(recs[n].serialNum, m.serialNum, sizeof(recs[n].serialNum));
    recs[n].provisioned   = m.provisioned;
    strlcpy(recs[n].fwVersion, m.fwVersion, sizeof(recs[n].fwVersion));
    recs[n].lastSeenEpoch = m.lastSeenEpoch;
    n++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);

  // Write to a temp file then rename, so a crash mid-write can't corrupt
  // the existing good copy.
  File f = FFat.open(MODULES_FILE ".tmp", "w");
  if (!f) { DBG("[MOD] open for write failed\n"); return; }
  ModulesFileHeader hdr = { MODULES_MAGIC, n };
  f.write((const uint8_t*)&hdr, sizeof(hdr));
  if (n > 0) f.write((const uint8_t*)recs, n * sizeof(PersistedModule));
  f.close();
  FFat.remove(MODULES_FILE);
  FFat.rename(MODULES_FILE ".tmp", MODULES_FILE);
  DBG("[MOD] Saved %d modules to FATFS\n", n);
}

// Load persisted modules from the FATFS file at boot, pruning stale entries.
void sfModulesLoad() {
  if (!sfFsReady) return;
  if (!FFat.exists(MODULES_FILE)) { DBG("[MOD] no saved module file\n"); return; }
  File f = FFat.open(MODULES_FILE, "r");
  if (!f) { DBG("[MOD] open for read failed\n"); return; }

  ModulesFileHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic != MODULES_MAGIC || hdr.count <= 0 || hdr.count > MAX_MODULES) {
    DBG("[MOD] bad/empty module file -- skipping\n");
    f.close();
    return;
  }
  static PersistedModule recs[MAX_MODULES];
  size_t want = (size_t)hdr.count * sizeof(PersistedModule);
  size_t got  = f.read((uint8_t*)recs, want);
  f.close();
  if (got != want) {
    DBG("[MOD] file size mismatch (%u != %u) -- skipping\n",
        (unsigned)got, (unsigned)want);
    return;
  }

  unsigned long nowEp = rtcEpochNow();  // 0 if RTC not yet valid
  int loaded = 0, pruned = 0;
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  for (int i = 0; i < hdr.count && sfModuleCount < MAX_MODULES; i++) {
    // Prune entries older than MODULE_STALE_SECS (only when we have a
    // valid clock AND a recorded epoch to compare against).
    if (nowEp && recs[i].lastSeenEpoch &&
        nowEp > recs[i].lastSeenEpoch &&
        (nowEp - recs[i].lastSeenEpoch) > MODULE_STALE_SECS) {
      pruned++;
      continue;
    }
    // Skip records whose SN fails validation: a bus collision before the
    // validation fix could have persisted a garbage SN (e.g. a glued frame
    // tail). Dropping the record here HEALS the registry -- the module
    // re-registers with its correct SN on its next version response.
    {
      char snChk[21];
      strlcpy(snChk, recs[i].serialNum, sizeof(snChk));
      if (snChk[0] && !sfValidSN(snChk)) {
        DBG("[MOD] dropping persisted record id=%d with corrupt SN\n", recs[i].id);
        pruned++;
        continue;
      }
    }
    SFModule* m = &sfModules[sfModuleCount++];
    memset(m, 0, sizeof(SFModule));
    m->id            = recs[i].id;
    strlcpy(m->serialNum, recs[i].serialNum, sizeof(m->serialNum));
    m->provisioned   = recs[i].provisioned;
    strlcpy(m->fwVersion, recs[i].fwVersion, sizeof(m->fwVersion));
    m->lastSeenEpoch = recs[i].lastSeenEpoch;
    m->flapIndex     = -1;
    m->flapChar      = 0;
    m->lastSeen      = 0;   // not seen yet this boot
    loaded++;
  }
  if (sfMutex) xSemaphoreGive(sfMutex);
  DBG("[MOD] Loaded %d modules from FATFS (%d pruned as stale)\n", loaded, pruned);
}

// Wipe both the in-memory registry and the persisted file.
void sfModulesClear() {
  if (sfMutex) xSemaphoreTake(sfMutex, portMAX_DELAY);
  sfModuleCount = 0;
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  if (sfMutex) xSemaphoreGive(sfMutex);
  if (sfFsReady) FFat.remove(MODULES_FILE);
  sfModulesDirty = false;
  DBG("[MOD] Registry cleared (memory + FATFS)\n");
}

/* ----------------------------------------------------------
   Send split-flap commands
   All generate the ASCII bus protocol and call rs485SendStr()
---------------------------------------------------------- */

// Display a character on one module.  addr=-1 = broadcast.
// The gateway does NOT translate the character to a flap index -- the module
// firmware does that itself. We only ensure it is a printable ASCII byte and
// uppercase it (the flap set is uppercase), then send m<id>-<char> verbatim.
// Record the character a module is now displaying. addr<0 means a broadcast
// was sent, so every known module shows the same character -- update them all.
// Pass c=0 to mark the displayed character as unknown (e.g. after a home, when
// the module has left its previous flap but we can't name the new one without
// the module's flap table). flapIndex is cleared because the gateway tracks the
// character, not the index, on the char path. Caller must already hold sfMutex.
static void sfTrackCharLocked(int addr, char c) {
  if (addr < 0) {
    for (int i = 0; i < sfModuleCount; i++) {
      if (sfModules[i].provisioned) {
        sfModules[i].flapChar  = c;
        sfModules[i].flapIndex = -1;
      }
    }
  } else {
    SFModule* m = sfFindById((uint8_t)addr);
    if (m) { m->flapChar = c; m->flapIndex = -1; }
  }
}

// Mutex-wrapping convenience for callers that are not already holding sfMutex.
void sfTrackChar(int addr, char c) {
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    sfTrackCharLocked(addr, c);
    xSemaphoreGive(sfMutex);
  }
}

// Send one flap character. `c` is a single Windows-1252 byte: ASCII 0x20-0x7E, or
// a high byte (euro/accents/smart punctuation) -- see charset.h. Bytes that aren't
// a valid flap glyph (controls, undefined slots, NBSP/soft-hyphen) are dropped.
//
// The reel is 64 flaps and carries no lowercase (see vmodule.h), so we uppercase
// here exactly as the RS-485 gateway does -- with one exception: the seven colour
// codes. The protocol conveys the colour flaps as the lowercase letters
// r/o/y/g/b/p/w, so those are NOT folded; they stay lowercase and a first-match
// lookup resolves them to colours (the reel has no lowercase letters to collide
// with, only 'q' and the colour codes). Accented lowercase is folded too.
void sfSendChar(int addr, char c) {
  uint8_t b = (uint8_t)c;
  // The reel is printed in capitals -- like a real one -- so lowercase must fold to its
  // uppercase flap or it will not resolve. The seven colour codes are the exception:
  // r o y g b p w are lowercase BY PROTOCOL, not letters, and folding them would turn
  // red into the letter R.
  //
  // cp1252ToUpper() is the single definition of that rule (charset.cpp). The module folds
  // with the same function when it resolves a frame, and the reel is BUILT by excluding
  // exactly what cp1252IsLower() accepts -- so the three cannot drift apart. This used to
  // be a hand-rolled copy of the rule right here, complete with its own commentary about
  // 0xF7 and 0xFF.
  if (!strchr("roygbpw", (char)b)) b = cp1252ToUpper(b);
  if (!isFlapByte(b)) return;                                 // not a valid flap glyph
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*-%c\n", b);
  else
    snprintf(buf, sizeof(buf), "m%d-%c\n", addr, b);
  rs485SendStr(buf);
  // Display tracking is handled centrally in rs485Send via sfTrackFromFrame,
  // so every path (including raw frame sends) is covered uniformly.
}

// Display by flap index.  addr=-1 = broadcast.
void sfSendIndex(int addr, int idx) {
  char buf[24];
  if (addr < 0)
    snprintf(buf, sizeof(buf), "m*+%d\n", idx);
  else
    snprintf(buf, sizeof(buf), "m%d+%d\n", addr, idx);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame.
}



// Home one module or all (addr=-1)
void sfHome(int addr) {
  char buf[16];
  if (addr < 0) snprintf(buf, sizeof(buf), "m*h\n");
  else          snprintf(buf, sizeof(buf), "m%dh\n", addr);
  rs485SendStr(buf);
  // Display tracking handled centrally in rs485Send via sfTrackFromFrame
  // (home -> displayed character becomes unknown).
}

// Query firmware version of a DIRECT (single-id, never broadcast) module.
//
// NOTE: the omitted trailing '\n' is NOT a module protocol requirement. The
// module accepts BOTH "m<id>v" and "m<id>v\n" and answers either one -- it acts
// on the 'v' byte itself. We drop the newline to work around a GATEWAY-SIDE
// limitation in our half-duplex bus turnaround, as follows:
//
// A module answers a direct version query SYNCHRONOUSLY, the instant it parses
// the 'v' byte -- with zero assembly delay (unlike a dump, which builds its
// EEPROM string before raising DE, so its reply is naturally late enough to be
// safe). Our problem is on the gateway: the ESP32's hardware-managed DE keeps us
// driving the line until the WHOLE frame has clocked out, and our UART receiver
// is off while we transmit, so we can't release the bus and flip to RX fast
// enough to catch that immediate reply. A trailing '\n' keeps us transmitting
// for one extra byte-time (~1 ms at 9600 baud) AFTER the module has already
// started replying -- the reply's leading bytes land while we're still driving
// the line (and deaf), so they're lost and no [RX] frame is ever assembled. That
// was the long-standing "version query gets no reply, but a dump right after
// works" symptom -- a gateway turnaround race, not a module quirk.
//
// Dropping the newline shortens our transmit window so we've already released
// the bus and are listening by the time the module answers (verified on
// hardware: a newline-less send always replies; the byte-identical "m<id>v\n"
// never did). The BROADCAST "m*v\n" path is separate and keeps its newline: a
// wildcard query collects an optional ID range and the module fires a staggered,
// DEFERRED reply on the '\n' (or a 50 ms idle timeout), so there is no
// turnaround race there. sfQueryVersion is only ever called with a concrete id.
//
// (Belt and suspenders: rs485Send() now normalizes framing for EVERY sender, so
// even a hand-typed "m<id>v\n" from the bus monitor or a REST/MQTT raw send is
// shipped bare. This helper still emits the bare form directly to document intent.)
void sfQueryVersion(int addr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "m%dv", addr);   // no '\n' -- gateway turnaround workaround, see note
  rs485SendStr(buf);
}



// Send a text string across a sequence of module IDs starting at startAddr.
// Each character is sent to startAddr, startAddr+1, ... up to strlen(text).
void sfSendText(int startAddr, const char* text, bool blankUnused) {
  // `text` arrives as UTF-8 (from the web UI / MQTT / JSON). Transcode it to the
  // single-byte flap encoding (Windows-1252) first, so one displayed glyph --
  // including a euro sign or an accented letter, which are multi-byte in UTF-8 --
  // maps to exactly one flap module. Unrepresentable code points are dropped.
  // Sized by the longest text a caller may lay across the wall, not by the reel:
  // one flap byte per displayed glyph, however many modules that spans.
  char enc[SF_MAX_TEXT + 1];
  size_t len = utf8ToFlap(text, enc, sizeof(enc));
  for (size_t i = 0; i < len; i++) {
    // sfSendChar uppercases ASCII and rejects non-printable bytes itself; the
    // module firmware maps the character byte to a flap index.
    sfSendChar((int)(startAddr + i), enc[i]);
    delay(10); // inter-message gap to avoid bus collision
  }
  // Optionally blank any previously-set modules beyond the text length
  // (caller passes blankUnused=true when overwriting a display row)
  (void)blankUnused; // extensible for future use
}

// Set Quiet Time on/off.
//
// RISING edge (off -> on): the wall is BLANKED. Each module's current display is
// snapshotted into its pending slot, then one broadcast home drives every reel to
// its blank flap -- the same operation as the Home All button. The snapshot is what
// makes the blanking safe to undo: the falling edge below already replays pending
// requests, so the wall comes back exactly as it was unless the host asked for
// something newer while quiet.
//
// ORDER MATTERS. The home must go out BEFORE gQuietTime is raised: rs485Send
// suppresses display motion ('-', '+' and 'h') while quiet is on, so blanking after
// the flag would suppress the very frame that does the blanking.
//
// FALLING edge (on -> off): the reels are resynced to the last display each module
// was asked to show while quiet -- or, if nothing was asked, to what it was showing
// when quiet began, which is what restores the wall.
//
// The modules here are virtual, but the path is the real one: the home goes out as
// an m*h frame through rs485Send -> vbus -> vmodule, so each reel physically flips
// down to its blank flap on the panel exactly as a real module would.
//
// Safe to call from any task. rs485Send is never called while sfMutex is held:
// sfTrackFromFrame re-takes that mutex and would self-deadlock (see globals.cpp).
void sfSetQuietTime(bool on) {
  bool was = gQuietTime;

  if (!was && on) {
    // Remember what the wall is showing, so turning quiet off can put it back.
    if (xSemaphoreTake(sfMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < sfModuleCount; i++) {
        SFModule& m = sfModules[i];
        if (!m.provisioned) continue;
        if (m.flapChar) {                     // showing a character
          m.pendChar = m.flapChar; m.pendIndex = -1; m.hasPend = true;
        } else if (m.flapIndex > 0) {         // showing a flap by index (0 IS blank)
          m.pendChar = 0; m.pendIndex = m.flapIndex; m.hasPend = true;
        }
        // Otherwise it is already blank, or was never written: nothing to restore.
      }
      xSemaphoreGive(sfMutex);
    }
    sfHome(-1);          // unlocked, and quiet is still off -- so this is not suppressed
    printf("[QUIET] on -- wall blanked (home all)\n");
  }

  gQuietTime = on;       // only now: the blanking frame above had to get out first

  if (was && !on) {
    // Snapshot pending requests under the lock, clear them, then send unlocked
    // (rs485Send must not be called while holding sfMutex).
    struct Pend { int id; char ch; int idx; };
    static Pend list[MAX_MODULES];
    int n = 0;
    if (xSemaphoreTake(sfMutex, portMAX_DELAY) == pdTRUE) {
      for (int i = 0; i < sfModuleCount && n < MAX_MODULES; i++) {
        if (sfModules[i].hasPend) {
          list[n].id  = sfModules[i].id;
          list[n].ch  = sfModules[i].pendChar;
          list[n].idx = sfModules[i].pendIndex;
          n++;
          sfModules[i].hasPend = false;
        }
      }
      xSemaphoreGive(sfMutex);
    }
    for (int i = 0; i < n; i++) {
      if (list[i].ch) sfSendChar(list[i].id, list[i].ch);
      else if (list[i].idx >= 0) sfSendIndex(list[i].id, list[i].idx);
    }
    if (n) printf("[QUIET] off -- resynced %d module(s) to last requested display\n", n);
  }
  printf("[QUIET] Quiet Time %s\n", on ? "ENABLED" : "disabled");
}
/* ----------------------------------------------------------
   Parse responses from modules (called from the RS485 receive task)
   Format examples:
     m38v:12\n           version response
     m38A:...\n          combined all-fields reply (v25+, v31 flap-set tail)
---------------------------------------------------------- */

// Update a module's activity timestamps (millis + RTC epoch for persistence).
static inline void sfTouch(SFModule* m) {
  if (!m) return;
  m->lastSeen = millis();
  unsigned long ep = rtcEpochNow();
  if (ep) m->lastSeenEpoch = ep;
}

// A serial number is 4..20 alphanumeric characters (in practice 20 hex chars
// from the module's chip ID). Bus collisions destroy frame terminators and can
// glue responses together, producing SN tokens containing ':' or raw garbage
// bytes; storing one of those poisons the registry (and FATFS), after which
// every SN-addressed command (mXD<sn>, mXW<sn>, ...) silently fails. Validate
// before EVERY store and on load so corruption can never enter or persist.
static bool sfValidSN(const char* sn) {
  if (!sn || !sn[0]) return false;
  size_t n = strlen(sn);
  if (n < 4 || n > 20) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)sn[i];
    if (!isalnum(c)) return false;
  }
  return true;
}

// Apply the version-bearing fields from a 'v' or 'A' response to the registry.
// Validates the serial (a glued/garbled frame yields a corrupt SN whose storage
// would poison the registry and FATFS); returns false if so, so the caller can
// reject the whole frame. `fwCopy` is the already-normalized firmware string
// ("?" when empty); `sn` may be empty (legacy version-only reply), in which case
// only the firmware is updated. The entry is re-found by id under sfMutex (the
// upsert pointer can be invalidated by concurrent compaction), and any other
// entry holding the same serial is collapsed so a serial maps to one record.
static bool sfApplyVersionFields(uint8_t id, const char* fwCopy, const char* sn) {
  if (sn && sn[0] && !sfValidSN(sn)) return false;
  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* mm = sfFindById(id);
  if (mm) {
    if (strcmp(mm->fwVersion, fwCopy) != 0) sfModulesDirty = true;  // persist new fw
    strlcpy(mm->fwVersion, fwCopy, sizeof(mm->fwVersion));
    // A clean version/all reply parsed for this ID -> the duplicate-ID suspicion
    // (if any) is resolved; reset the heuristic.
    if (sn && sn[0]) {
      strlcpy(mm->serialNum, sn, sizeof(mm->serialNum));
      for (int k = 0; k < sfModuleCount; k++) {
        if (&sfModules[k] != mm && strcmp(sfModules[k].serialNum, sn) == 0) {
          for (int j = k; j < sfModuleCount - 1; j++) sfModules[j] = sfModules[j + 1];
          sfModuleCount--;
          memset(&sfModules[sfModuleCount], 0, sizeof(SFModule));
          sfModulesDirty = true;
          break;
        }
      }
    }
  }
  xSemaphoreGive(sfMutex);
  return true;
}

void sfParseResponse(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return;

  // Convert to null-terminated string for easier parsing.
  // Sized for long inbound frames (a full dump response is ~590 bytes).
  // NOTE: static (not stack) -- sfParseResponse is called only from taskRS485
  // (single caller, no reentrancy), and a 768-byte stack buffer here would
  // overflow that task's 6KB stack. Keeping it in .bss avoids the overflow.
  static char buf[TX_MAX_BYTES + 1];
  size_t copyLen = (len < TX_MAX_BYTES) ? len : TX_MAX_BYTES;
  memcpy(buf, data, copyLen);
  buf[copyLen] = 0;
  // Strip trailing \r\n
  for (int i = (int)copyLen - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
    buf[i] = 0;

  // -- Normal module response: m<id><cmd>:<data>
  // Parse the address first
  const char* p = buf + 1; // skip leading 'm'
  char idStr[8] = {0};
  int  idLen = 0;
  while (*p && isdigit((unsigned char)*p) && idLen < 7) idStr[idLen++] = *p++;
  if (idLen == 0) return;
  uint8_t id = (uint8_t)atoi(idStr);

  xSemaphoreTake(sfMutex, portMAX_DELAY);
  SFModule* m = sfUpsert(id, NULL);
  if (m) { m->provisioned = true; sfTouch(m); }
  xSemaphoreGive(sfMutex);

  if (!m) return;
  char cmd = *p++;

  // Version response: m<id>v:<version>:<moduleId>:<serialNumber>
  // Legacy format m<id>v:<version> also accepted.
  if (cmd == 'v' && *p == ':') {
    char verBuf[64];
    strlcpy(verBuf, p + 1, sizeof(verBuf));
    // Strip trailing whitespace/newlines
    for (int k = (int)strlen(verBuf)-1; k >= 0 && (verBuf[k] == '\n' || verBuf[k] == '\r' || verBuf[k] == ' '); k--)
      verBuf[k] = '\0';
    // Split into up to 3 fields on ':'
    char* field[3] = {nullptr, nullptr, nullptr};
    field[0] = verBuf;
    int fi = 1;
    for (char* cp = verBuf; *cp && fi < 3; cp++) {
      if (*cp == ':') { *cp = '\0'; field[fi++] = cp + 1; }
    }
    int reportedId = (field[1] && field[1][0]) ? atoi(field[1]) : -1;
    char fwCopy[8] = "?";
    if (field[0] && field[0][0]) strlcpy(fwCopy, field[0], sizeof(fwCopy));
    // Validate + write fwVersion/serial under the lock (re-finds the entry by id,
    // collapses any duplicate-serial record). A corrupt SN rejects the frame.
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, field[2])) {
      DBG("[SF] rejecting corrupt version response for module %d (sn:%s)\n",
          id, field[2] ? field[2] : "");
      return;
    }
    DBG("[SF] Module %d fw:%s reportedId:%d sn:%s\n",
                  id, fwCopy, reportedId, field[2] ? field[2] : "");
    char payload[96];
    snprintf(payload, sizeof(payload),
      "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
      id, fwCopy, reportedId, field[2] ? field[2] : "");
    mqttPublishSFEvent("version", payload);
  }
  // Combined all-fields dump (firmware v25+): a single reply carrying everything
  // the 'v' and 'd' responses do, plus autoHome and the live current index:
  //   m<id>A:<version>:<moduleId>:<serialNumber>:<homeOffset>:<totalSteps>:<autoHome>:<curIndex>:<map>
  // We update fwVersion + serialNum exactly like the version response, and
  // reconstruct the "<homeOffset>:<totalSteps>:<map>" dump portion into the same
  // capture slot the 'd' path uses -- so a single 'A' satisfies both a version
  // refresh and a dump read in one bus transaction (see handleApiAll).
  else if (cmd == 'A' && *p == ':') {
    static char aBuf[TX_MAX_BYTES];     // static: taskRS485's 6KB stack can't hold this
    strlcpy(aBuf, p + 1, sizeof(aBuf));
    for (int k = (int)strlen(aBuf)-1; k >= 0 && (aBuf[k]=='\n'||aBuf[k]=='\r'||aBuf[k]==' '); k--) aBuf[k] = 0;
    // The v31+ flap-config tail used to be parsed out of the 'A' reply here and parked in
    // gDump for the module dialog. Both are gone, and so is the reason: the flap set is a
    // single shared reel, identical on every module and known at compile time. Parsing a
    // constant out of a frame to hand it to nobody is work with no reader.
    // Split into the 7 scalar fields plus the trailing map (which has no ':').
    // f: 0 ver, 1 modId, 2 sn, 3 homeOffset, 4 totalSteps, 5 autoHome, 6 curIndex, 7 map
    // The cap is well above the 8 fields we use: because the map is colon-free it
    // still lands cleanly in f[7], and any field a future firmware appends after
    // it falls into f[8+] and is harmlessly ignored -- rather than being glued
    // onto the map (which would corrupt the dump/backup string).
    char* f[16] = {0}; f[0] = aBuf; int fi = 1;
    for (char* cp = aBuf; *cp && fi < 16; cp++) { if (*cp == ':') { *cp = 0; f[fi++] = cp + 1; } }
    char fwCopy[8] = "?";
    if (f[0] && f[0][0]) strlcpy(fwCopy, f[0], sizeof(fwCopy));
    int reportedId = (f[1] && f[1][0]) ? atoi(f[1]) : -1;
    // Validate + write fwVersion/serial under the lock (shared with the 'v' path).
    if (!sfApplyVersionFields((uint8_t)id, fwCopy, f[2])) {
      DBG("[SF] rejecting corrupt all-fields response for module %d (sn:%s)\n", id, f[2] ? f[2] : "");
      return;
    }
    // Reconstruct the dump portion into the shared capture slot if a request is
    // waiting (handleApiAll arms it), so the existing wait machinery works. The
    // 'A'-only extras (autoHome, curIndex, self-reported id) ride along in
    // dedicated globals -- they can't go in the dump string without breaking
    // parseDump, which expects exactly ho:ts:map. All set BEFORE the ready flag.
    // Nothing captures the 'A' reply any more. It used to be parked in gDump for
    // /api/flap/all, which fed the module dialog -- and every field it carried is a
    // COMPILE-TIME CONSTANT here: the version is VM_FW_VERSION, the calibration is nominal,
    // the flap set is the one shared reel. The gateway was querying the emulated bus to be
    // told things it already knew. The reply is still FORMED, because the protocol says it
    // is, and a controller that asks still gets a correct answer -- it is just no longer
    // captured on this side.
    DBG("[SF] Module %d ALL fw:%s reportedId:%d sn:%s ho:%s ts:%s\n",
        id, fwCopy, reportedId, f[2] ? f[2] : "", f[3] ? f[3] : "", f[4] ? f[4] : "");
    // Publish both a version and a dump event, since 'A' carries both.
    char vpl[96];
    snprintf(vpl, sizeof(vpl), "{\"id\":%d,\"ver\":\"%s\",\"reportedId\":%d,\"sn\":\"%s\"}",
             id, fwCopy, reportedId, f[2] ? f[2] : "");
    mqttPublishSFEvent("version", vpl);
    static char dpl[MQTT_BUF_SIZE];
    snprintf(dpl, sizeof(dpl), "{\"id\":%d,\"dump\":\"%s:%s:%s\"}",
             id, f[3] ? f[3] : "", f[4] ? f[4] : "", f[7] ? f[7] : "");
    mqttPublishSFEvent("dump", dpl);
  }
}
