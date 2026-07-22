// web.h -- HTTP server entry point (webInit). Handlers are file-private.
//
// v3.0: the server is ESP-IDF's esp_http_server, spoken natively -- httpx.h carries the
// server lifecycle, the dispatch hook and the small helper set every handler uses.
// Multiple sockets, per-socket timeouts, and the async request support behind
// GET /api/events (sse.h) replaced the one-connection Arduino WebServer.

#ifndef SFGW_WEB_H
#define SFGW_WEB_H

#include "common.h"
#include "httpx.h"   // also declares webInit() / webEnsureListening()

// Serialize the display state -- {"rows":..,"cols":..,"cells":[..]} exactly as
// GET /api/display/state reports it -- into out. Returns bytes written. Takes vmMutex.
// Used by the SSE pump; the REST handler streams the same shape itself.
size_t dispStateJson(char* out, size_t cap);
// Same idea for /api/status -- shared by the REST handler and the SSE `status` event.
size_t statusJson(char* out, size_t cap);
// The canvas stream channel (PUT /api/canvas/stream, v3.2): taskWeb calls the pump
// every tick; while a stream is open it tightens its loop for drain throughput.
void canvasStreamPump();
bool canvasStreamActive();

#endif // SFGW_WEB_H
