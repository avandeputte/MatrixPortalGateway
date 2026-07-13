#include "gateway.h"
#include "vmodule.h"   // vmFlapCharAt: an index-addressed frame still has to track the wall



// rs485.cpp -- the bus layer.
// Owns the single send choke point (rs485Send: strips/re-frames every outbound
// command, enforces the bus-quiet guard and Quiet Time, and logs to the monitor
// ring) and the PSRAM-backed diagnostic ring buffer the web Bus Monitor reads.
// Frame-classification helpers here are file-private. rs485Send is serialized by
// txMutex across tasks.
//
// The only thing that changed from the RS-485 gateway is the last few inches:
// where it wrote bytes to a UART, this hands them to vbusDeliver() and the
// virtual modules parse them. Everything above -- framing, sanitization, the
// display tracking, the monitor ring, the MQTT mirror -- is untouched, which is
// exactly why the gateway above it cannot tell.
// ---- file-private forward declarations ----
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len);
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len);
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr);
static size_t sfKnownCommandLen(const uint8_t* data, size_t len);
static void sfQuietCapturePending(const uint8_t* data, size_t len);
static void sfTrackFromFrame(const uint8_t* data, size_t len);

// Allocate a large buffer in PSRAM (preferred) or internal RAM (fallback),
// zeroed. Logs where it landed. Returns NULL only if both allocations fail.
// NOTE: the panel framebuffer must NOT come from here -- this board's PSRAM is quad
// SPI, far too slow to feed a display. The driver allocates DMA-capable internal RAM
// itself, so this is a note for future callers, not a live hazard.
void* gwPsramAlloc(const char* name, size_t bytes) {
  void* p = NULL;
  if (psramFound()) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (p) {
    printf("[MEM] %s in PSRAM (%u bytes)\n", name, (unsigned)bytes);
  } else {
    p = malloc(bytes);   // fallback: internal RAM
    printf("[MEM] %s in internal RAM (%u bytes)%s\n", name, (unsigned)bytes,
           psramFound() ? " -- PSRAM alloc failed" : " -- no PSRAM");
  }
  if (p) memset(p, 0, bytes);
  return p;
}

// Allocate the large runtime buffers in PSRAM to free internal RAM (which the panel's
// DMA framebuffer and the WiFi/TCP stack both need). Call once from setup() before any
// task or registry init touches these buffers.
void psramAllocInit() {
  logRing   = (GwLogEntry*)gwPsramAlloc("command log",    sizeof(GwLogEntry) * MSG_RING_SIZE);
  mqttQueue = (MqttQItem*)gwPsramAlloc("MQTT queue",      sizeof(MqttQItem) * MQTT_Q_SIZE);
  sfModules = (SFModule*) gwPsramAlloc("module registry", sizeof(SFModule)  * MAX_MODULES);
  txQueue   = (TxQItem*)  gwPsramAlloc("scheduled TX",    sizeof(TxQItem)   * TXQ_SIZE);
}

