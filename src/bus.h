// bus.h -- frame sanitization, the command log, and the single send API.
//
// This is the choke point every outbound frame passes through. The wire protocol
// is the physical split-flap gateway's (the companion app speaks it verbatim),
// but there is no RS-485 transceiver here: busSend() owns frame correctness
// (strip terminators, trim junk past a complete command, re-frame, enforce
// Quiet Time, mirror to MQTT) and hands the finished bytes to vbusDeliver()
// instead of a UART.

#ifndef MPGW_BUS_H
#define MPGW_BUS_H

#include "common.h"

/* ----------------------------------------------------------
   Protocol frame passed to the MQTT wire mirror
---------------------------------------------------------- */
struct BusMsg {
  unsigned long timestamp;
  char          dir;            // 'T' delivered to the modules, 'R' synthesized module reply
  uint8_t       data[MSG_MAX_BYTES];
  size_t        len;
  char          wallTime[24];   // gateway-TZ string (MQTT / serial debug)
};

// ---- owned globals (defined in globals.cpp) ----
extern SemaphoreHandle_t txMutex;
extern StaticSemaphore_t txMutexBuf;
// The web Monitor is a log of COMMANDS the gateway received -- not a frame trace,
// because there is no bus. Frames still exist and still matter (the companion app
// POSTs them verbatim to /api/bus/send), but logging every frame would spray 45
// synthetic 'TX' rows plus as many fabricated 'RX' replies per REST batch. One
// command, one line.
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
extern volatile unsigned long rxCount;
extern volatile unsigned long txCount;

// Allocate `bytes` in PSRAM (preferred) or internal RAM, zeroed, logging where it
// landed. Returns NULL only if both fail. Shared by psramAllocInit() and vmInit().
void* gwPsramAlloc(const char* name, size_t bytes);

void psramAllocInit();
// Record one inbound command. source is 'R' (REST) or 'M' (MQTT).
void logCommand(char source, const char* text);
// Stream the log as JSON, one object per sink() call; never builds the whole array.
void logDrainTo(void (*sink)(const char* frag));
void busBegin();
void busSend(const uint8_t* data, size_t len, bool raw = false);
void busSendStr(const char* s);

// ---- scheduled (paced) outbound frames ----
// /api/bus/batch paces a cascade so the modules receive frames staggered (the
// companion's animation styles rely on it). The pacing is done HERE, not with delay()
// in the web handler: this is a one-connection-at-a-time server, so a handler that
// slept between sends would freeze the whole HTTP server for the batch's duration and
// let concurrent connections pile up in lwIP's accept queue holding TCP window buffers.
//
// Instead the handler stamps each frame with a due time and enqueues it, then returns
// immediately. taskBus -- which already wakes every 5 ms -- sends each frame when its
// due time arrives. The web server never blocks, and the cascade is unaffected (the 5 ms
// tick quantises step_ms, which is 5..30 ms anyway).
#define TXQ_SIZE       128     // scheduled frames in flight (~3 full 45-frame pages)
#define TXQ_FRAME_MAX  48      // display/index/home frames fit; longer ones send inline
struct TxQItem { uint32_t dueMs; uint16_t len; uint8_t data[TXQ_FRAME_MAX]; };
extern TxQItem* txQueue;                 // PSRAM ring (SPSC: taskWeb in, taskBus out)
extern volatile int txQHead, txQTail;
extern SemaphoreHandle_t txQMutex;
extern StaticSemaphore_t txQMutexBuf;
// Enqueue one frame for delivery at dueMs (millis timebase). Returns false if it will
// not fit (too long, or queue full) -- caller should then send it inline via busSend.
bool busSendScheduled(const uint8_t* data, size_t len, uint32_t dueMs);
// Send every queued frame whose due time has arrived. Called by taskBus each tick.
void busPollScheduled(uint32_t now);

#endif // MPGW_BUS_H
