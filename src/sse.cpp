#include "gateway.h"
#include "sse.h"

// sse.cpp -- see sse.h. Slots + one shared frame buffer, both guarded by sseMutex:
// connections are added on the httpd task while broadcasts run on taskWeb.

#define SSE_MAX_CLIENTS 3
static httpd_req_t*     sseSlot[SSE_MAX_CLIENTS] = {};
static SemaphoreHandle_t sseMutex = NULL;

// Frame buffer for "event: display\ndata: <json>\n\n". The display JSON is ~10 bytes a
// cell; 512 cells is the geometry ceiling, so 8 KB leaves ample slack. PSRAM: this is
// serialized at a few Hz at most, never on a DMA/ISR path.
#define SSE_BUF_CAP 8192
static char* sseBuf = NULL;

void sseInit() {
  if (!sseMutex) sseMutex = xSemaphoreCreateMutex();
  if (!sseBuf) {
    sseBuf = (char*)heap_caps_malloc(SSE_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (!sseBuf) sseBuf = (char*)malloc(SSE_BUF_CAP);   // no PSRAM: internal heap fallback
  }
}

int sseClientCount() {
  int n = 0;
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) if (sseSlot[i]) n++;
  return n;
}

// Drop one stream: hand the socket back to httpd. Called with sseMutex held.
static void sseDropLocked(int i) {
  if (!sseSlot[i]) return;
  httpd_req_async_handler_complete(sseSlot[i]);
  sseSlot[i] = NULL;
}

void sseCloseAll() {
  if (!sseMutex || xSemaphoreTake(sseMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) sseDropLocked(i);
  xSemaphoreGive(sseMutex);
}

// GET /api/events (runs on the httpd task, via httpx dispatch)
esp_err_t sseHandleRequest(httpd_req_t* r) {
  if (!sseBuf || !sseMutex) return httpxErr(r, 503, "events unavailable");

  int slot = -1;
  if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return httpxErr(r, 503, "busy");
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) if (!sseSlot[i]) { slot = i; break; }
  if (slot < 0) {
    xSemaphoreGive(sseMutex);
    return httpxErr(r, 503, "too many event streams");
  }

  httpd_resp_set_type(r, "text/event-stream");
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

  // First frame: a reconnect hint, then an immediate snapshot so the preview paints
  // without waiting for the wall to change.
  int n = snprintf(sseBuf, SSE_BUF_CAP, "retry: 3000\n\nevent: display\ndata: ");
  n += dispStateJson(sseBuf + n, SSE_BUF_CAP - n - 3);
  n += snprintf(sseBuf + n, SSE_BUF_CAP - n, "\n\n");

  if (httpd_resp_send_chunk(r, sseBuf, n) != ESP_OK) {
    xSemaphoreGive(sseMutex);
    return ESP_FAIL;              // client already gone; httpd closes the socket
  }
  httpd_req_t* async = NULL;
  if (httpd_req_async_handler_begin(r, &async) == ESP_OK) {
    sseSlot[slot] = async;        // the socket now belongs to the pump (taskWeb)
    DBG("[SSE] client connected (slot %d, %d open)\n", slot, sseClientCount());
    xSemaphoreGive(sseMutex);
    return ESP_OK;
  }
  // begin() failed: terminate the chunked response normally; EventSource will retry.
  xSemaphoreGive(sseMutex);
  return httpd_resp_send_chunk(r, NULL, 0);
}

// Push one preassembled frame (in sseBuf, length n) to every open stream; drop the
// stream on any send failure. Called with sseMutex held.
static void ssePushLocked(int n) {
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (!sseSlot[i]) continue;
    if (httpd_resp_send_chunk(sseSlot[i], sseBuf, n) != ESP_OK) {
      DBG("[SSE] client dropped (slot %d)\n", i);
      sseDropLocked(i);
    } else {
      // Tell httpd's LRU purge this socket is alive, or a quiet spell of pure pushes
      // (which the purge cannot see) could get the stream reaped under load.
      httpd_sess_update_lru_counter(sseSlot[i]->handle, httpd_req_to_sockfd(sseSlot[i]));
    }
  }
}

void sseBroadcastDisplay() {
  if (!sseMutex || !sseBuf || xSemaphoreTake(sseMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (sseClientCount()) {
    int n = snprintf(sseBuf, SSE_BUF_CAP, "event: display\ndata: ");
    n += dispStateJson(sseBuf + n, SSE_BUF_CAP - n - 3);
    n += snprintf(sseBuf + n, SSE_BUF_CAP - n, "\n\n");
    ssePushLocked(n);
  }
  xSemaphoreGive(sseMutex);
}

void sseKeepalive() {
  if (!sseMutex || !sseBuf || xSemaphoreTake(sseMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (sseClientCount()) {
    int n = snprintf(sseBuf, SSE_BUF_CAP, ": ka\n\n");
    ssePushLocked(n);
  }
  xSemaphoreGive(sseMutex);
}
