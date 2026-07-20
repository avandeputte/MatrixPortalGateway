// httpx.h -- the HTTP server (esp_http_server) and the small native helper set every
// handler uses. v3.0 replaced the Arduino WebServer outright: handlers are plain
// esp_http_server handlers (esp_err_t fn(httpd_req_t*)), registered in web.cpp's
// webInit() through httpxOn(), which wraps each one in a dispatch hook that
//   * stamps the web watchdog and the in-flight marker (httpxBusySince) around the call,
//     so a wedged handler still trips loop()'s 120 s stall reboot,
//   * sets the CORS header every REST answer used to add by hand.
//
// What the swap over the old one-connection WebServer buys:
//   * multiple concurrent sockets -- a slow stream no longer starves every other client,
//   * async requests -- the mechanism behind GET /api/events (sse.cpp), which a
//     synchronous server cannot hold open,
//   * IDF-maintained socket lifecycle: per-socket recv/send timeouts and LRU purge.
//
// Handlers still run one at a time (esp_http_server has a single task), so the
// handlers' static scratch buffers stay safe exactly as they were on taskWeb.
//
// Body styles, by helper:
//   * JSON POSTs        -> httpxReadJson() (buffers the body, parses, 400s on failure)
//   * raw binary bodies -> the handler's own httpd_req_recv() loop (linear, local state)
//   * streamed replies  -> httpd_resp_send_chunk via httpxChunk()/httpxChunkEnd()

#ifndef SFGW_HTTPX_H
#define SFGW_HTTPX_H

#include <Arduino.h>
#include <esp_http_server.h>
#include <ArduinoJson.h>

// Start the server (port 80) and register every route; defined in web.cpp.
void webInit();
// Re-start the server if it is down (boot-time httpd_start failure). Called every 20 s
// from taskWeb; true when the server is up.
bool webEnsureListening();

// ---- registration ----
// Register `fn` for exact path `uri` and `method`, wrapped in the dispatch hook.
void httpxOn(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*));
// Prefix route: matches any path beginning with `prefix` (which should end in '/');
// the handler reads the remainder out of r->uri. Exact routes win over prefix routes.
void httpxOnPrefix(const char* prefix, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*));
// The path remainder after `prefix` (query stripped), for prefix-route handlers.
String httpxPathTail(httpd_req_t* r, const char* prefix);
// Start httpd and bind everything registered so far (idempotent).
void httpxStart();
bool httpxUp();

// millis() when the request now being served entered its handler, 0 when idle.
// taskWeb covers the web watchdog only while this is recent (tasks.cpp).
unsigned long httpxBusySince();
// Registered route count (webInit prints it at boot; a drop means HTTPX_MAX_ROUTES is full).
int httpxRouteCount();

// ---- responses ----
// Reply `code` with a preformatted JSON body (CORS is already on the request).
esp_err_t httpxJson(httpd_req_t* r, int code, const char* body);
// Reply `code` with an arbitrary content type; len -1 = strlen(body).
esp_err_t httpxSend(httpd_req_t* r, int code, const char* type, const char* body, int len = -1);
// Reply `code` with {"error":"<msg>"}, JSON-escaping msg (it may carry client text).
esp_err_t httpxErr(httpd_req_t* r, int code, const char* msg);
// Set the numeric status line ("404 Not Found") without sending anything yet.
void      httpxStatus(httpd_req_t* r, int code);
// Chunked streaming: httpxChunk feeds bytes, httpxChunkEnd terminates the response.
esp_err_t httpxChunk(httpd_req_t* r, const char* data, size_t len);
static inline esp_err_t httpxChunkStr(httpd_req_t* r, const char* s) { return httpxChunk(r, s, strlen(s)); }
esp_err_t httpxChunkEnd(httpd_req_t* r);

// ---- request accessors ----
// Query parameter, URL-decoded; empty String when absent.
String    httpxArg(httpd_req_t* r, const char* name);
// Request header value; empty String when absent.
String    httpxHeader(httpd_req_t* r, const char* name);
// Read the whole request body and parse it as JSON into doc. On a missing/oversized/
// malformed body this answers the 400 itself and returns false.
bool      httpxReadJson(httpd_req_t* r, JsonDocument& doc);
// Receive up to `len` bytes of request body into buf; -1 on socket error/timeout.
// Feeds the web watchdog and applies the low-heap backpressure every inbound
// stream needs (the OTA war's lesson, generalized -- see http history in sse.h).
int       httpxRecv(httpd_req_t* r, char* buf, size_t len);

// THE shared body/scratch buffer for every raw-stream handler (canvas, fs, OTA,
// companion blob). One TCP-segment-sized static instead of one per handler: httpd runs
// handlers one at a time, so sharing is safe -- and 1.4 KB stays off the httpd stack.
#define HTTPX_BUF_LEN 1436
extern uint8_t httpxBuf[HTTPX_BUF_LEN];

#endif // SFGW_HTTPX_H
