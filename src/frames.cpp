#include "gateway.h"
#include "vmodule.h"   // vmFlapIndexOf: Quiet Time resolves captured frames to flap indices

// frames.cpp -- the protocol frame layer.
// Owns the single send choke point (frameSend: strips/re-frames every outbound
// command, enforces the reply-quiet guard and Quiet Time, and mirrors the frame
// to MQTT) and the PSRAM-backed command log.
// Frame-classification helpers here are file-private. frameSend is serialized by
// txMutex across tasks.
//
// The wire protocol is unchanged from the physical Split-Flap Gateway -- framing,
// sanitization, the MQTT mirror are all identical, which is exactly why the
// companion app cannot tell. The only difference is the last few inches: where
// the physical gateway wrote bytes to a UART, this hands them to vlinkDeliver()
// and the virtual modules parse them.
// ---- file-private forward declarations ----
static bool sfFrameIsDisplayMotion(const uint8_t* data, size_t len);
static bool sfIsDirectVersionQuery(const uint8_t* data, size_t len);
static char sfFrameCmd(const uint8_t* data, size_t len, int* outAddr);
static size_t sfKnownCommandLen(const uint8_t* data, size_t len);
static void sfQuietCapturePending(const uint8_t* data, size_t len);

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
// task touches these buffers.
void psramAllocInit() {
  logRing   = (GwLogEntry*)gwPsramAlloc("command log",    sizeof(GwLogEntry) * MSG_RING_SIZE);
  mqttQueue = (MqttQItem*)gwPsramAlloc("MQTT queue",      sizeof(MqttQItem) * MQTT_Q_SIZE);
  txQueue   = (TxQItem*)  gwPsramAlloc("scheduled TX",    sizeof(TxQItem)   * TXQ_SIZE);
}