// Enqueue one frame for paced delivery at dueMs. SPSC ring: taskWeb is the only
// producer, taskRS485 the only consumer, guarded by txQMutex (never held across the
// actual rs485Send, which takes txMutex -- lock order txMutex is never nested under it).
bool rs485SendScheduled(const uint8_t* data, size_t len, uint32_t dueMs) {
  if (!txQueue || !txQMutex) return false;
  if (len == 0 || len > TXQ_FRAME_MAX) return false;   // too long -> caller sends inline
  bool ok = false;
  if (xSemaphoreTake(txQMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  int next = (txQHead + 1) % TXQ_SIZE;
  if (next != txQTail) {                                 // room (one slot kept empty)
    txQueue[txQHead].dueMs = dueMs;
    txQueue[txQHead].len   = (uint16_t)len;
    memcpy(txQueue[txQHead].data, data, len);
    txQHead = next;
    ok = true;
  }
  xSemaphoreGive(txQMutex);
  return ok;                                             // false (full) -> caller inlines
}

// Drain every frame whose due time has arrived, oldest first. Due times within a batch
// are monotonic, so checking the tail is enough. The frame is copied out UNDER the lock
// and sent AFTER releasing it: rs485Send takes txMutex, and holding txQMutex across it
// would stall the producer (taskWeb) for the whole send.
void rs485PollScheduled(uint32_t now) {
  if (!txQueue || !txQMutex) return;
  for (;;) {
    uint8_t buf[TXQ_FRAME_MAX];
    size_t  len = 0;
    if (xSemaphoreTake(txQMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (txQTail != txQHead && (int32_t)(now - txQueue[txQTail].dueMs) >= 0) {
      len = txQueue[txQTail].len;
      memcpy(buf, txQueue[txQTail].data, len);
      txQTail = (txQTail + 1) % TXQ_SIZE;
    }
    xSemaphoreGive(txQMutex);
    if (!len) return;                                    // nothing due
    rs485Send(buf, len, false);
  }
}

void logCommand(char source, const char* text) {
  if (!msgMutex || !logRing) return;
  GwLogEntry e;
  e.timestamp = millis();
  e.source    = source;
  strlcpy(e.text, text ? text : "", sizeof(e.text));
  rtcFormatTime(e.wallTime, sizeof(e.wallTime));
  e.epoch = rtcEpochNow();
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  logRing[logHead] = e;
  logHead = (logHead + 1) % MSG_RING_SIZE;
  xSemaphoreGive(msgMutex);
  DBG("[CMD] %-4s %s\n", source == 'M' ? "MQTT" : "REST", e.text);
}

// Stream the command log to `sink`, one JSON object per call. Never materialises the
// whole array: a String big enough for a full ring would double its buffer mid-copy,
// driving min-free-heap dangerously low.
void logDrainTo(void (*sink)(const char* frag)) {
  if (!msgMutex || !logRing) { sink("[]"); return; }
  xSemaphoreTake(msgMutex, portMAX_DELAY);
  int head = logHead;
  xSemaphoreGive(msgMutex);

  // x3, not x2: flapToJsonUtf8 may expand a high Windows-1252 flap byte into a
  // 3-byte UTF-8 glyph (accents, the euro sign).
  static char esc[LOG_TEXT_MAX * 3 + 1];
  static char obj[LOG_TEXT_MAX * 3 + 128];

  sink("[");
  bool first = true;
  int i = logPollCursor;
  while (i != head) {
    const GwLogEntry& e = logRing[i];
    // Render through the shared helper, exactly as the monitor ring does upstream.
    // The hand-rolled loop this replaces escaped only " and \ -- but a logged frame
    // carries its terminator ("send m00-A\n"), and a RAW NEWLINE inside a JSON string
    // is invalid JSON. The browser's r.json() therefore threw on any poll that caught
    // one, the .catch() swallowed it, and the command log silently stopped updating.
    // flapToJsonUtf8 strips \r\n, escapes " and \, replaces control bytes with the
    // junk char, and emits high Windows-1252 bytes as their real UTF-8 glyph.
    flapToJsonUtf8(e.text, strlen(e.text), esc, sizeof(esc), ' ');
    snprintf(obj, sizeof(obj),
             "%s{\"ts\":%lu,\"ep\":%lu,\"wt\":\"%s\",\"src\":\"%c\",\"text\":\"%s\"}",
             first ? "" : ",", e.timestamp, e.epoch, e.wallTime, e.source, esc);
    sink(obj);
    first = false;
    i = (i + 1) % MSG_RING_SIZE;
  }
  sink("]");
  logPollCursor = head;
}
// The emulated bus has no UART to configure. Kept as the same call site the boot
// sequence and the /api/config/rs485 handler already make, so the baud/parity
// settings continue to round-trip through the UI even though nothing reads them.
void rs485Begin() {
  DBG("[BUS] emulated bus (cfg baud=%lu is cosmetic)\n", cfg.rs485Baud);
}

// Inspect an outbound frame and update per-module display tracking. This runs
// for EVERY transmitted frame (called from rs485Send), so a well-formed display
// command sent through a raw path -- the Bus Monitor "Send Frame" box, the
// /api/rs485/send endpoint, or the MQTT splitflap/send topic -- updates tracking
// exactly like the high-level helpers do. Recognized forms:
//   m<id>-<char>  / m*-<char>   show character (broadcast with '*')
//   m<id>+<idx>   / m*+<idx>    show flap index (char becomes unknown)
//   m<id>h        / m*h         home (char becomes unknown)
// Anything else (by-serial mX..., version/dump/tuning commands, responses)
// is ignored. addr -1 means broadcast. Takes sfMutex internally; never call
// while already holding it.
static void sfTrackFromFrame(const uint8_t* data, size_t len) {
  // Minimum "m" + addr + cmd = 3 chars (e.g. "m*h"). Must start with 'm'.
  if (len < 3 || data[0] != 'm') return;
  // The by-serial frames all use a literal 'X' as the address token (mXH, mXD,
  // mXA, mXW, mXN). Those never change a known module's displayed character, so
  // skip them outright.
  if (data[1] == 'X') return;

  size_t i = 1;
  int addr;
  if (data[i] == '*') {            // broadcast
    addr = -1;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0');
      i++;
      if (v > 254) return;         // out of valid id range -> not a display cmd
    }
    addr = (int)v;
  } else {
    return;                        // not an address we recognize
  }
  if (i >= len) return;
  char cmd = (char)data[i];

  if (cmd == '-') {                // show character: next byte is the char
    if (i + 1 >= len) return;
    char c = (char)data[i + 1];    // ASCII or a Windows-1252 high byte (euro/accents)
    if (!isFlapByte((uint8_t)c)) return;
    // Record for the display wall so it shows every written cell -- provisioned
    // or not, straight from the frame (independent of the module registry).
    if (addr < 0) memset(gWallChars, c, sizeof(gWallChars));       // broadcast
    else if (addr < (int)sizeof(gWallChars)) gWallChars[addr] = c;
    sfTrackChar(addr, c);
  } else if (cmd == '+') {         // show index: record index, char unknown
    long idx = 0;
    size_t j = i + 1;
    if (j >= len || data[j] < '0' || data[j] > '9') return;  // need a number
    while (j < len && data[j] >= '0' && data[j] <= '9') {
      idx = idx * 10 + (data[j] - '0');
      j++;
      if (idx >= SF_MAX_FLAPS) { idx = -1; break; }   // out of flap range -> unknown
    }
    // The index-addressed API (POST /api/display/cells) drives the wall entirely through
    // '+', so the display wall has to be tracked here too -- otherwise everything that API
    // writes is invisible to the Live Preview and to /api/display/state, and only the panel
    // itself would know what it is showing. Resolve the index back to its flap byte.
    //
    // A pictograph flap has NO byte (that is why it is index-addressed in the first place),
    // so it tracks as 0 -- "present, character unknown" -- which the preview renders as '?'.
    // The panel draws the heart correctly regardless; it is only the text mirror that has
    // no way to say "heart" in a byte.
    char fc = (idx >= 0) ? vmFlapCharAt((int)idx) : 0;
    if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++) {
          if (sfModules[k].provisioned) {
            sfModules[k].flapIndex = (int)idx;
            sfModules[k].flapChar  = fc;
          }
        }
        memset(gWallChars, fc, sizeof(gWallChars));
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->flapIndex = (int)idx; m->flapChar = fc; }
        if (addr < (int)sizeof(gWallChars)) gWallChars[addr] = fc;
      }
      xSemaphoreGive(sfMutex);
    }
  } else if (cmd == 'h' &&         // home: char becomes unknown.
             (i + 1 >= len || data[i + 1] == '\n' || data[i + 1] == '\r')) {
    // Guard against false matches: 'h' must be the whole command, not a prefix
    // of something else. (There is no other 'h...' display command, but this
    // keeps the matcher strict.)
    if (addr < 0) memset(gWallChars, 0, sizeof(gWallChars));        // broadcast home
    else if (addr < (int)sizeof(gWallChars)) gWallChars[addr] = 0;  // -> blank cell
    sfTrackChar(addr, 0);
  }
}

