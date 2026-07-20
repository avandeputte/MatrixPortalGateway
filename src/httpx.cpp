#include "httpx.h"
#include "common.h"   // wdgWebMs, gOtaInProgress
#include <lwip/sockets.h>

// httpx.cpp -- see httpx.h. The route table is ours (exact-match lookup in the dispatch
// hook) rather than one httpd registration per route: httpd's per-handler bookkeeping is
// sized at compile time, and ~55 routes would neither fit nor need to.

static httpd_handle_t gHttpd = nullptr;

#define HTTPX_MAX_ROUTES 72
struct Route {
  const char*    uri;
  httpd_method_t method;
  esp_err_t    (*fn)(httpd_req_t*);
  bool           prefix;
};
static Route gRoutes[HTTPX_MAX_ROUTES];
static int   gRouteCount = 0;

static volatile unsigned long gBusySince = 0;
unsigned long httpxBusySince() { return gBusySince; }

uint8_t httpxBuf[HTTPX_BUF_LEN];

void httpxOn(const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
  if (gRouteCount >= HTTPX_MAX_ROUTES) {
    printf("[WEB] route table full -- %s dropped\n", uri);
    return;
  }
  gRoutes[gRouteCount++] = {uri, method, fn, false};
}

void httpxOnPrefix(const char* prefix, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
  if (gRouteCount >= HTTPX_MAX_ROUTES) {
    printf("[WEB] route table full -- %s dropped\n", prefix);
    return;
  }
  gRoutes[gRouteCount++] = {prefix, method, fn, true};
}

String httpxPathTail(httpd_req_t* r, const char* prefix) {
  const size_t pl = strlen(prefix);
  const char* q = strchr(r->uri, '?');
  const size_t pathLen = q ? (size_t)(q - r->uri) : strlen(r->uri);
  if (pathLen <= pl) return String();
  String t;
  t.reserve(pathLen - pl);
  for (size_t i = pl; i < pathLen; i++) t += r->uri[i];
  return t;
}

/* ----------------------------------------------------------
   Responses
---------------------------------------------------------- */
static const char* reasonPhrase(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default:  return "";
  }
}

// httpd_resp_set_status stores the POINTER, so the line must outlive the send. One
// static per code in play is overkill; a small rotating pool covers the single httpd
// task's one-request-at-a-time reality.
void httpxStatus(httpd_req_t* r, int code) {
  static char line[32];
  snprintf(line, sizeof(line), "%d %s", code, reasonPhrase(code));
  httpd_resp_set_status(r, line);
}

esp_err_t httpxJson(httpd_req_t* r, int code, const char* body) {
  httpxStatus(r, code);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpxSend(httpd_req_t* r, int code, const char* type, const char* body, int len) {
  httpxStatus(r, code);
  if (type && *type) httpd_resp_set_type(r, type);
  return httpd_resp_send(r, body, len < 0 ? HTTPD_RESP_USE_STRLEN : (size_t)len);
}

esp_err_t httpxErr(httpd_req_t* r, int code, const char* msg) {
  // JSON-escape msg: some callers interpolate client-supplied text (an unknown colour
  // name, for one), and an unescaped quote would make the error body invalid JSON.
  char esc[192];
  size_t o = 0;
  for (const char* p = msg; *p && o < sizeof(esc) - 7; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = (char)c; }
    else if (c < 0x20)         { o += snprintf(esc + o, 7, "\\u%04x", c); }
    else                       { esc[o++] = (char)c; }
  }
  esc[o] = 0;
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
  return httpxJson(r, code, buf);
}

esp_err_t httpxChunk(httpd_req_t* r, const char* data, size_t len) {
  // Emergency-only send pacing. Outbound TX queueing is bounded per socket by
  // TCP_SND_BUF (httpd_resp_send blocks when it is full), so unlike the inbound path
  // this needs no routine throttle -- and pausing here LENGTHENS the stream, giving
  // concurrent inbound bodies more time to pile their receive windows (measured: the
  // deep min-heap dips came from exactly that overlap). Pause only when the heap is
  // genuinely critical, as a last-ditch brake.
  if (ESP.getFreeHeap() < 30000) delay(10);
  return httpd_resp_send_chunk(r, data, len);
}
esp_err_t httpxChunkEnd(httpd_req_t* r) {
  return httpd_resp_send_chunk(r, nullptr, 0);
}

