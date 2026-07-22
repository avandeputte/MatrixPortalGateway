// sse.h -- Server-Sent Events: the live-preview push stream (GET /api/events), v3.0.
//
// The dashboard's wall preview used to poll /api/display/state every 1.5 s -- up to
// 1.5 s stale, re-downloading unchanged state, blind to everything between polls. The
// firmware knows the instant the wall changes; SSE is the pipe that knowledge was
// missing. One-directional (gateway -> browser) is exactly SSE's shape, and the
// browser's EventSource reconnects on its own -- WebSocket would buy bidirectionality
// nothing here needs.
//
// Division of labour:
//   * sseHandleRequest() runs on the httpd task (via web.cpp's route handler): sends
//     the headers + an immediate display snapshot, then parks the request with
//     httpd_req_async_handler_begin so the socket stays open after the handler returns.
//   * taskWeb (tasks.cpp) is the single push pump: it watches the wall for change and
//     calls sseBroadcastDisplay() / sseKeepalive(). One sender task, so no two writers
//     ever interleave on a socket.

#ifndef SFGW_SSE_H
#define SFGW_SSE_H

#include "common.h"

#include <esp_http_server.h>

void sseInit();                  // create the slot mutex (called from webInit)
esp_err_t sseHandleRequest(httpd_req_t* r);   // GET /api/events handler (httpd task)
int  sseClientCount();           // open streams right now (0..SSE_MAX_CLIENTS)
void sseBroadcastDisplay();      // push the current display state to every stream
void sseBroadcastStatus();       // push the status JSON (the dashboard drops its 3 s poll)
void sseKeepalive();             // ": ka" comment so proxies/timeouts keep the pipe warm
void sseCloseAll();              // drop every stream (before an httpd restart)

#endif // SFGW_SSE_H