// Parse the address + command of an outbound frame for Quiet Time. Returns the
// command char (or 0 if not a normal addressed display frame) and sets *outAddr
// to the module id, or -1 for broadcast ('*'). Mirrors sfTrackFromFrame's
// address parsing. Used only to classify display-motion frames.
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr) {
  *outAddr = -2;
  if (len < 3 || data[0] != 'm') return 0;
  if (data[1] == 'X') return 0;          // by-serial frame
  size_t i = 1;
  int addr;
  if (data[i] == '*') { addr = -1; i++; }
  else if (data[i] >= '0' && data[i] <= '9') {
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
      v = v * 10 + (data[i] - '0'); i++;
      if (v > 254) return 0;
    }
    addr = (int)v;
  } else return 0;
  if (i >= len) return 0;
  *outAddr = addr;
  return (char)data[i];
}

// True if the frame is normal display motion that Quiet Time should suppress:
// show character ('-'), show index ('+'), or home ('h'). Deliberate calibration
// moves (calibrate 'c', goto 'g', nudge 's') are intentionally NOT suppressed,
// since they only originate from an operator actively calibrating.
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  if (cmd == 0) return false;
  if (cmd == '-' || cmd == '+') return true;
  if (cmd == 'h') {
    // 'h' must be the whole command (mXh / m*h), not a prefix of something else.
    size_t i = 1;
    if (data[i] == '*') i++;
    else { while (i < len && data[i] >= '0' && data[i] <= '9') i++; }
    size_t after = i + 1;   // byte after the 'h'
    if (after >= len || data[after] == '\n' || data[after] == '\r') return true;
  }
  return false;
}