/* ----------------------------------------------------------
   Request accessors
---------------------------------------------------------- */
static void urlDecode(const char* in, size_t len, String& out) {
  out = "";
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    char c = in[i];
    if (c == '+') { out += ' '; continue; }
    if (c == '%' && i + 2 < len + 1 && isxdigit((unsigned char)in[i+1]) && isxdigit((unsigned char)in[i+2])) {
      char h[3] = { in[i+1], in[i+2], 0 };
      out += (char)strtol(h, nullptr, 16);
      i += 2;
      continue;
    }
    out += c;
  }
}

String httpxArg(httpd_req_t* r, const char* name) {
  const char* q = strchr(r->uri, '?');
  if (!q) return String();
  q++;
  size_t nameLen = strlen(name);
  while (*q) {
    const char* amp = strchr(q, '&');
    size_t pairLen  = amp ? (size_t)(amp - q) : strlen(q);
    const char* eq  = (const char*)memchr(q, '=', pairLen);
    size_t kLen     = eq ? (size_t)(eq - q) : pairLen;
    if (kLen == nameLen && strncmp(q, name, nameLen) == 0) {
      String v;
      if (eq) urlDecode(eq + 1, pairLen - kLen - 1, v);
      return v;
    }
    if (!amp) break;
    q = amp + 1;
  }
  return String();
}

String httpxHeader(httpd_req_t* r, const char* name) {
  size_t len = httpd_req_get_hdr_value_len(r, name);
  if (!len || len > 255) return String();
  char buf[256];
  if (httpd_req_get_hdr_value_str(r, name, buf, len + 1) != ESP_OK) return String();
  return String(buf);
}

int httpxRecv(httpd_req_t* r, char* buf, size_t len) {
  int n = httpd_req_recv(r, buf, len);
  if (n > 0) {
    wdgWebMs = millis();               // a long upload must not read as a web stall
    // Heap backpressure for EVERY inbound stream (the OTA handler's v2.2.1 lesson,
    // generalized): a fast sender queues TCP segments in internal RAM faster than the
    // consumer drains them. Slowing the recv loop closes lwIP's receive window and TCP
    // flow control paces the sender. The OTA handler layers its own heavier grading on
    // top (and is exempt here); this floor covers canvas/fs streams, whose 507
    // admission guard checks heap only once, up front.
    // Thresholds are tuned to the v3.0 no-MQTT baseline (~77 KB at rest): they must
    // engage well above the danger zone, because a higher baseline just lets lwIP
    // queue MORE in-flight bytes before anything pushes back (the same lesson as the
    // OTA-war grading, re-learned when the first 2 h soak dipped to 8.5 KB with the
    // old 50/35 K thresholds).
    if (!gOtaInProgress) {
      uint32_t h = ESP.getFreeHeap();
      if      (h < 30000) delay(40);
      else if (h < 45000) delay(20);
      else if (h < 60000) delay(5);
      // Boot-burst guard: on a gateway reboot the companion re-pushes EVERYTHING at
      // once (page + canvas frames, several sockets) while boot bring-up still owns
      // part of the heap -- observed 6 KB watermark from that one overlap. Pace all
      // inbound streams gently for the first 30 s; costs nothing in steady state.
      else if (millis() < 30000UL) delay(5);
    }
  }
  return n;
}

bool httpxReadJson(httpd_req_t* r, JsonDocument& doc) {
  const size_t len = r->content_len;
  if (!len)          { httpxErr(r, 400, "No body");       return false; }
  if (len > 65536)   { httpxErr(r, 413, "Body too large"); return false; }
  char* buf = (char*)malloc(len + 1);
  if (!buf)          { httpxErr(r, 507, "Low memory");     return false; }
  size_t got = 0;
  while (got < len) {
    int n = httpxRecv(r, buf + got, len - got);
    if (n <= 0) { free(buf); httpxErr(r, 400, "Truncated body"); return false; }
    got += (size_t)n;
  }
  buf[len] = 0;
  // const char* input puts ArduinoJson in COPY mode: the doc owns its strings and buf
  // can be freed. (A mutable char* would zero-copy and dangle.)
  DeserializationError e = deserializeJson(doc, (const char*)buf, len);
  free(buf);
  if (e != DeserializationError::Ok) { httpxErr(r, 400, "Bad JSON"); return false; }
  return true;
}