// Enqueue one frame for paced delivery at dueMs. SPSC ring: taskWeb is the only
// producer, taskFrames the only consumer, guarded by txQMutex (never held across the
// actual frameSend, which takes txMutex -- lock order txMutex is never nested under it).
bool frameSendScheduled(const uint8_t* data, size_t len, uint32_t dueMs) {
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
// and sent AFTER releasing it: frameSend takes txMutex, and holding txQMutex across it
// would stall the producer (taskWeb) for the whole send.
void framePollScheduled(uint32_t now) {
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
    frameSend(buf, len, false);
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
    // Render through the shared helper, exactly as the MQTT mirror does.
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
// Parse the address + command of an outbound frame for Quiet Time. Returns the
// command char (or 0 if not a normal addressed display frame) and sets *outAddr
// to the module id, or -1 for broadcast ('*'). Used only to classify display-motion
// frames, so Quiet Time can suppress them.
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
// show character ('-'), show index ('+'), or home ('h'). The queries ('v', 'A')
// pass -- they move nothing -- and anything else is ignored by the modules
// anyway, so there is no motion to suppress.
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
  const char cmd = sfFrameCmd(data, len, &addr);
  size_t i = 1;
  if (data[i] == '*') i++;
  else { while (i < len && data[i] >= '0' && data[i] <= '9') i++; }

  // Resolve the frame to the FLAP the host wants, not to a character.
  //
  // This is the fix. The old capture stored a BYTE, and the restore re-sent it through
  // sfSendChar() -- back through the uppercase fold. "Hello world" came out of a quiet-time
  // cycle as "HELLo worLD", and a pictograph, which has no byte at all, came back as
  // whatever the fallback happened to pick. A byte cannot name a flap on a 237-flap reel.
  // An index can, so resolve once, here, and restore by index.
  int flap = -1;
  if (cmd == '-') {
    if (i + 1 < len) flap = vmFlapIndexOf((char)data[i + 1]);
  } else if (cmd == '+') {
    long idx = 0; size_t j = i + 1; bool got = false;
    while (j < len && data[j] >= '0' && data[j] <= '9') {
      idx = idx * 10 + (data[j] - '0'); j++; got = true;
      if (idx >= SF_MAX_FLAPS) { got = false; break; }
    }
    if (got) flap = (int)idx;
  }
  if (flap < 0) return;          // nothing the reel can show: nothing worth replaying

  if (!vmods || !vmMutex) return;
  if (xSemaphoreTake(vmMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (addr < 0) {
    for (int k = 0; k < vmCount; k++) vmods[k].pendFlap = (int16_t)flap;
  } else {
    for (int k = 0; k < vmCount; k++)
      if (vmods[k].id == (uint8_t)addr) { vmods[k].pendFlap = (int16_t)flap; break; }
  }
  xSemaphoreGive(vmMutex);
}

// True iff `data[0..len)` (caller strips trailing CR/LF first) is a DIRECT,
// numeric-id firmware-version query "m<id>v" with no payload after the 'v'.
// This is the one frame the gateway transmits WITHOUT a newline terminator: a real
// module answers a direct version query the instant it parses the 'v', and on the
// physical half-duplex wire the gateway is still driving the line (and deaf) for one
// more byte-time if a '\n' follows -- so the reply's first bytes are lost. There is
// no such race here, but the frame normaliser keeps the wire format faithful. Broadcast "m*v",
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
// (frozen) protocol. Only the commands this product actually speaks are modelled
// ('-', '+', 'h', 'v', 'A' -- what the companion, the web UI and MQTT emit); the
// physical gateway's calibration/dump family is not, so those frames -- like
// by-serial "mX.." frames and any unrecognized command char -- return the full
// length UNCHANGED and reach the modules untrimmed, which ignore them. The
// raw-send bypass skips this entirely.
static size_t sfKnownCommandLen(const uint8_t* data, size_t len) {
  if (len < 2 || data[0] != 'm') return len;        // not an m-frame: leave as-is
  if (data[1] == 'X') return len;                   // by-serial frame: pass through untouched
  size_t i = 1;
  bool wildcard = false;
  if (data[i] == '*') {                             // wildcard address
    wildcard = true;
    i = i + 1;
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
    // Home: zero payload, complete the instant the command char is read.
    case 'h':
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
    // Show flap index: keep the leading run of digits.
    case '+': {
      size_t d = i;
      while (d < len && data[d] >= '0' && data[d] <= '9') d++;
      return (d == i) ? len : d;                      // no digits where expected: leave as-is
    }
    default:
      return len;                                     // unknown command: pass through
  }
}

void frameSend(const uint8_t* data, size_t len, bool raw) {
  if (!len || len > TX_MAX_BYTES) return;

  // --- Wire framing + sanitization (single choke point for every send path) --
  // Normal path: the gateway owns wire correctness so callers never have to:
  //   1) strip any trailing CR/LF the caller supplied,
  //   2) trim anything past a complete, well-formed known command, so a stray
  //      "m4vDSassa" becomes "m4v" rather than relying on the module to ignore
  //      the junk (see sfKnownCommandLen), then
  //   3) re-add exactly one '\n' terminator -- EXCEPT a direct numeric-id version
  //      query "m<id>v", which must ship bare to dodge the half-duplex turnaround
  //      collision (see sfIsDirectVersionQuery). So "m1v", "m1v\n", "m1v\r\n", and even
  //      "m1vJUNK" all leave as bare "m1v", while "m5-A" and "m9o2832" leave
  //      correctly newline-terminated (which also spares payload commands the
  //      module's 50 ms idle-timeout wait).
  // Raw path (raw==true -- the Monitor "Raw" toggle, or {"raw":true} on the
  // REST/MQTT send): transmit the caller's exact bytes verbatim, with no trim and
  // no terminator change -- a deliberate debugging escape hatch. The reply-quiet
  // guard, Quiet Time, tracking, and the MQTT mirror still apply.
  size_t bare;
  bool   appendNL;
  if (raw) {
    bare     = len;     // verbatim -- no stripping, no sanitizing
    appendNL = false;   // no terminator added
  } else {
    bare = len;
    while (bare > 0 && (data[bare-1] == '\n' || data[bare-1] == '\r')) bare--;
    if (!bare) return;                                  // nothing but terminators
    bare      = sfKnownCommandLen(data, bare);          // trim trailing junk
    appendNL  = !sfIsDirectVersionQuery(data, bare);    // version query => ship bare
  }
  if (!bare) return;

  // Quiet Time: swallow normal display-motion frames so the flaps stay still.
  // The request is acknowledged (we return as if sent) and the desired display
  // is remembered for resync; nothing reaches the modules and tracking is unchanged.
  if (gQuietTime && sfFrameIsDisplayMotion(data, bare)) {
    sfQuietCapturePending(data, bare);
    DBG("[QUIET] suppressed display frame (%u bytes)\n", (unsigned)bare);
    return;
  }
  // Reply-quiet guard: if modules are
  // mid-response (the reply train after a broadcast m*v), hold off until replies
  // have been quiet for TX_REPLY_GUARD_MS, bounded by TX_REPLY_WAIT_CAP_MS so we
  // always make progress. Nothing can be corrupted here
  // -- replies are queued, not clocked out -- but it keeps command and reply
  // frames from interleaving in the MQTT mirror, and it keeps this path identical
  // to the one the physical gateway runs.
  //
  // From here through mqttPublishMsg is the critical section: a static scratch
  // buffer, txCount, and vlinkDeliver's writes to the module array. txMutex
  // serializes it across taskWeb / taskNetwork / taskFrames so two senders cannot
  // interleave. There are no early returns inside, so the mutex is always
  // released. Lock order is txMutex -> {msgMutex, vmMutex}, never inverted: nothing takes
  // txMutex while holding either of those. vlinkDeliver takes vmMutex from in here, which is
  // exactly why no caller may hold vmMutex across frameSend.
  if (txMutex) xSemaphoreTake(txMutex, portMAX_DELAY);
  {
    unsigned long waitStart = millis();
    while (millis() - gLastRxMs < TX_REPLY_GUARD_MS &&
           millis() - waitStart < TX_REPLY_WAIT_CAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  // Deliver the finished frame to the virtual modules. They strip any trailing
  // terminator themselves, so `bare` is handed over as-is and `appendNL` only
  // affects what the MQTT mirror records as the on-wire bytes.
  vlinkDeliver(data, bare);
  txCount = txCount + 1;
  gDisplayDirty = true;   // HA display sensor refresh (network task, rate-limited)
  // Mirror the frame to MQTT (<prefix>/frames/tx) -- that surface is about the wire
  // format, which the companion app really does speak. It does NOT go to the web
  // Monitor: 45 identical 'm00-' rows per page are noise, and there is no wire
  // for them to be "transmitted" on. The command that produced them is logged
  // once, by logCommand().
  FrameMsg m;
  m.timestamp = millis();
  m.dir = 'T';
  size_t ringLen = (bare > MSG_MAX_BYTES) ? MSG_MAX_BYTES : bare;
  memcpy(m.data, data, ringLen);
  if (appendNL && ringLen < MSG_MAX_BYTES) m.data[ringLen++] = '\n';
  m.len = ringLen;
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  mqttPublishMsg(m);
  if (txMutex) xSemaphoreGive(txMutex);
}

// Send a null-terminated ASCII command string through frameSend.
void frameSendStr(const char* s) {
  frameSend((const uint8_t*)s, strlen(s));
}