// Remember the display the host requested while Quiet Time is on, so the reels
// can resync when it turns off. Only show-char/show-index frames carry display
// intent worth replaying; home is suppressed but not queued.
static void sfQuietCapturePending(const uint8_t* data, size_t len) {
  int addr;
  char cmd = sfFrameCmd(data, len, &addr);
  size_t i = 1; if (data[i]=='*') i++; else { while (i<len && data[i]>='0' && data[i]<='9') i++; }
  if (xSemaphoreTake(sfMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (cmd == '-') {
    if (i + 1 < len) {
      char c = (char)data[i + 1];
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=c; sfModules[k].pendIndex=-1; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=c; m->pendIndex=-1; m->hasPend=true; }
      }
    }
  } else if (cmd == '+') {
    long idx = 0; size_t j = i + 1; bool got=false;
    while (j < len && data[j] >= '0' && data[j] <= '9') { idx = idx*10 + (data[j]-'0'); j++; got=true; if (idx>=SF_MAX_FLAPS){idx=-1;break;} }
    if (got) {
      if (addr < 0) {
        for (int k = 0; k < sfModuleCount; k++)
          if (sfModules[k].provisioned) { sfModules[k].pendChar=0; sfModules[k].pendIndex=(int)idx; sfModules[k].hasPend=true; }
      } else {
        SFModule* m = sfFindById((uint8_t)addr);
        if (m) { m->pendChar=0; m->pendIndex=(int)idx; m->hasPend=true; }
      }
    }
  }
  xSemaphoreGive(sfMutex);
}

// True iff `data[0..len)` (caller strips trailing CR/LF first) is a DIRECT,
// numeric-id firmware-version query "m<id>v" with no payload after the 'v'.
// This is the one frame the gateway must transmit WITHOUT a newline terminator
// (see sfQueryVersion() for the half-duplex turnaround reason). Broadcast "m*v",
// by-serial "mX.." frames, and anything with bytes after the 'v' are NOT matched
// and so keep their terminator.
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len) {
  int addr;
  if (sfFrameCmd(data, len, &addr) != 'v') return false;   // command must be 'v'
  if (addr < 0) return false;                              // numeric id only (reject m*v)
  size_t i = 1;                                            // locate the command char:
  while (i < len && data[i] >= '0' && data[i] <= '9') i++; //   skip 'm', then the digits
  return (i + 1 == len);                                   // 'v' must be the final byte
}