/* ----------------------------------------------------------
   Dispatch
---------------------------------------------------------- */
static esp_err_t dispatch(httpd_req_t* r) {
  gBusySince = millis();
  wdgWebMs   = millis();
  // TCP_NODELAY: httpd writes headers and body separately, and with Nagle on, the
  // second write stalls ~40 ms waiting for the client's delayed ACK -- measured as a
  // hard 38-40 ms floor on every keep-alive request (an ops animation at that floor
  // is a slideshow). The old server never saw it only because it closed the socket
  // after every response. Setting it per-request is redundant but costs microseconds.
  { int fd = httpd_req_to_sockfd(r);
    int one = 1;
    if (fd >= 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
  // Every REST answer carried this by hand in the WebServer era; set it once here.
  httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

  // Exact path (URI minus query) + method lookup in our table.
  const char* q = strchr(r->uri, '?');
  const size_t pathLen = q ? (size_t)(q - r->uri) : strlen(r->uri);
  esp_err_t rc = ESP_OK;
  bool found = false;
  for (int i = 0; i < gRouteCount && !found; i++) {    // pass 1: exact routes
    const Route& rt = gRoutes[i];
    if (!rt.prefix && (int)rt.method == (int)r->method &&
        strlen(rt.uri) == pathLen && strncmp(rt.uri, r->uri, pathLen) == 0) {
      rc = rt.fn(r);
      found = true;
    }
  }
  for (int i = 0; i < gRouteCount && !found; i++) {    // pass 2: prefix routes
    const Route& rt = gRoutes[i];
    const size_t pl = strlen(rt.uri);
    if (rt.prefix && (int)rt.method == (int)r->method &&
        pathLen > pl && strncmp(rt.uri, r->uri, pl) == 0) {
      rc = rt.fn(r);
      found = true;
    }
  }
  if (!found) {
    if (r->method == HTTP_OPTIONS) {
      // CORS preflight for any API path: one wildcard answer instead of the ~25
      // per-route OPTIONS registrations the old server carried.
      httpd_resp_set_hdr(r, "Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
      httpd_resp_set_hdr(r, "Access-Control-Allow-Headers", "Content-Type");
      httpxStatus(r, 204);
      rc = httpd_resp_send(r, "", 0);
    } else {
      rc = httpxErr(r, 404, "not found");
    }
  }

  gBusySince = 0;
  wdgWebMs   = millis();
  return rc;
}

// Match-anything: routing is the exact-match table in dispatch above.
static bool matchAll(const char*, const char*, size_t) { return true; }

void httpxStart() {
  if (gHttpd) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = 80;
  cfg.core_id          = 0;        // where taskWeb always lived: core 1 belongs to WiFi + the panel
  cfg.task_priority    = 2;        // same as the old taskWeb
  cfg.stack_size       = 10240;    // handlers run here now; canvas/OTA paths are the deep ones
  // 4, deliberately tight: dashboard + SSE + companion + one more (curl/OTA). Every OPEN
  // socket is a potential pile of TCP buffers on internal RAM -- a concurrent
  // readback + canvas PUT + log poll was measured stacking ~20 KB, and enough overlap
  // drove the min-heap watermark to 3 KB. Fewer sockets bounds the worst case; the LRU
  // purge below keeps a full house usable by closing the idlest connection.
  cfg.max_open_sockets = 4;
  cfg.max_uri_handlers = 5;        // one catch-all per method is all httpd sees
  cfg.backlog_conn     = 3;        // a deep SYN queue is just more buffers to stack
  cfg.lru_purge_enable = true;     // full house: close the idlest socket instead of refusing
  cfg.recv_wait_timeout = 8;       // a stalled sender aborts its own request, not the server
  cfg.send_wait_timeout = 8;
  cfg.uri_match_fn     = matchAll;
  if (httpd_start(&gHttpd, &cfg) != ESP_OK) {
    gHttpd = nullptr;
    printf("[WEB] httpd_start FAILED\n");
    return;
  }
  static const httpd_method_t METHODS[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_OPTIONS, HTTP_DELETE };
  for (httpd_method_t m : METHODS) {
    httpd_uri_t u = {};
    u.uri     = "*";
    u.method  = m;
    u.handler = dispatch;
    httpd_register_uri_handler(gHttpd, &u);
  }
}

bool httpxUp() { return gHttpd != nullptr; }
