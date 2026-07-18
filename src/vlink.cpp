#include "gateway.h"

// vlink.cpp -- the seam between the gateway and its virtual modules. See
// vlink.h for what is and isn't modelled.
//
// Two responsibilities: hand outbound frames to the modules (vlinkDeliver), and
// return their replies in module-ID order (vlinkQueue / vlinkPoll). Everything here
// runs on ordinary tasks; there is no ISR and no DMA.

struct VbusReply {
  uint32_t     dueMs;
  uint8_t      mod;
  VmReplyKind  kind;
  bool         used;
};

static VbusReply vlinkQ[VLINK_QUEUE_LEN];

void vlinkBegin() {
  memset(vlinkQ, 0, sizeof(vlinkQ));
  printf("[LINK] frame link up (%d virtual modules)\n", vmCount);
}

void vlinkQueue(uint8_t mod, VmReplyKind kind, uint32_t dueMs) {
  for (int i = 0; i < VLINK_QUEUE_LEN; i++) {
    if (!vlinkQ[i].used) {
      vlinkQ[i].used  = true;
      vlinkQ[i].mod   = mod;
      vlinkQ[i].kind  = kind;
      vlinkQ[i].dueMs = dueMs;
      return;
    }
  }
  // VLINK_QUEUE_LEN exceeds the module ceiling, so a full queue means replies are
  // being produced faster than taskFrames drains them. Count it rather than lose a
  // reply silently: a rising counter here is a real bug signal, and the [WDG]
  // line reports it.
  vlinkDropped = vlinkDropped + 1;
}

void vlinkDeliver(const uint8_t* data, size_t len) {
  if (!vmods || !len) return;
  // Wait for the lock rather than time out: dropping a command here would lose a
  // character off the wall with no error anywhere. The wait is bounded in
  // practice because nothing holds vmMutex across a blocking call (dispRender takes
  // it per record), and nothing takes txMutex while holding it, so this cannot
  // deadlock against the caller.
  if (vmMutex) xSemaphoreTake(vmMutex, portMAX_DELAY);
  vmDispatch(data, len, millis());
  if (vmMutex) xSemaphoreGive(vmMutex);
}

bool vlinkPoll(uint32_t now, uint8_t* out, size_t outSize, size_t* outLen) {
  if (!vmods) return false;
  if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  // Earliest-due wins, so a broadcast reply train comes out in module-ID order
  // even though the queue itself is unordered.
  int best = -1;
  for (int i = 0; i < VLINK_QUEUE_LEN; i++) {
    if (!vlinkQ[i].used) continue;
    if ((int32_t)(now - vlinkQ[i].dueMs) < 0) continue;          // not due yet
    if (best < 0 || (int32_t)(vlinkQ[i].dueMs - vlinkQ[best].dueMs) < 0) best = i;
  }
  size_t n = 0;
  if (best >= 0) {
    n = vmRenderReply(vlinkQ[best].mod, vlinkQ[best].kind, out, outSize);
    vlinkQ[best].used = false;
  }
  if (vmMutex) xSemaphoreGive(vmMutex);

  if (!n) return false;
  *outLen = n;
  return true;
}