// Given a frame `data[0..len)` (caller strips trailing CR/LF first), return the
// length of the longest prefix that forms a COMPLETE, well-formed known command.
// Bytes beyond that are extraneous and the caller may trim them -- so "m4vDSassa"
// collapses to "m4v" instead of leaning on the module to ignore the junk. This is
// grammar ENFORCEMENT, not guessing: each command's payload shape is fixed by the
// (frozen) protocol. Frames we can't model confidently -- by-serial "mX.." frames
// and any unrecognized command char -- return the full length UNCHANGED, so a
// long restore map or a future command is never truncated. The raw-send bypass
// skips this entirely.
static size_t sfKnownCommandLen(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return len;        // not an m-frame: leave as-is
  if (data[1] == 'X') return len;                   // by-serial frame: pass through untouched
  size_t i = 1;
  bool wildcard = false;
  if (data[i] == '*') {                             // wildcard address
    wildcard = true;
    i++;
  } else if (data[i] >= '0' && data[i] <= '9') {    // numeric id (0..254)
    long v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') { v = v*10 + (data[i]-'0'); i++; }
    if (v > 254) return len;                         // invalid id: don't touch
  } else {
    return len;                                      // malformed address: leave as-is
  }
  if (i >= len) return len;                           // no command char yet: leave as-is
  char cmd = (char)data[i];
  i++;                                                // consume the command char
  switch (cmd) {
    // Zero-payload commands: complete the instant the command char is read.
    case 'h': case 'c': case 'd':
      return i;                                       // trim anything after
    // Version query and combined all-fields dump ('A', v25+): zero-payload when
    // addressed by a numeric id, but a wildcard broadcast may carry an optional
    // "<lo>-<hi>" range (m*v0-49 / m*A0-49) -- pass those through untrimmed.
    case 'v': case 'A':
      return wildcard ? len : i;
    // Show one character: keep exactly one payload byte.
    case '-':
      if (i < len) i++;
      return i;
    // Numeric-payload commands: keep the leading run of digits.
    case '+': case 'o': case 't': case 's':
    case 'g': case 'a': case 'i': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;
      return (d == i) ? len : d;                      // no digits where expected: leave as-is
    }
    // Write calibrated position "<index>:<pos>" -- two numeric fields.
    case 'w': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // index
      if (d == i || d >= len || data[d] != ':') return len;          // malformed: leave as-is
      d++;                                                            // ':'
      size_t p = d;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;        // position
      return (d == p) ? len : d;                                      // no position digits: leave
    }
    default:
      return len;                                     // unknown command: pass through
  }
}

