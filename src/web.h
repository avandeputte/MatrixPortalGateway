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

#endif // SFGW_WEB_H
