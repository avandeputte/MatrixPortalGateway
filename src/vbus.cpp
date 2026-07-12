#include "gateway.h"

// vbus.cpp -- the emulated module bus. See vbus.h for what is and isn't modelled.
//
// Two responsibilities: hand outbound frames to the modules (vbusDeliver), and
// return their replies in module-ID order (vbusQueue / vbusPoll). Everything here
// runs on ordinary tasks; there is no ISR and no DMA.

struct VbusReply {
  uint32_t     dueMs;
  int32_t      arg;
  uint8_t      mod;
  VmReplyKind  kind;
  bool         used;
};

static VbusReply vbusQ[VBUS_QUEUE_LEN];

void vbusBegin() {
  memset(vbusQ, 0, sizeof(vbusQ));
  printf("[VBUS] emulated bus up (%d virtual modules)\n", vmCount);
}

void vbusQueue(uint8_t mod, VmReplyKind kind, int32_t arg, uint32_t dueMs) {
  for (int i = 0; i < VBUS_QUEUE_LEN; i++) {
    if (!vbusQ[i].used) {
      vbusQ[i].used  = true;
      vbusQ[i].mod   = mod;
      vbusQ[i].kind  = kind;
      vbusQ[i].arg   = arg;
      vbusQ[i].dueMs = dueMs;
      return;
    }
  }
  // VBUS_QUEUE_LEN exceeds the module ceiling, so a full queue means replies are
  // being produced faster than taskBus drains them. Count it rather than lose a
  // reply silently: a rising counter here is a real bug signal, and the [WDG]
  // line reports it.
  vbusDropped++;
}

void vbusDeliver(const uint8_t* data, size_t len) {
  if (!vmods || !len) return;
  // Wait for the lock rather than time out: dropping a command here would lose a
  // character off the wall with no error anywhere. The wait is bounded in
  // practice because nothing holds vmMutex across a blocking call (vmSave takes
  // it per record), and nothing takes txMutex while holding it, so this cannot
  // deadlock against the caller.
  if (vmMutex) xSemaphoreTake(vmMutex, portMAX_DELAY);
  vmDispatch(data, len, millis());
  if (vmMutex) xSemaphoreGive(vmMutex);
}

bool vbusPoll(uint32_t now, uint8_t* out, size_t outSize, size_t* outLen) {
  if (!vmods) return false;
  if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  // Earliest-due wins, so a broadcast reply train comes out in module-ID order
  // even though the queue itself is unordered.
  int best = -1;
  for (int i = 0; i < VBUS_QUEUE_LEN; i++) {
    if (!vbusQ[i].used) continue;
    if ((int32_t)(now - vbusQ[i].dueMs) < 0) continue;          // not due yet
    if (best < 0 || (int32_t)(vbusQ[i].dueMs - vbusQ[best].dueMs) < 0) best = i;
  }
  size_t n = 0;
  if (best >= 0) {
    n = vmRenderReply(vbusQ[best].mod, vbusQ[best].kind, vbusQ[best].arg, out, outSize);
    vbusQ[best].used = false;
  }
  if (vmMutex) xSemaphoreGive(vmMutex);

  if (!n) return false;
  *outLen = n;
  return true;
}
