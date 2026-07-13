// vbus.h -- the emulated module bus.
//
// Sits exactly where the UART used to. rs485Send() frames, sanitizes and logs a
// command as before, then hands the finished bytes to vbusDeliver() instead of
// rs485.write(). The virtual modules parse them and QUEUE their replies; the bus
// task drains that queue and feeds the bytes back through the same accumulator
// the UART fed, so mqttPublishMsg / sfParseResponse are all reached by
// the identical path. Neither the gateway nor a client can tell.
//
// The PROTOCOL is emulated; the wire is not. A real RS-485 bus at 9600 baud is
// half-duplex and slow -- an 'A' reply would take tens of ms to clock out and a
// broadcast m*v across 45 modules several seconds of staggered reply
// slots. None of that is reproduced. Replies are delivered promptly, in module-ID
// order, with only enough spacing (VBUS_SLOT_MS) to keep a broadcast train
// ordered and legible in the bus monitor.
//
// Two consequences worth knowing:
//   * The gateway's collision-avoidance and staggered-broadcast handling still
//     runs, it just never has to wait long. Its ranged-batch polling (m*v0-49)
//     works and is honoured.
//   * Collisions do not happen. Two modules sharing an ID reply one after the
//     other rather than on top of each other, so the gateway's duplicate-ID
//     heuristic -- which keys off garbled serial numbers -- never fires. Every
//     other consequence of a duplicate ID (both modules obeying one command) is
//     reproduced faithfully.

#ifndef MPGW_VBUS_H
#define MPGW_VBUS_H

#include "common.h"

// Replies are queued as an intent, not as text: a broadcast m*A would otherwise
// need one rendered reply per module resident at once. Rendering on the way out
// costs one shared buffer instead.
enum VmReplyKind : uint8_t {
  VR_VER = 1,   // m<id>v:<ver>:<id>:<sn>
  VR_ALL,       // m<id>A:... (combined dump, v31 flap-set tail)
};

// A broadcast (m*v) makes EVERY module answer at once, so this must clear VM_MAX_MODULES
// with room to spare -- otherwise a wall-wide identify silently drops the tail of its own
// replies and the registry comes up short.
#define VBUS_QUEUE_LEN     224   // a broadcast across a full 192-module wall + slack
// Module think-time before an answer. Non-zero for one reason: rs485Send is still
// inside its critical section when vbusDeliver runs, so a reply must not be
// consumed before the command has finished being logged and tracked.
#define VBUS_REPLY_MS        3
// Spacing between the frames of a broadcast reply train. Enough to keep them in
// module-ID order and separately timestamped in the bus monitor; nothing like the
// 100 ms / 700 ms slots a real half-duplex bus needs.
#define VBUS_SLOT_MS         2

// ---- owned globals (defined in globals.cpp) ----
extern volatile unsigned long vbusDropped;   // replies discarded, queue was full

void vbusBegin();

// Gateway -> modules. Called from rs485Send while it holds txMutex, so it must
// not block and must not take txMutex. Takes vmMutex internally.
void vbusDeliver(const uint8_t* data, size_t len);

// Modules -> gateway. Renders the earliest due reply into `out`; false when
// nothing is due. Called only from taskBus.
bool vbusPoll(uint32_t now, uint8_t* out, size_t outSize, size_t* outLen);

// Queue a reply from module `mod` at `dueMs`. Called by vmodule.cpp, already
// holding vmMutex.
void vbusQueue(uint8_t mod, VmReplyKind kind, int32_t arg, uint32_t dueMs);

// Render one queued reply to bytes. Implemented in vmodule.cpp (it needs the
// module's state); called by vbusPoll under vmMutex.
size_t vmRenderReply(uint8_t mod, VmReplyKind kind, int32_t arg,
                     uint8_t* out, size_t outSize);

#endif // MPGW_VBUS_H
