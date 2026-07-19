// frames.h -- frame sanitization, the command log, and the single send API.
//
// This is the choke point every outbound frame passes through. The wire protocol
// is the physical Split-Flap Gateway's (the companion app speaks it verbatim),
// but there is no transceiver here: frameSend() owns frame correctness
// (strip terminators, trim junk past a complete command, re-frame, enforce
// Quiet Time, mirror to MQTT) and hands the finished bytes straight to
// vmDispatch() under vmMutex. Nothing replies -- the query commands went in
// v1.24 -- so frames flow one way: in.

#ifndef MPGW_FRAMES_H
#define MPGW_FRAMES_H

#include "common.h"

/* ----------------------------------------------------------
   Protocol frame passed to the MQTT wire mirror (<prefix>/frames/tx)
---------------------------------------------------------- */
struct FrameMsg {
  unsigned long timestamp;
  uint8_t       data[MSG_MAX_BYTES];
  size_t        len;
  char          wallTime[24];   // gateway-TZ string (MQTT / serial debug)
};

// ---- owned globals (defined in globals.cpp) ----
extern SemaphoreHandle_t txMutex;
extern StaticSemaphore_t txMutexBuf;
// The command log records the COMMANDS the gateway received -- not a frame
// trace. Frames still exist and still matter (the companion POSTs them verbatim
// to /api/frames/send), but logging every frame would spray 45 synthetic 'TX'
// rows plus as many fabricated 'RX' replies per REST batch. One command, one
// line.
#define LOG_TEXT_MAX 128
struct GwLogEntry {
  unsigned long timestamp;      // millis() at capture
  unsigned long epoch;          // UTC epoch (0 = clock not set yet)
  char          wallTime[24];   // gateway-TZ string, for the serial log
  char          source;         // 'R' REST, 'M' MQTT
  char          text[LOG_TEXT_MAX];
};
extern GwLogEntry* logRing;
extern volatile int logHead;
extern volatile int logPollCursor;
extern StaticSemaphore_t msgMutexBuf;
extern SemaphoreHandle_t msgMutex;
extern volatile unsigned long txCount;

// Allocate `bytes` in PSRAM (preferred) or internal RAM, zeroed, logging where it
// landed. Returns NULL only if both fail. Shared by psramAllocInit() and vmInit().
void* gwPsramAlloc(const char* name, size_t bytes);

void psramAllocInit();
// Record one inbound command. source is 'R' (REST) or 'M' (MQTT).
void logCommand(char source, const char* text);
// Stream the log as JSON, one object per sink() call; never builds the whole array.
void logDrainTo(void (*sink)(const char* frag));
void frameSend(const uint8_t* data, size_t len, bool raw = false);
void frameSendStr(const char* s);

// ---- scheduled (paced) outbound frames ----
// /api/frames/batch paces a cascade so the modules receive frames staggered (the
// companion's animation styles rely on it). The pacing is done HERE, not with delay()
// in the web handler: this is a one-connection-at-a-time server, so a handler that
// slept between sends would freeze the whole HTTP server for the batch's duration and
// let concurrent connections pile up in lwIP's accept queue holding TCP window buffers.
//
// Instead the handler stamps each frame with a due time and enqueues it, then returns
// immediately. taskFrames -- which already wakes every 5 ms -- sends each frame when its
// due time arrives. The web server never blocks, and the cascade is unaffected (the 5 ms
// tick quantises step_ms, which is 5..30 ms anyway).
#define TXQ_SIZE       512     // scheduled frames in flight. MUST hold multiple FULL pages
                               // of the LARGEST wall (VM_MAX_MODULES = 192): when the ring
                               // overflows, the fallback sends inline -- which JUMPS THE
                               // QUEUE and lands before still-scheduled earlier frames, so
                               // an older page can overwrite a newer one (observed: 121 of
                               // 160 cells stale on a 32x5 wall whose one page overflowed
                               // the old 128-slot ring). ~28 KB of PSRAM.
#define TXQ_FRAME_MAX  48      // display/index/home frames fit; longer ones send inline
struct TxQItem { uint32_t dueMs; uint16_t len; uint8_t data[TXQ_FRAME_MAX]; };
extern TxQItem* txQueue;                 // PSRAM ring (SPSC: taskWeb in, taskFrames out)
extern volatile int txQHead, txQTail;
extern SemaphoreHandle_t txQMutex;
extern StaticSemaphore_t txQMutexBuf;
// Enqueue one frame for delivery at dueMs (millis timebase). Returns false if it will
// not fit (too long, or queue full) -- caller should then send it inline via frameSend.
bool frameSendScheduled(const uint8_t* data, size_t len, uint32_t dueMs);
// Send every queued frame whose due time has arrived. Called by taskFrames each tick.
void framePollScheduled(uint32_t now);

#endif // MPGW_FRAMES_H
