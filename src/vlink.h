// vlink.h -- the seam between the gateway and its virtual modules.
//
// Sits exactly where the UART used to. frameSend() frames and sanitizes a
// command as before, then hands the finished bytes to vlinkDeliver() instead of
// a UART write. The virtual modules parse them and QUEUE their replies; the
// frame task drains that queue and feeds the bytes back through the same
// accumulator the UART fed, so mqttPublishMsg is reached by the identical path.
// Neither the gateway nor a client can tell.
//
// The PROTOCOL is emulated; the wire is not. The physical gateway's half-duplex
// serial link at 9600 baud is slow -- an 'A' reply takes tens of ms to clock out
// and a broadcast m*v across 45 modules several seconds of staggered reply
// slots. None of that is reproduced. Replies are delivered promptly, in
// module-ID order, with only enough spacing (VLINK_SLOT_MS) to keep a broadcast
// train ordered and legible in the MQTT mirror.
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

#ifndef MPGW_VLINK_H
#define MPGW_VLINK_H

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
// replies.
#define VLINK_QUEUE_LEN     224   // a broadcast across a full 192-module wall + slack
// Module think-time before an answer. Non-zero for one reason: frameSend is still
// inside its critical section when vlinkDeliver runs, so a reply must not be
// consumed before the command has finished being mirrored and tracked.
#define VLINK_REPLY_MS        3
// Spacing between the frames of a broadcast reply train. Enough to keep them in
// module-ID order and separately timestamped in the MQTT mirror; nothing like the
// 100 ms / 700 ms slots the physical half-duplex wire needs.
#define VLINK_SLOT_MS         2

// ---- owned globals (defined in globals.cpp) ----
extern volatile unsigned long vlinkDropped;   // replies discarded, queue was full

void vlinkBegin();

// Gateway -> modules. Called from frameSend while it holds txMutex, so it must
// not block and must not take txMutex. Takes vmMutex internally.
void vlinkDeliver(const uint8_t* data, size_t len);

// Modules -> gateway. Renders the earliest due reply into `out`; false when
// nothing is due. Called only from taskFrames.
bool vlinkPoll(uint32_t now, uint8_t* out, size_t outSize, size_t* outLen);

// Queue a reply from module `mod` at `dueMs`. Called by vmodule.cpp, already
// holding vmMutex.
void vlinkQueue(uint8_t mod, VmReplyKind kind, uint32_t dueMs);

// Render one queued reply to bytes. Implemented in vmodule.cpp (it needs the
// module's state); called by vlinkPoll under vmMutex.
size_t vmRenderReply(uint8_t mod, VmReplyKind kind, uint8_t* out, size_t outSize);

#endif // MPGW_VLINK_H