void rs485Send(const uint8_t* data, size_t len, bool raw) {
  if (!len || len > TX_MAX_BYTES) return;

  // --- Wire framing + sanitization (single choke point for every send path) --
  // Normal path: the gateway owns wire correctness so callers never have to:
  //   1) strip any trailing CR/LF the caller supplied,
  //   2) trim anything past a complete, well-formed known command, so a stray
  //      "m4vDSassa" becomes "m4v" rather than relying on the module to ignore
  //      the junk (see sfKnownCommandLen), then
  //   3) re-add exactly one '\n' terminator -- EXCEPT a direct numeric-id version
  //      query "m<id>v", which must ship bare to dodge the half-duplex turnaround
  //      collision (see sfQueryVersion). So "m1v", "m1v\n", "m1v\r\n", and even
  //      "m1vJUNK" all leave as bare "m1v", while "m5-A" and "m9o2832" leave
  //      correctly newline-terminated (which also spares payload commands the
  //      module's 50 ms idle-timeout wait).
  // Raw path (raw==true -- the Bus Monitor "Raw" toggle, or {"raw":true} on the
  // REST/MQTT send): transmit the caller's exact bytes verbatim, with no trim and
  // no terminator change -- a deliberate debugging escape hatch. Bus-collision
  // guarding, Quiet Time, tracking, logging, and the monitor ring still apply.
  size_t bare;
  bool   appendNL;
  bool   sanitized = false;     // true if sfKnownCommandLen trimmed trailing junk
  if (raw) {
    bare     = len;     // verbatim -- no stripping, no sanitizing
    appendNL = false;   // no terminator added
  } else {
    bare = len;
    while (bare > 0 && (data[bare-1] == '\n' || data[bare-1] == '\r')) bare--;
    if (!bare) return;                                  // nothing but terminators
    size_t preSan = bare;
    bare      = sfKnownCommandLen(data, bare);          // trim trailing junk
    sanitized = (bare < preSan);                        // bytes past a complete command were dropped
    appendNL  = !sfIsDirectVersionQuery(data, bare);    // version query => ship bare
  }
  if (!bare) return;

  // Quiet Time: swallow normal display-motion frames so the flaps stay still.
  // The request is acknowledged (we return as if sent) and the desired display
  // is remembered for resync; nothing reaches the bus and tracking is unchanged.
  if (gQuietTime && sfFrameIsDisplayMotion(data, bare)) {
    sfQuietCapturePending(data, bare);
    DBG("[QUIET] suppressed display frame (%u bytes)\n", (unsigned)bare);
    return;
  }
  // Bus-quiet guard: if modules are
  // mid-response (the reply train after a broadcast m*v), hold off until the bus
  // has been quiet for TX_BUS_GUARD_MS, bounded by TX_BUS_WAIT_CAP_MS so we
  // always make progress. On the emulated bus this can no longer corrupt a frame
  // -- replies are queued, not clocked out -- but it keeps command and reply
  // frames from interleaving in the monitor, and it keeps this path identical to
  // the one the RS-485 gateway runs.
  //
  // From here through the monitor-ring push is the critical section: a static scratch
  // buffer, txCount, vbusDeliver's writes to the module array, and the ring push.
  // txMutex serializes it across taskWeb / taskNetwork / taskRS485 so two senders
  // cannot interleave. There are no early returns inside, so the mutex is always
  // released. Lock order is txMutex -> {sfMutex, msgMutex, vmMutex}, never
  // inverted: nothing takes txMutex while holding any of those.
  if (txMutex) xSemaphoreTake(txMutex, portMAX_DELAY);
  {
    unsigned long waitStart = millis();
    while (millis() - gLastRxMs < TX_BUS_GUARD_MS &&
           millis() - waitStart < TX_BUS_WAIT_CAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  // Deliver the finished frame to the virtual modules. They strip any trailing
  // terminator themselves, so `bare` is handed over as-is and `appendNL` only
  // affects what the monitor ring records as the on-wire bytes.
  vbusDeliver(data, bare);
  txCount++;
  // Update per-module display tracking from this frame. Doing it here -- the
  // single point every outbound frame passes through -- means raw sends are
  // tracked exactly like the high-level helpers, with no per-path duplication.
  sfTrackFromFrame(data, bare);
  gDisplayDirty = true;   // HA display sensor refresh (network task, rate-limited)
  // Log the transmitted frame (the command without a trailing terminator, for
  // readability -- the raw path may carry one). The monitor ring below keeps the
  // No [TX] serial line. There is no wire, so 'transmitting' a frame is just a call into
  // vbusDeliver -- and one REST batch would spray 45 of them past the log. The command
  // that produced them is printed once, by logCommand().
  RS485Msg m;
  m.timestamp = millis();
  m.dir = 'T';
  m.origin = 0;
  m.sanitized = sanitized;
  // The frame still goes to the MQTT protocol mirror (<prefix>/tx) -- that surface is
  // about the wire format, which the companion app really does speak. It does NOT go to
  // the web Monitor: 45 identical 'm00-' rows per page are noise, and there is no bus
  // for them to be "transmitted" on. The Monitor logs the command that produced them.
  size_t ringLen = (bare > MSG_MAX_BYTES) ? MSG_MAX_BYTES : bare;
  memcpy(m.data, data, ringLen);
  if (appendNL && ringLen < MSG_MAX_BYTES) m.data[ringLen++] = '\n';
  m.len = ringLen;
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  m.epoch = rtcEpochNow();
  mqttPublishMsg(m);
  if (txMutex) xSemaphoreGive(txMutex);
}

// Send a null-terminated ASCII string on RS485
void rs485SendStr(const char* s) {
  rs485Send((const uint8_t*)s, strlen(s));
}
