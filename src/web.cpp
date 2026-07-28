#include "gateway.h"
#include "panel.h"   // panelSetColourOrder: a BGR panel is a runtime fact, not a build one
#include "effects.h"
#include "canvas.h"
#include "sse.h"     // GET /api/events: the live-preview push stream (v3.0)
#include "web_ui.h"
#include <mbedtls/base64.h>   // the canvas "image" op decodes a base64 sprite
#include "audio.h"           // capabilities audio token + effect "audio" param (v3.4)
#include "sound.h"           // POST /api/sound + the sound capability token (v3.6)
#include "sensor.h"          // env fields in status + the environment token (v3.7)
#include <fcntl.h>            // non-blocking mode for the canvas stream socket (v3.2)
#include <lwip/sockets.h>    // setsockopt on the stream socket at close (v3.3)



// web.cpp -- HTTP server: the dashboard page and the REST API.
// Runs entirely on taskWeb (one request at a time). handleRoot streams the
// static dashboard from web_ui.h; each handleApi* function serves one REST
// route (the HTTP method + path is noted above each, and registered in
// webInit). Handlers are static -- only webInit is exported.
// ---- file-private forward declarations ----
static esp_err_t handleApiChar(httpd_req_t* r);
static esp_err_t handleApiConfigGet(httpd_req_t* r);
static esp_err_t handleApiConfigSettings(httpd_req_t* r);
static esp_err_t handleApiConfigWifi(httpd_req_t* r);
static esp_err_t handleApiDisplayState(httpd_req_t* r);
static esp_err_t handleApiDisplayBrightness(httpd_req_t* r);
static esp_err_t handleApiFsList(httpd_req_t* r);
static esp_err_t handleApiFsFile(httpd_req_t* r);
static esp_err_t handleApiFsDelete(httpd_req_t* r);
static esp_err_t handleFsUpload(httpd_req_t* r);
static esp_err_t handleApiHome(httpd_req_t* r);
static esp_err_t handleApiIndex(httpd_req_t* r);
static esp_err_t handleApiMessages(httpd_req_t* r);
static esp_err_t handleApiModules(httpd_req_t* r);
static esp_err_t handleApiQuiet(httpd_req_t* r);
static esp_err_t handleApiQuietSchedule(httpd_req_t* r);
static esp_err_t handleApiCompanion(httpd_req_t* r);
static esp_err_t handleApiCompanionSettingsGet(httpd_req_t* r);
static esp_err_t handleApiCompanionSettingsPut(httpd_req_t* r);
static esp_err_t handleApiSend(httpd_req_t* r);
static esp_err_t handleApiSendBatch(httpd_req_t* r);
static esp_err_t handleApiDisplayCells(httpd_req_t* r);
static esp_err_t handleApiCapabilities(httpd_req_t* r);
static esp_err_t handleApiStatus(httpd_req_t* r);
static esp_err_t handleApiText(httpd_req_t* r);
static esp_err_t handleFavicon(httpd_req_t* r);
static esp_err_t handleLogo(httpd_req_t* r);
static esp_err_t handleRoot(httpd_req_t* r);
/* ----------------------------------------------------------
   Web server
---------------------------------------------------------- */
// One build-time ETag for every immutable asset (the page, favicon, logo, /lang
// dictionaries): they all live in the firmware image, so a reflash -- which
// changes __TIME__ -- is exactly when they change.
static const char BUILD_ETAG[] = "\"" __DATE__ "-" __TIME__ "\"";

// The request currently being streamed to, for the C-function sink callbacks
// (logDrainTo, canvasAnimList/FontList) that predate a request argument. Safe as a
// single static: esp_http_server dispatches one request at a time.
static httpd_req_t* gStreamReq = nullptr;


// -- GET /  (main dashboard)
// Browser tab icon (favicon): a split-flap tile -- two flaps, the signature
// horizontal seam with axle pivots, and a character bisected by it. Served at
// /favicon.svg and linked from each page <head>. SVG keeps it crisp at any size
// with no binary blob; single-quoted attributes let it sit in a plain C string.
const char FAVICON_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64' role='img' aria-label='Split-Flap Gateway'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient><clipPath id='sfTile'><rect x='7' y='7' width='50' height='50' rx='10'/></clipPath></defs><rect x='7' y='8.5' width='50' height='50' rx='10' fill='#000' opacity='0.35'/><g clip-path='url(#sfTile)'><rect x='7' y='7' width='50' height='25' fill='url(#sfTop)'/><rect x='7' y='32' width='50' height='25' fill='url(#sfBot)'/><text x='32' y='46' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='40' fill='#f3eee3'>S</text><rect x='7' y='30.9' width='50' height='2.2' fill='#0c0d10'/><rect x='7' y='33.1' width='50' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='4.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='55.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='7' y='7' width='50' height='50' rx='10' fill='none' stroke='#0a0b0d' stroke-width='1'/></svg>";
// GET /favicon.svg
static esp_err_t handleFavicon(httpd_req_t* r) {
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) { return httpxSend(r, 304, "image/svg+xml", ""); }
  return httpxSend(r, 200, "image/svg+xml", FAVICON_SVG);
}

// Web UI wordmark: the app name on a split-flap board -- the same two-flap
// tiles, seam and pivots as the favicon, one cell per character. The dashboard
// itself draws its header in CSS; /logo.svg is served for the COMPANION, whose
// gwproxy fetches it to brand its gateway page.
const char LOGO_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 44' role='img' aria-label='Split-Flap'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient></defs><rect x='3' y='2' width='250' height='40' rx='6' fill='#000' opacity='0.30'/><clipPath id='clip1'><rect x='3' y='0' width='250' height='40' rx='6'/></clipPath><g clip-path='url(#clip1)'><rect x='3' y='0' width='250' height='20' fill='url(#sfTop)'/><rect x='3' y='20' width='250' height='20' fill='url(#sfBot)'/><rect x='27.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='28.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='52.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='53.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='77.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='78.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='102.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='103.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='127.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='128.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='152.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='153.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='177.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='178.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='202.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='203.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='227.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='228.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><text x='15.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>S</text><text x='40.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><text x='65.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='90.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>I</text><text x='115.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>T</text><text x='140.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>-</text><text x='165.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>F</text><text x='190.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='215.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='240.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><rect x='3' y='18.9' width='250' height='2.2' fill='#0c0d10'/><rect x='3' y='21.1' width='250' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='1.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='251.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='3' y='0' width='250' height='40' rx='6' fill='none' stroke='#0a0b0d' stroke-width='0.8'/></svg>";
// GET /logo.svg
static esp_err_t handleLogo(httpd_req_t* r) {
  // ETag = build time, revalidated every request (like the page and /lang). A plain 7-day
  // max-age with NO validator was a bug: when the logo changed, the browser kept serving its
  // OLD cached copy for a week -- the header text updated but the wordmark did not.
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) { return httpxSend(r, 304, "image/svg+xml", ""); }
  return httpxSend(r, 200, "image/svg+xml", LOGO_SVG);
}

// GET /openapi.yaml (v3.4) -- the device serves its own API contract, gzipped at
// build time (~15 KB on the wire). Sent with Content-Encoding: gzip unconditionally:
// the plain text is not stored, and every HTTP client of the last two decades
// decompresses it. Same build-stamp ETag discipline as the other immutable assets.
// Advertised in capabilities ("openapi") and via /.well-known/api-catalog (RFC 9727).
static esp_err_t handleOpenapiSpec(httpd_req_t* r) {
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) return httpxSend(r, 304, "application/yaml", "");
  httpd_resp_set_type(r, "application/yaml");
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  for (size_t off = 0; off < sizeof(OPENAPI_YAML_GZ); off += 4096) {
    const size_t c = (sizeof(OPENAPI_YAML_GZ) - off < 4096) ? sizeof(OPENAPI_YAML_GZ) - off : 4096;
    httpxChunk(r, (const char*)OPENAPI_YAML_GZ + off, c);
    wdgWebMs = millis();
  }
  return httpxChunkEnd(r);
}

// GET /.well-known/api-catalog (RFC 9727): the standard discovery pointer to the above.
static esp_err_t handleApiCatalog(httpd_req_t* r) {
  return httpxSend(r, 200, "application/linkset+json",
      "{\"linkset\":[{\"anchor\":\"/\","
      "\"service-desc\":[{\"href\":\"/openapi.yaml\",\"type\":\"application/yaml\"}]}]}");
}

// Stream a byte range of the static page in watchdog-friendly chunks so a slow
// client can't trip the stall detector mid-send.
static void streamPage(httpd_req_t* r, const char* p, size_t n) {
  const size_t CHUNK = 1024;
  for (size_t off = 0; off < n; off += CHUNK) {
    size_t c = (n - off < CHUNK) ? (n - off) : CHUNK;
    httpxChunk(r, p + off, c);
    wdgWebMs = millis();
  }
}

// GET /lang/<code>  -- one UI translation dictionary (v1.1)
//
// The dashboard's English is the text already in PAGE_HTML, so English costs nothing and
// needs no request. Every other language is a gzipped JSON dict generated into web_ui.h by
// tools/build_ui.py, and the browser fetches only the one it needs.
//
// Content-Encoding: gzip is correct HERE (and wrong for /api/companion/settings): these
// bytes are a *transfer encoding* of JSON that the browser transparently inflates before
// the page's fetch().json() ever sees it. The companion blob is the opposite -- there the
// gzip IS the payload, which is why that endpoint must not claim the header.
//
// One route is registered per language in webInit(), so an unknown code simply 404s;
// the handler reads the code back out of the URI it was matched on.
static esp_err_t handleLang(httpd_req_t* r) {
  size_t idx = 0;
  for (; idx < UI_LANG_COUNT; idx++) {
    const size_t cl = strlen(UI_LANGS[idx].code);
    if (strncmp(r->uri + 6, UI_LANGS[idx].code, cl) == 0 &&
        (r->uri[6 + cl] == 0 || r->uri[6 + cl] == '?')) break;   // 6 = strlen("/lang/")
  }
  if (idx >= UI_LANG_COUNT) return httpxErr(r, 404, "not found");   // unreachable: routes are exact
  const UiLang& L = UI_LANGS[idx];
  wdgWebMs = millis();
  // Dictionaries live in the firmware image, so they change only on reflash -- the same
  // ETag as the page busts them at exactly the right moment.
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) { return httpxSend(r, 304, "application/json", ""); }
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  httpd_resp_set_type(r, "application/json");
  httpxChunk(r, (PGM_P)L.gz, L.len);
  wdgWebMs = millis();
  return httpxChunkEnd(r);
}

// GET /
static esp_err_t handleRoot(httpd_req_t* r) {
  wdgWebMs = millis();
  // The page is baked into the firmware, so its bytes change only when the firmware
  // is rebuilt -- and every rebuild changes __TIME__. Serve it with that as an ETag
  // and honour If-None-Match: navigating away and back then costs a tiny 304.
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");   // revalidate, but cheaply (304)
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) { return httpxSend(r, 304, "text/html", ""); }
  httpd_resp_set_type(r, "text/html");
  // Pre-gzipped whole (v3.2, ~4x smaller; the version footer is client-rendered from
  // /api/config, so nothing needs substituting server-side any more). Plain fallback
  // for clients that don't accept gzip.
  if (httpxHeader(r, "Accept-Encoding").indexOf("gzip") >= 0) {
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    for (size_t off = 0; off < sizeof(PAGE_HTML_GZ); off += 4096) {
      size_t c = (sizeof(PAGE_HTML_GZ) - off < 4096) ? (sizeof(PAGE_HTML_GZ) - off) : 4096;
      httpxChunk(r, (const char*)(PAGE_HTML_GZ + off), c);
      wdgWebMs = millis();
    }
    return httpxChunkEnd(r);
  }
  streamPage(r, PAGE_HTML, sizeof(PAGE_HTML) - 1);
  return httpxChunkEnd(r);
}

// GET /api/log -- the command log, newest entries since last poll
static void logSink(const char* frag) { httpxChunkStr(gStreamReq, frag); wdgWebMs = millis(); }
static esp_err_t handleApiMessages(httpd_req_t* r) {
  // Chunked: streamed one entry at a time, never one contiguous allocation.
  httpd_resp_set_type(r, "application/json");
  gStreamReq = r;
  logDrainTo(logSink);
  return httpxChunkEnd(r);   // terminate the chunked response
}

// POST /api/frames/send -- send one raw protocol frame to the virtual modules.
// (The physical gateway's /api/rs485/* paths, kept as aliases through v1.21, were
// dropped in v1.22; the companion must use /api/frames/*.)
static esp_err_t handleApiSend(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* d = doc["data"] | "";
  bool raw = doc["raw"] | false;   // optional: send verbatim, bypassing sanitization
  uint8_t outBuf[TX_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
  memcpy(outBuf, d, outLen);
  if (!outLen) { httpxErr(r, 400, "Empty data"); return ESP_OK; }
  { char cd[LOG_TEXT_MAX]; snprintf(cd, sizeof(cd), "send %s", d); logCommand('R', cd); }
  frameSend(outBuf, outLen, raw);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%zu,\"raw\":%s}", outLen, raw ? "true" : "false");
  return httpxSend(r, 200, "application/json", resp);
}

// POST /api/frames/batch -- send many frames in one request.
// Body: {"frames":["m00-A\n","m01-B\n",...], "step_ms":15}. Each frame is sent
// normalized (like /api/frames/send); an optional step_ms paces the cascade
// device-side. Lets the companion draw a whole animated page in ONE HTTP call
// instead of one request per module. Caps keep the request bounded and the web
// watchdog fed.
static esp_err_t handleApiSendBatch(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  JsonArray frames = doc["frames"].as<JsonArray>();
  if (frames.isNull()) { httpxErr(r, 400, "'frames' array required"); return ESP_OK; }
  int step = doc["step_ms"] | 0;
  if (step < 0)  step = 0;
  if (step > 30) step = 30;          // keep per-frame pacing small
  // One 'REST' row marking the batch, just above the TX frames it produces.
  { char cd[64]; snprintf(cd, sizeof(cd), "batch %u frames, step=%dms",
      (unsigned)frames.size(), step); logCommand('R', cd); }
  int sent = 0;
  uint32_t now  = millis();
  uint32_t due  = now;               // frame i is due at now + i*step
  for (JsonVariant v : frames) {
    if (sent >= 512) break;          // bound the batch
    const char* f = v.as<const char*>();
    if (!f || !*f) continue;
    uint8_t outBuf[TX_MAX_BYTES];
    size_t outLen = min(strlen(f), (size_t)TX_MAX_BYTES);
    memcpy(outBuf, f, outLen);
    // Pace by SCHEDULING, never by delay(): this handler runs on taskWeb, and blocking
    // it freezes the one-connection HTTP server and piles up concurrent sockets (their
    // TCP window buffers stack in internal RAM). taskFrames sends each frame when due.
    // step==0 or a frame too long / a full queue falls back to an immediate send.
    if (step > 0 && frameSendScheduled(outBuf, outLen, due)) {
      due += (uint32_t)step;
    } else {
      frameSend(outBuf, outLen, false);
    }
    sent++;
    wdgWebMs = millis();             // this loop is now fast, but stay watchdog-safe
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"sent\":%d}", sent);
  return httpxSend(r, 200, "application/json", resp);
}


/* POST /api/display/cells  -- the index-addressed display API (v1.6)
 *
 * WHY THIS EXISTS. The legacy protocol sets a flap by CHARACTER: m<id>-<char>, one byte.
 * Two things are therefore impossible on it, and no amount of care fixes either:
 *
 *   * LOWERCASE. The byte for 'r' already means RED -- the seven colour flaps are addressed
 *     by r o y g b p w, by protocol. So a lowercase letter must fold to uppercase, or the
 *     colours break. You cannot have both on one byte.
 *   * PICTOGRAPHS. A heart has no Windows-1252 byte at all, so there is no byte to send.
 *
 * Both flaps EXIST on the reel (163..222 lowercase, 223..236 pictographs). They are simply
 * unreachable by character. This endpoint addresses them by INDEX -- m<id>+<n> -- which the
 * modules have always understood, and names colours explicitly instead of stealing letters
 * for them. That is the whole design: a different way in, not a different reel.
 *
 * Body:
 *   { "start": 0, "step_ms": 15, "cells": [ ... ] }
 *
 *   start     first module id the cells land on (default 0)
 *   step_ms   0..30, paces the cascade (scheduled, never a delay() -- see the batch API)
 *   cells     one entry per module, left to right. Each is exactly one of:
 *               {"ch":"e"}        any character -- lowercase and accents kept as typed
 *               {"ch":"♥"}   a pictograph, by character
 *               {"color":"red"}   a colour flap, NAMED: red orange yellow green blue
 *                                 purple white
 *               {"blank":true}    home the module (flap 0)
 *               {"skip":true}     leave that module alone
 *
 * A cell whose character has no flap is REJECTED, not silently blanked: a request that
 * cannot be shown is a bug in the caller, and swallowing it would show a hole in a wall of
 * text with nothing to explain it.
 */
static esp_err_t handleApiDisplayCells(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  JsonArray cells = doc["cells"].as<JsonArray>();
  if (cells.isNull()) { httpxErr(r, 400, "'cells' array required"); return ESP_OK; }

  int start = doc["start"] | 0;
  if (start < 0 || start > 254) { httpxErr(r, 400, "'start' must be 0..254"); return ESP_OK; }
  int step = doc["step_ms"] | 0;
  if (step < 0)  step = 0;
  if (step > 30) step = 30;

  // Resolve EVERY cell before sending ANY of it. A half-written wall is worse than a
  // rejected request: the caller cannot tell how far it got, and the wall is left showing
  // a sentence that was never asked for.
  static int16_t flap[VM_MAX_MODULES];      // -1 = skip
  int n = 0;
  for (JsonObjectConst c : cells) {
    if (n >= VM_MAX_MODULES) break;
    if (start + n > 254)     break;

    if (c["skip"].is<bool>() && c["skip"].as<bool>())  { flap[n++] = -1; continue; }
    if (c["blank"].is<bool>() && c["blank"].as<bool>()) { flap[n++] = 0;  continue; }

    if (c["color"].is<const char*>()) {
      int idx = reelColourIndex(c["color"].as<const char*>());
      if (idx < 0) {
        char e[96];
        snprintf(e, sizeof(e), "cell %d: unknown color '%.16s' (red orange yellow green "
                 "blue purple white)", n, c["color"].as<const char*>());
        httpxErr(r, 400, e); return ESP_OK;
      }
      flap[n++] = (int16_t)idx;
      continue;
    }

    if (c["ch"].is<const char*>()) {
      const char* ch = c["ch"].as<const char*>();
      uint32_t cp = 0;
      if (!ch || !*ch || utf8Next(ch, &cp) == 0) {
        char e[64]; snprintf(e, sizeof(e), "cell %d: 'ch' is not valid UTF-8", n);
        httpxErr(r, 400, e); return ESP_OK;
      }
      int idx = vmFlapIndexOfCodepoint(cp);
      if (idx < 0) {
        char e[96];
        snprintf(e, sizeof(e), "cell %d: no flap for U+%04X -- the reel cannot show it",
                 n, (unsigned)cp);
        httpxErr(r, 400, e); return ESP_OK;
      }
      flap[n++] = (int16_t)idx;
      continue;
    }

    char e[80];
    snprintf(e, sizeof(e), "cell %d: need one of ch, color, blank, skip", n);
    httpxErr(r, 400, e); return ESP_OK;
  }

  { char cd[64]; snprintf(cd, sizeof(cd), "cells %d from id %d, step=%dms", n, start, step);
    logCommand('R', cd); }

  // Send by INDEX. This is the ordinary m<id>+<n> command -- the modules have understood it
  // all along; it is only the gateway that never had a reason to speak it.
  int sent = 0;
  uint32_t due = millis();
  for (int i = 0; i < n; i++) {
    if (flap[i] < 0) continue;                       // skip: leave the module as it is
    char f[16];
    int len = snprintf(f, sizeof(f), "m%d+%d\n", start + i, (int)flap[i]);
    if (step > 0 && frameSendScheduled((const uint8_t*)f, (size_t)len, due)) {
      due += (uint32_t)step;
    } else {
      frameSend((const uint8_t*)f, (size_t)len, false);
    }
    sent++;
    wdgWebMs = millis();
  }
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"cells\":%d,\"sent\":%d}", n, sent);
  return httpxSend(r, 200, "application/json", resp);
}

/* GET /api/flap/modules
 *
 * The wall IS the modules. There is no registry to consult: every cell of the wall is a
 * module and its id is its cell index. This reads vmods[] directly -- the one
 * place that actually knows. (The fake serial numbers and reported module
 * firmware version left with the 'v'/'A' queries in v1.24.)
 *
 * (There used to be a shadow copy: a sticky SFModule registry, persisted to FATFS, with a
 * stale-probe pruner and a duplicate-ID heuristic. It existed to track modules that appear
 * and disappear on a physical wire. Nothing here appears or disappears.)
 */
static esp_err_t handleApiModules(httpd_req_t* r) {
  // setConnectionTimeout(), NOT setTimeout(): the latter sets Stream's *read* timeout and
  // leaves SO_SNDTIMEO at its 3 s default. This loop does one socket write per module, so on
  // a wedged client 160 modules x 3 s is far past the 120 s web watchdog, which would reboot
  // the board. Bound each write instead.
  httpd_resp_set_type(r, "application/json");
  httpxChunkStr(r, "[");

  const int n = vmCount;
  for (int i = 0; i < n; i++) {
    // Snapshot one module under the lock, format outside it: the drawing task walks this
    // array at 100 Hz and must not wait on a socket write.
    uint8_t id = 0; int flap = 0;
    if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      id   = vmods[i].id;
      flap = vmods[i].curIndex;
      xSemaphoreGive(vmMutex);
    }
    // flapChar reports the CODE POINT, like /api/display/state: a pictograph flap has no
    // CP1252 byte, and reporting an empty string for one is how you end up believing the wall
    // is blank when it is showing a heart. A colour flap has no character at all and reports
    // as its protocol letter (r o y g b p w).
    char utf8[8] = "";
    const uint32_t cp = vmFlapCodepointAt(flap);
    if (cp) {
      size_t n8 = utf8Encode(cp, utf8);
      utf8[n8] = 0;
      if (cp == '"' || cp == '\\') { utf8[1] = utf8[0]; utf8[0] = '\\'; utf8[2] = 0; }   // JSON
    } else {
      const char c = vmFlapCharAt(flap);                 // a colour flap
      if (c) { utf8[0] = c; utf8[1] = 0; }
    }

    char row[96];
    snprintf(row, sizeof(row),
             "%s{\"id\":%u,\"flapIndex\":%d,\"flapChar\":\"%s\"}",
             i ? "," : "", (unsigned)id, flap, utf8);
    httpxChunkStr(r, row);
    wdgWebMs = millis();
  }
  httpxChunkStr(r, "]");
  return httpxChunkEnd(r);
}


/* GET /api/display/state -- what the wall is actually showing.
 *
 * Read straight off the reels (vmods[i].curIndex), not off a shadow copy of what the gateway
 * once transmitted. That distinction is not academic: the old gWallChars mirror stored a
 * BYTE per cell, so it could not represent a pictograph flap at all (no byte exists) and it
 * recorded what was *sent* rather than what is *shown*.
 *
 * A cell is: the character the flap carries, "?" when the flap has no byte (a pictograph --
 * text cannot name it, the panel draws it fine), or null when there is no module there.
 */
static esp_err_t handleApiDisplayState(httpd_req_t* r) {
  const int rows = gPanel.rows ? gPanel.rows : 1;
  const int cols = gPanel.cols ? gPanel.cols : 1;
  const int cells = rows * cols;

  static int16_t flap[VM_MAX_MODULES];
  int n = vmCount;
  if (n > VM_MAX_MODULES) n = VM_MAX_MODULES;
  if (vmMutex && xSemaphoreTake(vmMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = 0; i < n; i++) flap[i] = vmods[i].curIndex;
    xSemaphoreGive(vmMutex);
  }

  httpd_resp_set_type(r, "application/json");
  char head[48];
  snprintf(head, sizeof(head), "{\"rows\":%d,\"cols\":%d,\"cells\":[", rows, cols);
  httpxChunkStr(r, head);
  for (int i = 0; i < cells; i++) {
    char one[16];
    if (i >= n) {
      snprintf(one, sizeof(one), "%snull", i ? "," : "");
    } else {
      // Report the CODE POINT, not the byte. A pictograph flap has no CP1252 byte, so a
      // byte-shaped read renders a heart as '?' -- which is the same blindness that made the
      // old registry unable to restore one after Quiet Time. A colour flap has no character
      // at all; it reports as its protocol letter (r o y g b p w), as it always has.
      const uint32_t cp = vmFlapCodepointAt(flap[i]);
      if (!cp) {
        const char c = vmFlapCharAt(flap[i]);                   // a colour flap
        snprintf(one, sizeof(one), "%s\"%c\"", i ? "," : "", c ? c : ' ');
      } else {
        char utf8[8] = "";
        size_t n8 = utf8Encode(cp, utf8);
        utf8[n8] = 0;
        if (cp == '"' || cp == '\\')                            // JSON-escape the two that need it
          snprintf(one, sizeof(one), "%s\"\\%s\"", i ? "," : "", utf8);
        else
          snprintf(one, sizeof(one), "%s\"%s\"", i ? "," : "", utf8);
      }
    }
    httpxChunkStr(r, one);
    if ((i & 31) == 0) wdgWebMs = millis();
  }
  // v3.0.1, both additive: "flaps" is the raw flap INDEX per cell (-1 = no module), the
  // only way a client can tell a colour flap (156..162) from a lowercase r/o/y/g/b/p/w --
  // the "cells" letter is identical for both. "mode" says whether the PANEL is currently
  // showing this wall at all: "pixels" means canvas/effect/animation/ticker owns it, and
  // a live preview should render GET /api/canvas/frame instead of these cells.
  httpxChunkStr(r, "],\"flaps\":[");
  for (int i = 0; i < cells; i++) {
    char one[12];
    snprintf(one, sizeof(one), "%s%d", i ? "," : "", i < n ? (int)flap[i] : -1);
    httpxChunkStr(r, one);
  }
  char tail[32];
  snprintf(tail, sizeof(tail), "],\"mode\":\"%s\"}", dispPixelsMode() ? "pixels" : "wall");
  httpxChunkStr(r, tail);
  return httpxChunkEnd(r);
}

// The same display-state JSON, serialized into a buffer for the SSE pump (web.h). The
// REST handler above streams; this builds -- the pump wraps the result in an SSE frame
// and pushes one copy to every /api/events stream. Returns bytes written (never > cap-1).
size_t dispStateJson(char* out, size_t cap) {
  if (!out || cap < 32) return 0;
  const int rows = gPanel.rows ? gPanel.rows : 1;
  const int cols = gPanel.cols ? gPanel.cols : 1;
  const int cells = rows * cols;

  static int16_t flap[VM_MAX_MODULES];   // taskWeb is the only caller: no reentrancy
  int n = vmCount;
  if (n > VM_MAX_MODULES) n = VM_MAX_MODULES;
  if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < n; i++) flap[i] = vmods[i].curIndex;
    xSemaphoreGive(vmMutex);
  } else {
    return 0;                            // reels busy: the pump just tries again next tick
  }

  size_t o = (size_t)snprintf(out, cap, "{\"rows\":%d,\"cols\":%d,\"cells\":[", rows, cols);
  for (int i = 0; i < cells && o + 16 < cap; i++) {
    if (i >= n) {
      o += (size_t)snprintf(out + o, cap - o, "%snull", i ? "," : "");
      continue;
    }
    const uint32_t cp = vmFlapCodepointAt(flap[i]);
    if (!cp) {
      const char c = vmFlapCharAt(flap[i]);                   // a colour flap
      o += (size_t)snprintf(out + o, cap - o, "%s\"%c\"", i ? "," : "", c ? c : ' ');
    } else {
      char utf8[8] = "";
      size_t n8 = utf8Encode(cp, utf8);
      utf8[n8] = 0;
      if (cp == '"' || cp == '\\')                            // JSON-escape the two that need it
        o += (size_t)snprintf(out + o, cap - o, "%s\"\\%s\"", i ? "," : "", utf8);
      else
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"", i ? "," : "", utf8);
    }
  }
  o += (size_t)snprintf(out + o, cap - o, "],\"flaps\":[");
  for (int i = 0; i < cells && o + 12 < cap; i++)
    o += (size_t)snprintf(out + o, cap - o, "%s%d", i ? "," : "", i < n ? (int)flap[i] : -1);
  o += (size_t)snprintf(out + o, cap - o, "],\"mode\":\"%s\"}", dispPixelsMode() ? "pixels" : "wall");
  return o;
}


// GET/POST /api/display/brightness -- the panel's global brightness (v2.2).
// GET reports it; POST {"brightness":1..255} applies it to the NEXT FRAME (the same
// path handleApiConfigSettings takes: panelSetBrightness only sets a pending flag,
// and the next panelShow in any mode -- wall, effect, canvas, animation -- writes
// the new duty) and persists it. Advertised as the "brightness" capability token.
static esp_err_t handleApiDisplayBrightness(httpd_req_t* r) {
  if (r->method == HTTP_POST) {
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (!doc["brightness"].is<int>()) { httpxErr(r, 400, "'brightness' (1-255) required"); return ESP_OK; }
    int v = doc["brightness"].as<int>();
    if (v < 1 || v > 255) { httpxErr(r, 400, "'brightness' (1-255) required"); return ESP_OK; }
    cfg.panelBright = (uint8_t)v;
    panelSetBrightness(cfg.panelBright);   // pending flag; the next panelShow applies it
    dispMarkDirty();                       // an idle wall repaints, so the change is visible now
    saveConfig();
    { char cd[LOG_TEXT_MAX]; snprintf(cd, sizeof(cd), "brightness %d", v); logCommand('R', cd); }
  }
  char out[48];
  snprintf(out, sizeof(out), "{\"ok\":true,\"brightness\":%u}", (unsigned)cfg.panelBright);
  return httpxSend(r, 200, "application/json", out);
}

// POST /api/flap/char   {"id":5,"char":"A"}   id=-1 for broadcast
static esp_err_t handleApiChar(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  int id = doc["id"] | -1;
  const char* ch = doc["char"] | "";
  if (!ch[0]) { httpxErr(r, 400, "Missing char"); return ESP_OK; }
  // `ch` is UTF-8: a euro/accented glyph is multi-byte. Transcode to a single
  // Windows-1252 byte and display the first character (see charset.h).
  char enc[8];
  utf8ToFlap(ch, enc, sizeof(enc));
  if (!enc[0]) { httpxErr(r, 400, "Unsupported character"); return ESP_OK; }
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "char -> all modules");
    else        snprintf(cd, sizeof(cd), "char -> module %d", id);
    logCommand('R', cd); }
  sfSendChar(id, enc[0]);
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/index  {"id":5,"index":3}
static esp_err_t handleApiIndex(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  int id  = doc["id"]    | -1;
  int idx = doc["index"] | -1;
  // The whole reel is addressable -- the lowercase and pictograph sections included:
  // this is the same m<id>+<n> command /api/display/cells sends.
  if (idx < 0 || idx >= SF_MAX_FLAPS) {
    char e[48];
    snprintf(e, sizeof(e), "Invalid index (0-%d)", SF_MAX_FLAPS - 1);
    httpxErr(r, 400, e); return ESP_OK;
  }
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "index %d -> all modules", idx);
    else        snprintf(cd, sizeof(cd), "index %d -> module %d", idx, id);
    logCommand('R', cd); }
  sfSendIndex(id, idx);
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/text   {"text":"HELLO","start":0}
static esp_err_t handleApiText(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* text = doc["text"] | "";
  int start = doc["start"] | 0;
  if (!text[0]) { httpxErr(r, 400, "Empty text"); return ESP_OK; }
  { char cd[LOG_TEXT_MAX]; snprintf(cd, sizeof(cd), "text from module %d: \"%.60s\"", start, text);
    logCommand('R', cd); }
  sfSendText(start, text);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"chars\":%zu}", strlen(text));
  return httpxSend(r, 200, "application/json", resp);
}

// POST /api/flap/home   {"id":5}  or  {"id":-1}
static esp_err_t handleApiHome(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  int id = doc["id"] | -1;
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "home all modules");
    else        snprintf(cd, sizeof(cd), "home module %d", id);
    logCommand('R', cd); }
  sfHome(id);
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}


/* ----------------------------------------------------------
   GET /api/capabilities -- what this wall can actually show
   ----------------------------------------------------------
   One call, made once when a client connects, that answers "what characters can this display
   show?" without the client having to know what kind of gateway it is talking to. The physical
   gateway answers the same question at the same URL, with the same shape -- which is the whole
   point of it: a client asks the wall, not the hardware.

   On THIS board the answer is short, because the wall is drawn: every module renders from one
   shared reel (reel.h), so there is exactly one flap set and `uniform` is true by construction.
   On the physical gateway every module owns its own reel and they need not agree, so the same
   response can carry several sets -- and there, `union` and `common` genuinely differ.

   `sets` reports each DISTINCT reel once, with the ids that use it, rather than one entry per
   module. A 75-module wall with one reel is one entry and one range, a few hundred bytes; a
   mixed wall costs one entry per genuinely different reel, which is the information itself
   rather than a repetition of it.

   Streamed, like /api/flap/modules: the union string alone is ~300 bytes and nothing here needs
   to exist in RAM all at once.                                                                */
// A BUFFERED writer for the response below.
//
// Each httpxChunk() is one HTTP chunk and one TCP write. Streaming this response a CHARACTER
// at a time -- which is the obvious way to write it, and what this did at first -- sent a
// 230-character repertoire as ~700 chunks of one to three bytes, each waiting on its own
// round-trip: FIVE SECONDS to deliver 1.6 KB, while /api/status delivered 465 bytes in twenty
// milliseconds. Time-to-first-byte was 11 ms throughout, so none of it was the computing. It was
// the writing.
//
// So: accumulate, and flush a kilobyte at a time.
static char   capBuf[1024];
static size_t capLen = 0;

static void capFlush() {
  if (!capLen) return;
  capBuf[capLen] = 0;
  httpxChunkStr(gStreamReq, capBuf);
  capLen = 0;
  wdgWebMs = millis();
}
static void capPut(const char* str) {
  size_t n = strlen(str);
  if (capLen + n >= sizeof(capBuf) - 1) capFlush();
  if (n >= sizeof(capBuf) - 1) { httpxChunkStr(gStreamReq, str); return; }   // never truncate a caller
  memcpy(capBuf + capLen, str, n);
  capLen += n;
}
// One code point into the buffer, escaped as JSON requires.
static void capPutCp(uint32_t cp) {
  char out[8];
  if (cp == '"' || cp == '\\') { out[0] = '\\'; out[1] = (char)cp; out[2] = 0; }
  else { size_t n = utf8Encode(cp, out); out[n] = 0; }
  capPut(out);
}
// The reel as a JSON string body: every flap that shows a CHARACTER. The colour flaps are
// skipped -- they are named, not spelled -- and the lowercase and pictograph flaps are in,
// because they are reachable (by index, via /api/display/cells) and a client that could not see
// them here would never know to ask.
static void capPutReel() {
  for (int i = 0; i < SF_MAX_FLAPS; i++) {
    const uint32_t cp = vmFlapCodepointAt(i);
    if (cp) capPutCp(cp);
  }
}

static esp_err_t handleApiCapabilities(httpd_req_t* r) {
  const int rows = gPanel.rows ? gPanel.rows : 1;
  const int cols = gPanel.cols ? gPanel.cols : 1;

  httpd_resp_set_type(r, "application/json");
  gStreamReq = r;
  capLen = 0;

  char head[320];
  snprintf(head, sizeof(head),
           "{\"product\":\"%s\",\"fw\":\"%s\",\"api\":\"%s\","
           "\"openapi\":\"/openapi.yaml\","
           "\"grid\":{\"rows\":%d,\"cols\":%d},\"modules\":%d,\"maxFlaps\":%d,",
           PRODUCT_NAME, FW_VERSION, API_VERSION, rows, cols, vmCount, SF_MAX_FLAPS);
  capPut(head);

  // The colour flaps are NOT characters. They are named, because on the index-addressed path
  // 'r' is the letter r -- which is exactly why /api/display/cells names them too.
  capPut("\"colors\":[");
  for (int i = 0; i < SF_COLOUR_FLAPS; i++) {
    char one[16];
    snprintf(one, sizeof(one), "%s\"%s\"", i ? "," : "", REEL_COLOUR_NAMES[i]);
    capPut(one);
  }
  capPut("],");

  // union == common == the reel, and uniform is true: one reel, drawn once per cell. All three
  // fields are still present, because a client must not have to care which gateway it is
  // talking to -- the physical gateway answers this same URL, where they genuinely differ.
  capPut("\"charset\":{\"uniform\":true,\"assumed\":[],\"unknown\":[],\"union\":\"");
  capPutReel();
  capPut("\",\"common\":\"");
  capPutReel();
  capPut("\"},");

  // One reel, every module. `source` says where a set came from: read off the module
  // ("reported"), its firmware's compiled-in default ("assumed"), or -- here -- the gateway's
  // own reel, which it drew itself and therefore knows exactly.
  char sets[96];
  snprintf(sets, sizeof(sets),
           "\"sets\":[{\"flaps\":%d,\"source\":\"builtin\",\"modules\":\"0-%d\",\"chars\":\"",
           SF_MAX_FLAPS, vmCount - 1);
  capPut(sets);
  capPutReel();
  capPut("\"}],");

  // How the wall MOVES. "drawn": a cell is a repaint, not a mechanism — a new value can
  // retarget it mid-flip, nothing queues, so sub-second updates (a ticking seconds field)
  // are honest here. settleMs is the worst-case flip ANIMATION (flapMs x flapMax, both
  // live-configurable) — cosmetic pacing, not a physical constraint; it is advisory, for a
  // client pacing full-wall animations. The physical gateway answers the same key with kind
  // "mechanical", where the number IS a physical constraint. Stated directly, so a client
  // never has to infer motion from which endpoints exist.
  { char motion[64];
    snprintf(motion, sizeof(motion), "\"motion\":{\"kind\":\"drawn\",\"settleMs\":%lu},",
             (unsigned long)cfg.flapMs * (unsigned long)cfg.flapMax);
    capPut(motion); }

  // Raw canvas and on-device effects are Matrix-only -- the physical wall has no framebuffer to
  // hand out, and answers this URL without these keys. Stated here so the companion lights up
  // canvas/effect controls from capabilities, not from a firmware-version sniff: `canvas` is the
  // framebuffer a client would push frames to, `effects` the on-device animation set.
  { char cv[960];   // v3.1: the atlas descriptor + rects flag overflowed 480 and snprintf
                    // TRUNCATED the JSON -- /api/capabilities went invalid, silently. Sized
                    // with headroom and verified below.
    snprintf(cv, sizeof(cv),
             "\"canvas\":{\"formats\":[\"rgb888\",\"rgb565\",\"qoi\"],\"width\":%u,\"height\":%u,"
             "\"rect\":true,\"rects\":true,\"stream\":true,\"opsBin\":1,\"anim\":true,\"ticker\":true,\"readback\":true,"
             "\"atlas\":{\"named\":true,\"persist\":true,\"maxSheets\":%u,"
             "\"maxBytes\":%u,\"maxSheetBytes\":%u},"
             "\"ops\":[\"clear\",\"pixel\",\"hline\",\"vline\",\"line\",\"rect\",\"circle\",\"ellipse\","
             "\"triangle\",\"roundrect\",\"gradient\",\"polyline\",\"poly\",\"arc\",\"bezier\","
             "\"clip\",\"origin\",\"blend\",\"text\",\"textbox\",\"image\",\"sprite\",\"scroll\",\"show\"],"
             "\"compositing\":{\"alpha\":true,\"blendModes\":[\"over\",\"add\",\"multiply\",\"screen\",\"max\"],\"aa\":true}},"
             "\"effects\":%s,\"effectParams\":%s,",
             (unsigned)gPanel.panelW, (unsigned)gPanel.panelH,
             (unsigned)ATLAS_MAX_SHEETS, (unsigned)ATLAS_TOTAL_BUDGET,
             (unsigned)ATLAS_MAX_SHEET_BYTES, effectListJson(), effectParamsUnionJson());
    // A truncated canvas block would be INVALID JSON for every client; make it loud.
    if (strlen(cv) >= sizeof(cv) - 1) printf("[WEB] capabilities canvas block TRUNCATED -- enlarge cv[]\n");
    capPut(cv); }

  // Self-describing effect defs (v3.4): every effect with exactly the params it
  // consumes -- clients gate on the "effectDefs" feature token and build their
  // effect UIs from this instead of hard-coding options. Additive: the flat
  // "effects" + "effectParams" above are unchanged for older clients.
  capPut("\"effectDefs\":");
  capPut(effectDefsJson());
  capPut(",");

  // What the wall can DO, not just show, so a client reads this instead of sniffing the
  // firmware version and guessing.
  { char ft[400];
    snprintf(ft, sizeof(ft),
             "\"features\":[\"cells\",\"colors\",\"index\",\"lowercase\",\"pictographs\","
             "\"quiet\",\"ota\",\"canvas\",\"effects\",\"ticker\",\"brightness\",\"events\","
             "\"effectDefs\"%s%s%s]}",
             audioAvailable() ? ",\"audio\"" : "",
             soundAvailable() ? ",\"sound\"" : "",
             sensorAvailable() ? ",\"environment\"" : "");
    capPut(ft); }
  capFlush();
  return httpxChunkEnd(r);
}

// The status JSON, shared by GET /api/status and the SSE `status` event (v3.2).
static float envCompRH(float rawT, float rawRH, float offC) {
  if (offC == 0.0f) return rawRH;
  const float ta   = rawT + offC;
  const float ps_s = expf(17.62f * rawT / (243.12f + rawT));
  const float ps_a = expf(17.62f * ta   / (243.12f + ta));
  float rh = rawRH * ps_s / ps_a;
  return rh < 0 ? 0 : rh > 100 ? 100 : rh;
}

size_t statusJson(char* outBuf, size_t outCap) {
  // Use snprintf to avoid JsonDocument heap allocation (called every 3s by browser)
  char rtcBuf[24]; rtcFormatTime(rtcBuf, sizeof(rtcBuf));
  IPAddress lip = WiFi.localIP(), aip = WiFi.softAPIP();
  // Per-task minimum-ever free stack (bytes). A value trending toward 0 is an
  // early warning of the stack-canary crash class.
  unsigned stkFrm = hTaskFrames ? uxTaskGetStackHighWaterMark(hTaskFrames) : 0;
  unsigned stkWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned stkNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  TaskHandle_t hHttpd = xTaskGetHandle("httpd");
  unsigned stkHtp = hHttpd     ? uxTaskGetStackHighWaterMark(hHttpd)     : 0;
  unsigned stkRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  // v3.0: seconds since the companion last checked in (-1 = never / deregistered)
  long compAge = gCompanionSeenMs ? (long)((millis() - gCompanionSeenMs) / 1000UL) : -1;
  unsigned stkDsp = hTaskDisp ? uxTaskGetStackHighWaterMark(hTaskDisp) : 0;
  // Environment (v3.7): the cached SHTC3 reading, or {"ok":false} when absent/pending.
  char envf[80]; float eT, eH; uint32_t eAge;
  if (sensorRead(eT, eH, eAge)) {
    const float offc = cfg.tempOffsetC10 / 10.0f;        // calibration offset (v3.7)
    const float tcal = eT + offc;
    const float rhc  = envCompRH(eT, eH, offc);          // RH follows the same offset
    snprintf(envf, sizeof(envf), "{\"ok\":true,\"tempC\":%.1f,\"rh\":%.0f,\"age\":%lu}",
             tcal, rhc, (unsigned long)(eAge / 1000));
  }
  else snprintf(envf, sizeof(envf), "{\"ok\":false}");
  size_t n = (size_t)snprintf(outBuf, outCap,
    "{\"uptime\":%lu,\"tx\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"minheap\":%u,\"modules\":%d,"
    "\"stk\":{\"frames\":%u,\"web\":%u,\"net\":%u,\"httpd\":%u,\"rtc\":%u,\"disp\":%u},"
    "\"panel\":{\"ok\":%s,\"w\":%u,\"h\":%u,\"cols\":%u,\"rows\":%u,"
    "\"cellW\":%u,\"cellH\":%u,\"depth\":%u,\"font\":\"%s\",\"vmods\":%d},"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"quiet\":%s,"
    "\"companion\":{\"url\":\"%s\",\"status\":\"%s\",\"age\":%ld},\"env\":%s}",
    millis()/1000, txCount,
    (WiFi.status()==WL_CONNECTED)?"true":"false",
    lip[0],lip[1],lip[2],lip[3],
    aip[0],aip[1],aip[2],aip[3],
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    vmCount,
    stkFrm, stkWeb, stkNet, stkHtp, stkRtc, stkDsp,
    gPanel.ready?"true":"false", gPanel.panelW, gPanel.panelH,
    gPanel.cols, gPanel.rows, gPanel.cellW, gPanel.cellH, (unsigned)panelInfo().depth,
    dispFontName(), vmCount,
    rtcBuf,
    ntpSynced?"true":"false",
    gQuietTime?"true":"false",
    cfg.companionUrl, gCompanionStatus, compAge, envf);
  return n < outCap ? n : outCap - 1;
}

// GET /api/status
static esp_err_t handleApiStatus(httpd_req_t* r) {
  char out[900];
  statusJson(out, sizeof(out));
  return httpxSend(r, 200, "application/json", out);
}

// GET /api/config
static esp_err_t handleApiConfigGet(httpd_req_t* r) {
  JsonDocument doc;
  // "version" is the GATEWAY API level, not this firmware's version. The
  // companion parses MAJOR.MINOR out of it and enables its gateway-stored
  // settings on >= 3.1; this firmware implements that surface exactly, so it
  // must answer 3.1.0. "product" and "fwVersion" are what tell the two apart.
  doc["version"]   = API_VERSION;
  doc["product"]   = PRODUCT_NAME;
  doc["fwVersion"] = FW_VERSION;
  doc["wSSID"]    = cfg.wifiSSID;
  doc["posixTZ"]    = cfg.posixTZ;
  doc["ntpServer"]  = cfg.ntpServer;
  doc["gridRows"]   = cfg.gridRows;
  doc["gridCols"]   = cfg.gridCols;
  doc["serialDebug"]   = cfg.serialDebug;
  doc["hostname"]       = cfgHostname();          // effective, MAC-derived if unset
  doc["hostnameAuto"]   = (cfg.hostname[0] == 0); // true = derived, not pinned
  // ---- panel (Matrix Portal Gateway) ----
  // gridRows/gridCols above are the emulated WALL: one virtual module per cell.
  // These describe the LED panel it is drawn on. Additive fields -- the companion
  // ignores anything it does not name.
  doc["panelW"]        = cfg.panelW;
  doc["panelH"]        = cfg.panelH;
  doc["panelBitDepth"] = cfg.panelBitDepth;
  doc["panelBGR"]      = cfg.panelBGR;
  doc["panelBright"]   = cfg.panelBright;
  doc["flapMs"]        = cfg.flapMs;
  doc["flapMax"]       = cfg.flapMax;
  doc["soundEnabled"]  = cfg.soundEnabled;   // master speaker enable (v3.6)
  doc["soundVolume"]   = cfg.soundVolume;    // master volume 0..100
  doc["tempOffset"]    = cfg.tempOffsetC10 / 10.0;   // SHTC3 temp calibration, degC (v3.7)
  { static const char* const TN[4] = {"none","crossfade","wipe","slide"};
    doc["transitionType"] = TN[cfg.transType <= 3 ? cfg.transType : 0];   // persisted (v3.7.2)
    doc["transitionMs"]   = cfg.transMs; }
  doc["maxFlaps"]      = SF_MAX_FLAPS;   // 237: glyphs + colours + lowercase + pictographs
  doc["bootAnim"]      = cfg.bootAnim;   // library animation autoplayed at boot ("" = none)
  char out[1280];   // headroom for identity + panel + JSON-escaped SSID/TZ/hostname
  serializeJson(doc, out, sizeof(out));
  return httpxSend(r, 200, "application/json", out);
}

// POST /api/config/wifi
static esp_err_t handleApiConfigWifi(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  saveConfig();
  DBG("[CFG] WiFi SSID set to '%s'\n", cfg.wifiSSID);
  httpxSend(r, 200, "application/json", "{\"ok\":true}");
  delay(100);            // let the 200 reach the wire before the interface drops
  WiFi.disconnect();     // taskNetwork re-associates with the new credentials
  return ESP_OK;
}



// POST /api/config/settings  -- save all settings in one call
static esp_err_t handleApiConfigSettings(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  // WiFi
  if (doc["ssid"].is<const char*>()) strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  if (doc["pass"].is<const char*>()) strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  // Serial debug toggle
  if (doc["serialDebug"].is<bool>()) {
    cfg.serialDebug = doc["serialDebug"].as<bool>();
    gSerialDebug    = cfg.serialDebug;
    saveConfig();
    printf("[CFG] Serial debug %s\n", cfg.serialDebug ? "enabled" : "disabled");
    return httpxSend(r, 200, "application/json", "{\"ok\":true}");
    return ESP_OK;
  }
  if (doc["posixTZ"].is<const char*>()) {
    strlcpy(cfg.posixTZ, doc["posixTZ"] | "UTC0", sizeof(cfg.posixTZ));
    strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
    cfgApplyTZ();
    ntpSynced = false;
    DBG("[CFG] Timezone set to %s\n", cfg.posixTZ);
  }
  if (doc["ntpServer"].is<const char*>()) {
    strlcpy(cfg.ntpServer, doc["ntpServer"] | DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    if (!cfg.ntpServer[0]) strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
    ntpSynced = false;   // re-sync against the new server on next network tick
    DBG("[CFG] NTP server set to %s\n", cfg.ntpServer);
  }
  if (doc["gridRows"].is<int>() || doc["gridCols"].is<int>()) {
    int gr = doc["gridRows"] | cfg.gridRows;
    int gc = doc["gridCols"] | cfg.gridCols;
    if (gr < 1)   gr = 1;
    if (gr > 64)  gr = 64;
    if (gc < 1)   gc = 1;
    if (gc > 64)  gc = 64;
    // Unlike the physical gateway, this grid is the REAL wall here: every cell is a
    // virtual module that has to exist. Bound the product, not just each side.
    // Shrink the taller dimension first so a wide wall stays wide.
    while (gr * gc > VM_MAX_MODULES) { if (gr > gc) gr--; else gc--; }
    cfg.gridRows = (uint8_t)gr;
    cfg.gridCols = (uint8_t)gc;
    DBG("[CFG] Wall set to %dx%d (rows x cols) = %d modules -- reboot to apply\n",
        gr, gc, gr * gc);
  }
  // ---- panel geometry and reel speed ----
  // The driver takes width/height/bitDepth at construction, so those need a reboot;
  // brightness and the flip timing are picked up on the next frame.
  if (doc["panelW"].is<int>())  { int v = doc["panelW"];
    if (v >= 32 && v <= PANEL_MAX_W) cfg.panelW = (uint16_t)v; }
  if (doc["panelH"].is<int>())  { int v = doc["panelH"];
    if (v == 16 || v == 32 || v == 64) cfg.panelH = (uint16_t)v; }
  if (doc["panelBitDepth"].is<int>()) { int v = doc["panelBitDepth"];
    if (v >= 1 && v <= 6) cfg.panelBitDepth = (uint8_t)v; }
  // Applies to the NEXT FRAME, not on reboot: it is only a decision about which bit a
  // colour lands on, so there is nothing to re-allocate and no reason to make anyone
  // power-cycle to find out whether their panel is BGR.
  if (doc["panelBGR"].is<bool>()) {
    cfg.panelBGR = doc["panelBGR"].as<bool>();
    panelSetColourOrder(cfg.panelBGR);
  }
  if (doc["panelBright"].is<int>())   { int v = doc["panelBright"];
    // Apply now, not just on the next wall repaint: an effect or raw canvas owns the panel while
    // taskDisplay stands down, so dispRender (the only other caller) would not push the new duty.
    // panelSetBrightness only sets a pending flag; the next panelShow in any mode writes it.
    if (v >= 1 && v <= 255) { cfg.panelBright = (uint8_t)v; panelSetBrightness(cfg.panelBright); } }
  if (doc["flapMs"].is<int>())        { int v = doc["flapMs"];
    if (v >= 2 && v <= 500) cfg.flapMs = (uint16_t)v; }
  if (doc["flapMax"].is<int>())       { int v = doc["flapMax"];
    if (v >= 1 && v <= FLAP_ANIM_MAX) cfg.flapMax = (uint8_t)v; }
  if (doc["soundEnabled"].is<bool>()) {
    cfg.soundEnabled = doc["soundEnabled"];
    if (!cfg.soundEnabled) soundStop();      // disabling silences anything mid-play
  }
  if (doc["soundVolume"].is<int>())   { int v = doc["soundVolume"];
    if (v >= 0 && v <= 100) cfg.soundVolume = (uint8_t)v; }
  if (doc["tempOffset"].is<float>() || doc["tempOffset"].is<int>()) {
    float o = doc["tempOffset"].as<float>();
    if (o < -30) o = -30; else if (o > 30) o = 30;
    cfg.tempOffsetC10 = (int16_t)lroundf(o * 10.0f);
  }
  // Boot animation (v2.1): a library name, or "" to disable. Validated so a typo
  // cannot wedge boot; existence is NOT required (the file may be uploaded later).
  if (doc["bootAnim"].is<const char*>()) {
    const char* ba = doc["bootAnim"].as<const char*>();
    if (!*ba || canvasAnimNameOk(ba)) strlcpy(cfg.bootAnim, ba, sizeof(cfg.bootAnim));
  }
  // Hostname. Lowercase first (DNS labels are case-insensitive but mDNS responders and
  // browsers are not always careful), then validate. An empty string means "go back to
  // deriving it from the MAC" -- that is how you un-pin a name. Takes effect on reboot.
  if (doc["hostname"].is<const char*>()) {
    const char* h = doc["hostname"];
    char lo[HOSTNAME_MAX];
    size_t n = 0;
    for (; h[n] && n < sizeof(lo) - 1; n++) lo[n] = (char)tolower((unsigned char)h[n]);
    lo[n] = 0;
    if (!lo[0])                    cfg.hostname[0] = 0;               // revert to auto
    else if (cfgValidHostname(lo)) strlcpy(cfg.hostname, lo, sizeof(cfg.hostname));
    else { httpxErr(r, 400, "hostname must be 1-31 chars of a-z 0-9 -"); return ESP_OK; }
  }
  dispMarkDirty();
  saveConfig();

  // dispPlan() silently shrinks a wall that does not fit its panel. That is the right
  // behaviour at boot, but from a settings form it is a trap: a 15x3 wall on a 16px-high
  // panel collapses to 15x1 and you only find out by looking at the LEDs. Say so here.
  char resp[192];
  PanelGeometry plan = dispPlan(cfg.panelW, cfg.panelH, cfg.gridCols, cfg.gridRows);
  if (plan.cols != cfg.gridCols || plan.rows != cfg.gridRows) {
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"warn\":\"a %ux%u wall does not fit a %ux%u panel -- it will be reduced to %ux%u on reboot\"}",
             cfg.gridCols, cfg.gridRows, cfg.panelW, cfg.panelH, plan.cols, plan.rows);
  } else {
    strlcpy(resp, "{\"ok\":true}", sizeof(resp));
  }
  httpxSend(r, 200, "application/json", resp);
  // Only disconnect/reconnect if WiFi credentials were in the payload -- and only
  // after the 200 has had a moment to reach the wire.
  bool hasWifi = doc["ssid"].is<const char*>() || doc["pass"].is<const char*>();
  if (hasWifi) { delay(100); WiFi.disconnect(); }
  return ESP_OK;
}







// GET returns Quiet Time state; POST {"on":true|false} sets it.
static esp_err_t handleApiQuiet(httpd_req_t* r) {
  if (r->method == HTTP_POST) {
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (!doc["on"].is<bool>()) { httpxErr(r, 400, "'on' (bool) required"); return ESP_OK; }
    bool on = doc["on"].as<bool>();
    // The schedule wins inside its window: refuse a manual OFF here too, so the
    // window cannot be defeated by accident. Disable the schedule to override.
    if (!on && quietSchedInWindow()) {
      printf("[QUIET] REST quiet OFF ignored -- schedule active (in window)\n");
    } else {
      sfSetQuietTime(on);
    }
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}", gQuietTime ? "true" : "false");
  return httpxSend(r, 200, "application/json", out);
}

// GET/POST /api/quiet/schedule  -- daily Quiet-Time schedule (v3.0).
// The schedule is evaluated once a second in taskRTC; when the current local
// time enters/leaves the window, Quiet Time is toggled automatically.
static esp_err_t handleApiQuietSchedule(httpd_req_t* r) {
  if (r->method == HTTP_POST) {
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (doc["enabled"].is<bool>())        cfg.quietSchedEnabled = doc["enabled"].as<bool>();
    if (doc["start"].is<const char*>())   strlcpy(cfg.quietStart, doc["start"].as<const char*>(), sizeof(cfg.quietStart));
    if (doc["end"].is<const char*>())     strlcpy(cfg.quietEnd,   doc["end"].as<const char*>(),   sizeof(cfg.quietEnd));
    if (doc["days"].is<int>())            cfg.quietDays = (uint8_t)(doc["days"].as<int>() & 0x7F);
    if (doc["offset"].is<int>()) {        // browser's UTC offset (minutes east of UTC)
      int o = doc["offset"].as<int>();
      if (o < -720) o = -720;             // clamp to the valid TZ range (UTC-12:00 .. UTC+14:00)
      if (o >  840) o =  840;
      cfg.quietTzOffsetMin = (int16_t)o;
    }
    saveConfig();
    DBG("[CFG] Quiet schedule %s %s-%s days=0x%02X tzoff=%dmin\n",
        cfg.quietSchedEnabled ? "on" : "off", cfg.quietStart, cfg.quietEnd,
        cfg.quietDays, (int)cfg.quietTzOffsetMin);
  }
  JsonDocument out;
  out["enabled"] = cfg.quietSchedEnabled;
  out["start"]   = cfg.quietStart;
  out["end"]     = cfg.quietEnd;
  out["days"]    = cfg.quietDays;
  out["offset"]  = cfg.quietTzOffsetMin;   // browser's UTC offset, echoed back for the client
  char buf[128];
  serializeJson(out, buf, sizeof(buf));
  return httpxSend(r, 200, "application/json", buf);
}

// GET/POST /api/companion  -- the companion app registers its URL here (v3.0)
// and heartbeats its running status. The URL is persisted (only rewritten to
// NVS when it changes, to avoid flash wear from heartbeats); the status is
// runtime-only. An empty url deregisters.
/* The tabs THIS firmware has, advertised to the companion at registration so its nav can
   deep-link exactly the screens that exist here, rather than a list hard-coded over there
   that goes stale whenever this one changes.

   Deliberately SHORTER than the split-flap gateway's list. This product has no Provision
   and no Calibration tab -- its modules are drawn rather than driven, so there is nothing to
   calibrate -- and as of v1.7 no MODULES tab either: every cell of the wall is a module, all
   of them are always present, and none has a serial to inspect or an EEPROM to read. The
   page could only ever say the same thing 75 times. (The /api/flap/modules ENDPOINT stays:
   the companion reads it to learn the wall.) Advertising a tab that does not exist would
   send the companion linking into thin air. The id is the public one used in the URL hash ("status", not the pane id
   "statusp"); keep it in step with the <nav> in ui/index.html and the M map beside it. */
static const char* const GW_TAB_ID[]  = {"display", "files", "settings", "status"};
static const char* const GW_TAB_LBL[] = {"Display", "Files", "Settings", "Status"};
static const size_t GW_TAB_N = sizeof(GW_TAB_ID) / sizeof(GW_TAB_ID[0]);

// Store the tab list a companion advertised, re-serialised into gCompanionTabs.
// Anything malformed, oversized, or over the caps leaves the buffer EMPTY rather than
// half-filled: the dashboard then falls back to its built-in companion tabs, which is the
// same behaviour as an older companion that advertises nothing.
static void storeCompanionTabs(JsonArrayConst tabs) {
  gCompanionTabs[0] = '\0';
  if (tabs.isNull() || tabs.size() == 0 || tabs.size() > COMPANION_TABS_MAX_N) return;

  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (JsonObjectConst t : tabs) {
    const char* id  = t["id"].is<const char*>()    ? t["id"].as<const char*>()    : nullptr;
    const char* lbl = t["label"].is<const char*>() ? t["label"].as<const char*>() : nullptr;
    if (!id || !lbl || !id[0] || !lbl[0]) return;
    if (strlen(id) > COMPANION_TAB_ID_MAX || strlen(lbl) > COMPANION_TAB_LBL_MAX) return;
    // The id lands in a URL hash and the label in the nav, so keep both to plain printable
    // ASCII -- no quotes, no control characters, nothing to escape.
    for (const char* p = id; *p; p++)
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return;
    for (const char* p = lbl; *p; p++)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '"' || *p == '\\') return;
    JsonObject e = arr.add<JsonObject>();
    e["id"]    = id;
    e["label"] = lbl;
  }
  if (measureJson(out) >= sizeof(gCompanionTabs)) return;   // would not fit: advertise nothing
  serializeJson(out, gCompanionTabs, sizeof(gCompanionTabs));
}

static esp_err_t handleApiCompanion(httpd_req_t* r) {
  if (r->method == HTTP_POST) {
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (doc["url"].is<const char*>()) {
      const char* url = doc["url"].as<const char*>();
      if (strcmp(url, cfg.companionUrl) != 0) {
        // Apply to RAM now -- the companion tabs must light up immediately -- but do
        // NOT saveConfig() here. Two companions registering against one gateway flip
        // this value on every heartbeat, and an unconditional save made that an NVS
        // write every ~30 s for as long as both were up. taskNetwork persists it once
        // the value has held still (COMPANION_SAVE_DEBOUNCE_MS); a contested URL never
        // reaches flash, which is what we want.
        strlcpy(cfg.companionUrl, url, sizeof(cfg.companionUrl));
        gCompanionUrlDirty   = true;
        gCompanionUrlDirtyMs = millis();   // restart the clock on EVERY change
        DBG("[CFG] Companion URL set to %s\n", cfg.companionUrl);
      }
      if (url[0] == '\0') {                                                      // deregister
        gCompanionStatus[0] = '\0'; gCompanionTabs[0] = '\0'; gCompanionSeenMs = 0;
      } else gCompanionSeenMs = millis();
    }
    // A companion that advertises its tabs re-sends them on every heartbeat, so this is a
    // plain overwrite. One that never mentions `tabs` leaves whatever we hold alone -- a
    // heartbeat carrying only a status must not wipe the list.
    if (doc["tabs"].is<JsonArrayConst>()) storeCompanionTabs(doc["tabs"].as<JsonArrayConst>());
    if (doc["status"].is<const char*>()) {
      // Copy + sanitise so the string is always JSON-safe when echoed back.
      const char* st = doc["status"].as<const char*>();
      size_t n = 0;
      for (size_t i = 0; st[i] && n < sizeof(gCompanionStatus) - 1; i++) {
        char c = st[i];
        if (c == '"' || c == '\\') c = '\'';
        if ((unsigned char)c < 0x20) c = ' ';
        gCompanionStatus[n++] = c;
      }
      gCompanionStatus[n] = '\0';
      gCompanionSeenMs = millis();
    }
  }
  JsonDocument out;
  out["url"]    = cfg.companionUrl;
  out["status"] = gCompanionStatus;
  // The companion's tabs, for THIS dashboard's nav. Already valid JSON (we wrote it with
  // serializeJson), so splice it in verbatim rather than re-parsing it. An empty array
  // means this companion never advertised any, and the dashboard uses its built-in list.
  out["tabs"]   = serialized(gCompanionTabs[0] ? gCompanionTabs : "[]");
  // ...and ours, for the COMPANION's nav.
  JsonArray gw = out["gwTabs"].to<JsonArray>();
  for (size_t i = 0; i < GW_TAB_N; i++) {
    JsonObject e = gw.add<JsonObject>();
    e["id"]    = GW_TAB_ID[i];
    e["label"] = GW_TAB_LBL[i];
  }
  // Sized for the worst case: a full-size companion list (384) + our gwTabs (~230) + the
  // URL (128) + the status (80) + keys. A String would heap-allocate on every heartbeat,
  // and the dashboard polls this endpoint every 4 s.
  char buf[COMPANION_TABS_MAX + 768];
  serializeJson(out, buf, sizeof(buf));
  return httpxSend(r, 200, "application/json", buf);
}

/* ----------------------------------------------------------
   Companion settings blob  (v3.1)

   A stateless companion keeps its settings/playlists/triggers here instead of on
   its own disk. The payload is gzip(minified JSON) whose schema belongs entirely
   to the companion -- the gateway stores the bytes verbatim and never parses them.

   The body is binary (a gzip header carries a NUL at offset 3), so it is streamed
   straight from the socket to a temp file -- one linear recv loop, no whole-body
   buffering. (The WebServer era needed this split across two callbacks with static
   state; native esp_http_server lets it read as what it is.)
---------------------------------------------------------- */
// PUT /api/companion/settings
static esp_err_t handleApiCompanionSettingsPut(httpd_req_t* r) {
  const size_t len = r->content_len;
  // Decide before opening anything, so a bad request never touches flash.
  if (!sfFsReady)                { return httpxErr(r, 503, "No filesystem"); }
  if (len == 0)                  { return httpxErr(r, 400, "Empty or truncated body"); }
  if (len > COMPANION_MAX_BYTES) { return httpxErr(r, 413, "Settings blob too large"); }
  FFat.remove(COMPANION_TMP);                                 // clear a stale temp file
  File f = FFat.open(COMPANION_TMP, "w");
  if (!f) { return httpxErr(r, 507, "Write failed"); }

  size_t recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    // A truncated body (fewer bytes than promised) must not overwrite good settings.
    if (n <= 0) { f.close(); FFat.remove(COMPANION_TMP); return httpxErr(r, 400, "Empty or truncated body"); }
    if (f.write(httpxBuf, (size_t)n) != (size_t)n) { f.close(); FFat.remove(COMPANION_TMP); return httpxErr(r, 507, "Write failed"); }
    recvd += (size_t)n;
  }
  f.close();
  // Publish atomically: the old blob survives intact until the rename lands.
  FFat.remove(COMPANION_FILE);
  if (!FFat.rename(COMPANION_TMP, COMPANION_FILE)) { return httpxErr(r, 507, "Write failed"); }
  DBG("[CFG] Companion settings stored (%u bytes)\n", (unsigned)recvd);
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"bytes\":%u}", (unsigned)recvd);
  return httpxSend(r, 200, "application/json", out);
}

// GET /api/companion/settings -- hand the stored blob back byte-for-byte
static esp_err_t handleApiCompanionSettingsGet(httpd_req_t* r) {
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  if (!sfFsReady || !FFat.exists(COMPANION_FILE)) { httpxErr(r, 404, "No settings stored"); return ESP_OK; }
  File f = FFat.open(COMPANION_FILE, "r");
  if (!f) { httpxErr(r, 404, "No settings stored"); return ESP_OK; }
  size_t n = f.size();
  if (n == 0) { f.close(); httpxErr(r, 404, "No settings stored"); return ESP_OK; }

  wdgWebMs = millis();
  // Deliberately NOT "Content-Encoding: gzip": these bytes are the payload, not a
  // transfer encoding of it. Declaring the encoding would make HTTP clients gunzip
  // the body transparently, and the companion -- which decompresses itself -- would
  // then be handed plain JSON it tries to decompress again.
  httpd_resp_set_type(r, "application/gzip");
  uint8_t buf[512];
  while (size_t got = f.read(buf, sizeof(buf))) {
    httpxChunk(r, (const char*)buf, got);
    wdgWebMs = millis();
  }
  f.close();
  return httpxChunkEnd(r);
}

/* ----------------------------------------------------------
   Raw canvas  (Matrix gateway only; the physical gateway has no framebuffer to expose)
   ----------------------------------------------------------
   Direct pixel control of the HUB75 panel, bypassing the split-flap wall. While a canvas is
   active gCanvasMode makes taskDisplay stand down -- exactly as it does during an OTA -- so the
   handlers below own every pixel. Leaving canvas mode marks the display dirty, and the reel
   renderer repaints the wall from the modules' current state. Nothing here is persisted: a
   reboot returns to the wall. panelPixel/panelFillRect etc. bounds-check, so off-panel writes
   are dropped rather than clamped or crashed.
---------------------------------------------------------- */
// Take the panel over from the reel renderer, and BLOCK until it confirms it has parked. A
// blind delay is not enough: taskDisplay may be mid-repaint when we raise gCanvasMode, and its
// closing panelShow() would swap the wall straight back over our first frame. gDispParked goes
// true only at the top of its loop, once any in-flight repaint has returned -- so waiting for it
// means the next swap on the panel is ours. Bounded, so a wedged display task cannot hang the
// request; feed the web watchdog while we wait.
static void canvasStandDown() {
  gEffect    = EFFECT_NONE;         // a raw canvas supersedes any on-device effect...
  gEffectReq = EFFECT_REQ_IDLE;     // ...and cancels a pending effect start
  gCanvasMode = true;
  gDispParked = false;
  uint32_t t0 = millis();
  while (!gDispParked && (uint32_t)(millis() - t0) < 250) { wdgWebMs = millis(); delay(2); }
}
// Take the panel over (waiting for the renderer to park), then optionally blank it.
// Quiet Time keeps the panel DARK: every canvas CONTENT endpoint refuses while it is on,
// so a companion's canvas apps cannot light the panel (the flap path is suppressed in
// frames.cpp; effects/ticker/anim-play/sound already refuse -- this closes the
// raw-canvas gap: frame/ops/rects/rect/qoi/stream/anim/gif/opsb). v3.7.1
static bool quietBlocked(httpd_req_t* r) {
  if (!gQuietTime) return false;
  httpxErr(r, 409, "Quiet Time is active");
  return true;
}

static void canvasEnter(bool clear) {
  if (!gCanvasMode) canvasStandDown();
  if (clear && gPanel.ready) { panelClear(); panelShow(); }
}
static void canvasLeave() { dispReturnToWall(); }              // hand the panel back (no-op if idle)

/* ---- PUT /api/canvas/stream: persistent TLV draw channel (v3.2) --------------------
   One long-lived PUT carrying draw records back-to-back, executed as they arrive. No
   per-frame HTTP round trip -- and, crucially, no per-frame RESPONSE, so the ~40 ms
   Nagle/delayed-ACK floor on request-response traffic does not apply: the client just
   keeps writing. Record framing (big-endian): u8 type, u24 payload length, payload.
     0x01 frame  u8 fmt (2=rgb565 BE, 3=rgb888) + W*H*bpp pixels    draw, no present
     0x02 rects  the PUT /api/canvas/rects body verbatim            draw, no present
     0x03 ops    a JSON array as POST /api/canvas/ops ("show" op presents)
     0x04 atlas  sheet name to bind (as the "atlas" op)
     0x05 show   present the back buffer
     0x00 end    close: the gateway answers 200 {"ok":true,"records":N} and closes
   The client declares a large placeholder Content-Length (inbound chunked encoding is
   not supported) and just stops at the end record. The handler parks the request with
   httpd_req_async_handler_begin and flips the socket non-blocking; taskWeb's pump
   (canvasStreamPump) drains and executes records so the single httpd worker is never
   captured. One stream at a time; drawing REST endpoints answer 409 while it is open.
   A malformed record aborts the stream (the panel keeps its last frame). */
static bool canvasRectsApply(const uint8_t* body, size_t len, int* outDone);
static int  canvasOpsRun(JsonArrayConst ops, bool* shownOut);
static int  canvasOpsRunBin(const uint8_t* p, size_t len, bool* shownOut, bool* okOut);

static struct {
  httpd_req_t*  req  = nullptr;        // parked async request; non-null = stream open
  uint8_t*      buf  = nullptr;        // record payload accumulator (PSRAM)
  size_t        cap  = 0;
  uint8_t       hdr[4]; uint8_t hdrN = 0;
  uint8_t       type = 0;
  uint32_t      need = 0, got = 0;     // current record payload progress
  bool          inRec = false;
  uint32_t      records = 0;
  unsigned long lastRx = 0;            // idle-abort clock
  // Diagnostics (GET /api/canvas/stream): the pump runs on taskWeb where printf may be
  // unreadable (no USB), so its recent history is inspectable over HTTP.
  uint32_t      ticks = 0;             // pump invocations since open
  int           lastRecv = 99;         // last httpd_req_recv return value
  const char*   lastClose = "-";       // why the previous stream ended
} cs;

bool canvasStreamActive() { return cs.req != nullptr; }

// 409 for the drawing REST endpoints while the stream owns the canvas: interleaved
// one-shot draws would fight the pump for the back buffer mid-record.
static bool csBusy(httpd_req_t* r) {
  if (!cs.req) return false;
  httpxErr(r, 409, "canvas stream active -- close it first");
  return true;
}

static void csSockBlocking(bool block) {
  const int fd = httpd_req_to_sockfd(cs.req);
  if (fd < 0) return;
  const int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, block ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
}

// Tear the stream down. ok: the end record arrived -- answer 200 first (blocking
// socket again for the send). Either way the SESSION is closed, not recycled: the
// placeholder Content-Length was never fulfilled, and httpd would otherwise stall
// trying to purge body bytes that are not coming.
static void csClose(bool ok, const char* why) {
  if (!cs.req) return;
  httpd_req_t*  req = cs.req;
  httpd_handle_t hd = req->handle;
  const int     fd  = httpd_req_to_sockfd(req);
  // The stream is CLOSED as of now: cs.req clears first so the 409 guard releases the
  // REST surface before the drain delay below -- a client that got its 200 and moved
  // straight on to a canvas call must not bounce (the first drain fix held cs.req
  // through the delay and produced exactly those 409s).
  cs.req = nullptr;
  if (cs.buf) { free(cs.buf); cs.buf = nullptr; cs.cap = 0; }
  cs.lastClose = why;
  if (ok) {
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };   // blocking send, bounded
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"records\":%lu}", (unsigned long)cs.records);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    // Let the 200 drain before the session close: an immediate trigger_close raced the
    // response flush on the PSRAM-buffers core and the client saw an RST instead of
    // its 200 (~1 in 15 bursts; 0 in 281 on stock). 30 ms cured most of it but the
    // v3.4.0 soak still saw ~1 in 50 -- 60 ms costs nothing (it runs on the pump's
    // own tick) and the 50-burst close test is the regression gate.
    delay(60);
  }
  httpd_req_async_handler_complete(req);
  if (fd >= 0) httpd_sess_trigger_close(hd, fd);
  printf("[CANVAS] stream closed (%s, %lu records)\n", why, (unsigned long)cs.records);
}

// Execute one complete record (type cs.type, payload cs.buf/cs.need).
// False = protocol error; the pump aborts the stream.
static bool csExec() {
  switch (cs.type) {
    case 0x01: {                                  // full frame: u8 fmt + rows
      if (cs.need < 1) return false;
      const uint8_t bpp  = cs.buf[0];
      const size_t  rowB = (size_t)gPanel.panelW * bpp;
      if ((bpp != 2 && bpp != 3) || cs.need != 1 + rowB * (size_t)gPanel.panelH) return false;
      canvasEnter(false);
      const uint8_t* p = cs.buf + 1;
      for (int y = 0; y < gPanel.panelH; y++, p += rowB) {
        if (bpp == 3) panelBlitRow888(0, y, gPanel.panelW, p);
        else          panelBlitRow565(0, y, gPanel.panelW, p);
      }
      return true;
    }
    case 0x02: return canvasRectsApply(cs.buf, cs.need, nullptr);
    case 0x03: {                                  // ops JSON array
      JsonDocument doc;
      if (deserializeJson(doc, (const char*)cs.buf, cs.need) || !doc.is<JsonArray>()) return false;
      canvasEnter(false);
      canvasOpsRun(doc.as<JsonArrayConst>(), nullptr);   // presents only via its "show" op
      return true;
    }
    case 0x04: {                                  // bind a named atlas sheet
      char name[40];
      const size_t n = min((size_t)cs.need, sizeof(name) - 1);
      memcpy(name, cs.buf, n); name[n] = 0;
      canvasAtlasBind(name);
      return true;
    }
    case 0x05: canvasEnter(false); panelShow(); return true;
    case 0x06: {                                  // binary ops (v3.5): zero-parse draw list
      canvasEnter(false);
      bool okb = true;
      canvasOpsRunBin(cs.buf, cs.need, nullptr, &okb);
      return okb;                                 // malformed binary = protocol error
    }
    default:   return false;                      // unknown type: the framing is not trustworthy
  }
}

// The stream pump: called from taskWeb every tick while a stream is open. Drains and
// executes whatever has arrived, up to a per-tick byte budget so taskWeb's other
// duties (SSE, watchdog cover) keep their cadence.
void canvasStreamPump() {
  if (!cs.req) return;
  if (gOtaInProgress) { csClose(false, "ota"); return; }
  cs.ticks++;
  // Drain EAGERLY, always. A first instinct to heap-grade this drain (like httpxRecv)
  // was measured to be BACKWARDS on this build: the prebuilt lwIP advertises a ~95 KB
  // receive window (see the v2.2.1 OTA incident), so a blasting client's records sit
  // in INTERNAL-heap pbufs until we read them -- reading moves them into the PSRAM
  // record buffer and FREES internal heap. Throttling the drain held those pbufs alive
  // through exactly the troughs it was meant to prevent (soaked to a 3.1 KB watermark).
  size_t budget = 65536;
  while (budget) {
    int n;
    if (!cs.inRec) {                              // collect the 4-byte record header
      n = httpd_req_recv(cs.req, (char*)cs.hdr + cs.hdrN, 4 - cs.hdrN);
      if (n > 0) {
        cs.hdrN += (uint8_t)n;
        if (cs.hdrN == 4) {
          cs.type = cs.hdr[0];
          cs.need = ((uint32_t)cs.hdr[1] << 16) | ((uint32_t)cs.hdr[2] << 8) | cs.hdr[3];
          if (cs.type == 0x00) { csClose(true, "end"); return; }
          if (cs.need > cs.cap) { csClose(false, "oversized record"); return; }
          cs.got = 0; cs.inRec = true;
        }
      }
    } else {
      n = httpd_req_recv(cs.req, (char*)cs.buf + cs.got,
                         min((size_t)(cs.need - cs.got), budget));
      if (n > 0) cs.got += (uint32_t)n;
    }
    cs.lastRecv = n;
    if (n == HTTPD_SOCK_ERR_TIMEOUT) break;       // nothing more right now
    if (n <= 0) { csClose(false, "peer closed"); return; }
    budget -= min((size_t)n, budget);
    cs.lastRx = millis();
    wdgWebMs  = millis();
    if (cs.inRec && cs.got == cs.need) {          // record complete (covers len-0 records)
      cs.inRec = false; cs.hdrN = 0;
      if (!csExec()) {
        printf("[CANVAS] stream: bad record type 0x%02x len %lu\n", cs.type, (unsigned long)cs.need);
        csClose(false, "bad record");
        return;
      }
      cs.records++;
    }
  }
  // Quiet spell: keep httpd's LRU purge off this socket, and give up on a dead client.
  httpd_sess_update_lru_counter(cs.req->handle, httpd_req_to_sockfd(cs.req));
  if (millis() - cs.lastRx > 30000UL) csClose(false, "idle timeout");
}

// POST /api/sound (v3.6) -- tones and note sequences on the board's speaker.
//   {"freq":880,"ms":200,"vol":60}                      one tone
//   {"notes":[[880,120],[0,40],[1320,160]],"vol":60}    a sequence (freq 0 = rest)
//   {"stop":true}                                       stop now
// Refused during Quiet Time, like everything audible/visible. GET reports state.
static esp_err_t handleApiSound(httpd_req_t* r) {
  if (r->method == HTTP_GET) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"available\":%s,\"playing\":%s}",
             soundAvailable() ? "true" : "false", soundPlaying() ? "true" : "false");
    return httpxSend(r, 200, "application/json", buf);
  }
  if (!soundAvailable()) return httpxErr(r, 503, "No speaker codec on this board");
  // The "stop" and GET paths are always allowed; but a PLAY needs the master enable.
  // Checked after parsing "stop" below so a stop request is honoured even when disabled.
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  if (doc["stop"] | false) {
    soundStop();
    return httpxSend(r, 200, "application/json", "{\"ok\":true,\"stopped\":true}");
  }
  if (!cfg.soundEnabled) return httpxErr(r, 403, "Speaker disabled in settings");
  if (gQuietTime) return httpxErr(r, 409, "Quiet Time is active");
  uint16_t f[SOUND_MAX_NOTES], m[SOUND_MAX_NOTES];
  int n = 0;
  if (doc["notes"].is<JsonArrayConst>()) {
    for (JsonVariantConst nv : doc["notes"].as<JsonArrayConst>()) {
      if (n >= SOUND_MAX_NOTES) break;
      int fr = nv[0] | 0, ms = nv[1] | 0;
      if (ms < 1) continue;
      f[n] = (uint16_t)(fr < 0 ? 0 : fr > 8000 ? 8000 : fr);
      m[n] = (uint16_t)(ms > 2000 ? 2000 : ms);
      n++;
    }
  } else {
    const int fr = doc["freq"] | 0, ms = doc["ms"] | 200;
    if (fr < 20 || fr > 8000) return httpxErr(r, 400, "freq must be 20..8000 Hz (or use notes[])");
    f[0] = (uint16_t)fr;
    m[0] = (uint16_t)(ms < 1 ? 1 : ms > 2000 ? 2000 : ms);
    n = 1;
  }
  if (!n) return httpxErr(r, 400, "nothing to play");
  int vol = doc["vol"] | 60;
  if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
  vol = vol * cfg.soundVolume / 100;         // master volume (settings) scales every call
  soundPlay(f, m, n, (uint8_t)vol);
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"queued\":%d}", n);
  return httpxSend(r, 200, "application/json", buf);
}

// GET /api/environment (v3.7) -- the onboard SHTC3 temperature + humidity reading.
static esp_err_t handleApiEnvironment(httpd_req_t* r) {
  float t, h; uint32_t age;
  char buf[128];
  if (sensorAvailable() && sensorRead(t, h, age)) {
    const float off = cfg.tempOffsetC10 / 10.0f;
    const float tc = t + off;                            // calibrated (v3.7)
    const float rhc = envCompRH(t, h, off);              // ambient RH from the same offset
    snprintf(buf, sizeof(buf),
             "{\"available\":true,\"tempC\":%.2f,\"tempF\":%.1f,\"rh\":%.1f,"
             "\"rawTempC\":%.2f,\"rawRH\":%.1f,\"offsetC\":%.1f,\"ageMs\":%lu}",
             tc, tc * 9.0f / 5.0f + 32.0f, rhc, t, h, off, (unsigned long)age);
  }
  else
    snprintf(buf, sizeof(buf), "{\"available\":%s}", sensorAvailable() ? "true" : "false");
  return httpxSend(r, 200, "application/json", buf);
}

// GET /api/canvas/audio -- microphone frontend state (v3.4 diagnostics + discovery):
// whether the ES7210 is present, whether capture is running, and the live features.
// The features are numbers derived from sound, never samples -- nothing recordable.
static esp_err_t handleApiCanvasAudio(httpd_req_t* r) {
  AudioFrame a;
  audioRead(a);
  char buf[320];
  int n = snprintf(buf, sizeof(buf),
           "{\"available\":%s,\"capturing\":%s,\"seq\":%lu,\"level\":%.3f,\"peak\":%.3f,"
           "\"beat\":%s,\"bassRaw\":%.4f,\"bands\":[",
           audioAvailable() ? "true" : "false", audioCapturing() ? "true" : "false",
           (unsigned long)a.seq, a.level, a.peak, a.beat ? "true" : "false", a.bassRaw);
  for (int b = 0; b < AUDIO_BANDS; b++)
    n += snprintf(buf + n, sizeof(buf) - n, "%s%.2f", b ? "," : "", a.bands[b]);
  snprintf(buf + n, sizeof(buf) - n, "]}");
  return httpxSend(r, 200, "application/json", buf);
}

// GET /api/canvas/stream -- stream channel state (diagnostics + client discovery).
static esp_err_t handleApiCanvasStreamGet(httpd_req_t* r) {
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"open\":%s,\"records\":%lu,\"ticks\":%lu,\"lastRecv\":%d,"
           "\"inRec\":%s,\"need\":%lu,\"got\":%lu,\"lastClose\":\"%s\"}",
           cs.req ? "true" : "false", (unsigned long)cs.records, (unsigned long)cs.ticks,
           cs.lastRecv, cs.inRec ? "true" : "false", (unsigned long)cs.need,
           (unsigned long)cs.got, cs.lastClose);
  return httpxSend(r, 200, "application/json", buf);
}

// PUT /api/canvas/stream -- open the channel (see the block comment above).
static esp_err_t handleApiCanvasStream(httpd_req_t* r) {
  if (!gPanel.ready)     return httpxErr(r, 503, "Panel not running");
  if (quietBlocked(r)) return ESP_OK;
  if (cs.req)            return httpxErr(r, 409, "a stream is already open");
  if (gOtaInProgress)    return httpxErr(r, 503, "OTA in progress");
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP)
    return httpxErr(r, 507, "Low on memory -- try again in a moment");
  const size_t cap = (size_t)gPanel.panelW * gPanel.panelH * 3 * 2 + 4 + 256 * 8;  // = rects cap, covers a frame
  cs.buf = (uint8_t*)ps_malloc(cap);
  if (!cs.buf) cs.buf = (uint8_t*)malloc(cap);
  if (!cs.buf) return httpxErr(r, 503, "Out of memory");
  cs.cap = cap; cs.hdrN = 0; cs.inRec = false; cs.records = 0; cs.lastRx = millis();
  httpd_req_t* async = nullptr;
  if (httpd_req_async_handler_begin(r, &async) != ESP_OK) {
    free(cs.buf); cs.buf = nullptr; cs.cap = 0;
    return httpxErr(r, 503, "async unavailable");
  }
  cs.req = async;                     // the socket now belongs to the pump (taskWeb)
  csSockBlocking(false);
  canvasEnter(false);
  printf("[CANVAS] stream open\n");
  return ESP_OK;
}

// The bundled face closest to a requested pixel height; 6x10 is the readable default.
static const Font1252* canvasFace(int size) {
  switch (size) {
    case 20: return &FONT_10x20;
    case 18: return &FONT_9x18;
    case 13: return &FONT_8x13;
    case 9:  return &FONT_6x9;
    case 8:  return &FONT_5x8;
    default: return &FONT_6x10;
  }
}
// One string at (x,y) top-left, one flap-font glyph per CHARACTER, solid colour. The
// input is UTF-8 (it came out of JSON) and is transcoded to CP1252 first (v3.0.1) --
// walking raw bytes drew a multi-byte "\u00b0" as two garbage glyphs, which is why the
// companion had been ASCII-only in its ops apps. Fixed-width faces, so the advance is
// just the face width; bit 15 of each row is the leftmost column.
static void canvasText(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b,
                       const Font1252* f) {
  char enc[256];
  utf8ToCp1252(s, enc, sizeof(enc));
  int cx = x;
  for (const uint8_t* p = (const uint8_t*)enc; *p; ++p) {
    dispDrawGlyph1252(cx, y, f, *p, 0, 255, r, g, b);
    cx += f->width;
  }
}
// A [r,g,b] triple from an op field, leaving the caller's defaults untouched when absent.
// Like canvasColor, but reports whether a colour was actually present -- for optional
// style params (outline/shadow) where absence means "don't draw that layer".
static bool canvasColorGet(JsonVariantConst c, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!c.is<JsonArrayConst>() || c.size() < 3) return false;
  r = (uint8_t)c[0].as<int>(); g = (uint8_t)c[1].as<int>(); b = (uint8_t)c[2].as<int>();
  return true;
}

static int gOpsBlend = 0;   // batch blend mode (v3.8): 0 over 1 add 2 multiply 3 screen 4 max
static uint8_t gColorAlpha = 255;   // v3.8: last colour alpha, for AA coverage base
static void canvasColor(JsonVariantConst c, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (c.is<JsonArrayConst>() && c.size() >= 3) {
    r = (uint8_t)c[0].as<int>(); g = (uint8_t)c[1].as<int>(); b = (uint8_t)c[2].as<int>();
    // v3.8: an optional 4th element is alpha 0..255 -- composite over the back buffer,
    // combined with the batch blend mode. Absent -> opaque (still honours the mode).
    const uint8_t a = (c.size() >= 4) ? (uint8_t)c[3].as<int>() : 255;
    gColorAlpha = a;
    panelSetBlend((uint8_t)gOpsBlend, a);
  }
}

// Decode one accumulated pixel (rgb888 or rgb565 big-endian, by bpp) to r,g,b.
// Shared by the full-frame and rect upload paths, which stream in arbitrary
// chunk boundaries and so carry partial pixels between writes.
static void pxDecode(const uint8_t* c, uint8_t bpp, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (bpp == 3) { r = c[0]; g = c[1]; b = c[2]; return; }
  const uint16_t v = ((uint16_t)c[0] << 8) | c[1];
  r = (uint8_t)(((v >> 11) & 0x1F) << 3);
  g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
  b = (uint8_t)(( v        & 0x1F) << 3);
}

// GET  /api/canvas -> {active,width,height,formats}   POST {"active":bool} take over / release.
static esp_err_t handleApiCanvas(httpd_req_t* r) {
  if (r->method == HTTP_POST) {
    if (csBusy(r)) return ESP_OK;
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (doc["active"] | false) canvasEnter(true); else canvasLeave();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"active\":%s}", gCanvasMode ? "true" : "false");
    return httpxSend(r, 200, "application/json", buf);
    return ESP_OK;
  }
  // The atlas field (v3.1): the sticky-bound sheet and every resident one.
  char atlas[320];
  canvasAtlasStateJson(atlas, sizeof(atlas));
  char buf[560];
  snprintf(buf, sizeof(buf),
           "{\"active\":%s,\"width\":%u,\"height\":%u,\"formats\":[\"rgb888\",\"rgb565\",\"qoi\"],"
           "\"effect\":\"%s\",\"anim\":%s,\"ticker\":%s,\"atlas\":%s,\"effects\":%s}",
           gCanvasMode ? "true" : "false", (unsigned)gPanel.panelW, (unsigned)gPanel.panelH,
           effectName(gEffect), gAnimActive ? "true" : "false", gTickerActive ? "true" : "false",
           atlas, effectListJson());
  return httpxSend(r, 200, "application/json", buf);
}

// POST /api/canvas/effect  {"type":"plasma|fire|matrix|none","speed":1..10}
// Start an on-device effect (rendered by taskDisplay at the panel's native rate) or, with
// "none", return to the wall. Supersedes raw-canvas mode -- the display task, not HTTP, owns the
// panel -- so it clears gCanvasMode too.
static esp_err_t handleApiCanvasEffect(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  uint8_t e = effectByName(doc["type"] | "none");
  // Refuse BEFORE touching any parameter: a request rejected for Quiet Time must
  // not leave its speed/hue/density behind for the next start to inherit.
  if (e != EFFECT_NONE && gQuietTime) {
    httpxErr(r, 409, "Quiet Time is active"); return ESP_OK;   // don't light the panel during quiet hours
  }
  int sp = doc["speed"] | (int)gEffectSpeed;
  gEffectSpeed = (uint8_t)(sp < 1 ? 1 : sp > 10 ? 10 : sp);
  // Optional per-start overrides; absent -> -1 -> the effect keeps its own look (see effects.h).
  int hv = doc["hue"].is<int>()     ? (int)doc["hue"]     : -1;
  int dv = doc["density"].is<int>() ? (int)doc["density"] : -1;
  gEffectHue     = (hv < 0) ? -1 : (hv > 255 ? 255 : hv);
  gEffectDensity = (dv < 0) ? -1 : (dv < 1 ? 1 : (dv > 100 ? 100 : dv));
  // "audio":true (v3.4): the mic modulates this effect (fire/matrix/plasma react to
  // level and beats). Explicit per start, like hue/density; a plain start turns it off.
  gEffectAudioMod = doc["audio"] | false;
  if (gEffectAudioMod && e != EFFECT_NONE) audioMaybeStart();
  if (e == EFFECT_NONE) {
    dispReturnToWall();             // stop -> reel wall
  } else {
    gCanvasMode = false;            // an effect owns the panel via taskDisplay, which runs
    gEffectReq  = e;                // effectReset() + starts it -- no effect state touched off-core
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"effect\":\"%s\",\"speed\":%u,\"hue\":%d,\"density\":%d,\"audio\":%s}",
           effectName(e), (unsigned)gEffectSpeed, gEffectHue, gEffectDensity,
           gEffectAudioMod ? "true" : "false");
  return httpxSend(r, 200, "application/json", buf);
}

// A base64 sprite op: {op:"image", x, y, w, h, fmt:"rgb888"|"rgb565", data:"<base64>"}. Decoded into
// PSRAM (bounded by CANVAS_OPS_IMG_MAX) and blitted at (x,y); silently skipped if oversized or bad.
// For a full-panel picture use PUT /api/canvas/frame -- this op is for small sprites in an ops batch.
#define CANVAS_OPS_IMG_MAX  8192
static void canvasOpImage(JsonVariantConst op, int x, int y, int w, int h) {
  const char* data = op["data"] | "";
  size_t b64len = strlen(data);
  const bool rgb565 = !strcmp(op["fmt"] | "rgb888", "rgb565");
  const int  bpp    = rgb565 ? 2 : 3;
  const long need   = (long)w * h * bpp;
  if (w <= 0 || h <= 0 || need <= 0 || need > CANVAS_OPS_IMG_MAX || b64len < 4 || b64len > 16384) return;
  size_t cap = (b64len / 4) * 3 + 4;
  uint8_t* pix = (uint8_t*)ps_malloc(cap);
  if (!pix) pix = (uint8_t*)malloc(cap);
  if (!pix) return;
  size_t olen = 0;
  if (mbedtls_base64_decode(pix, cap, &olen, (const uint8_t*)data, b64len) == 0 && (long)olen >= need) {
    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++) {
        const uint8_t* px = pix + ((size_t)row * w + col) * bpp;
        uint8_t rr, gg, bb;
        if (rgb565) { uint16_t v = ((uint16_t)px[0] << 8) | px[1];
                      rr = (uint8_t)(((v >> 11) & 0x1F) << 3); gg = (uint8_t)(((v >> 5) & 0x3F) << 2); bb = (uint8_t)((v & 0x1F) << 3); }
        else { rr = px[0]; gg = px[1]; bb = px[2]; }
        panelPixel(x + col, y + row, rr, gg, bb);
      }
  }
  free(pix);
}

// Fill (x,y,w,h) with a linear gradient from `from` to `to`, vertical unless horizontal.
// 4x4 Bayer threshold matrix for ordered dithering: at 3-4 bitplanes a smooth ramp
// quantises into visible bands; adding a position-dependent sub-step offset before the
// panel quantises breaks each band edge into a fine checker that reads as a smooth
// blend at viewing distance. Default ON for gradients since v3.6 ("dither":false opts out).
static const int8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint8_t ditherCh(int c, int x, int y, int q) {
  c += ((BAYER4[((y & 3) << 2) | (x & 3)] - 8) * q) >> 4;
  return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

// Gradient fill: mode 0 = vertical, 1 = horizontal, 2 = radial (centre -> corners),
// 3 = angled (angleDeg: 0 = left->right, 90 = top->bottom, any degree between).
static void canvasOpGradientEx(int x, int y, int w, int h,
                               uint8_t r0, uint8_t g0, uint8_t b0,
                               uint8_t r1, uint8_t g1, uint8_t b1,
                               int mode, int angleDeg, bool dither) {
  if (w <= 0 || h <= 0) return;
  const int q = 256 >> panelInfo().depth;                  // one quantisation step
  if (mode <= 1 && !dither) {                              // the classic strip fast path
    const bool vertical = (mode == 0);
    const int n = vertical ? h : w, den = (n > 1) ? (n - 1) : 1;
    for (int i = 0; i < n; i++) {
      const uint8_t r = (uint8_t)((int)r0 + ((int)r1 - (int)r0) * i / den);
      const uint8_t g = (uint8_t)((int)g0 + ((int)g1 - (int)g0) * i / den);
      const uint8_t b = (uint8_t)((int)b0 + ((int)b1 - (int)b0) * i / den);
      if (vertical) panelHLine(x, y + i, w, r, g, b);
      else          panelVLine(x + i, y, h, r, g, b);
    }
    return;
  }
  // per-pixel paths (dithered linear, radial, angled)
  float ax = 0, ay = 0, tmin = 0, tspan = 1;
  if (mode == 3) {
    const float rad = (float)angleDeg * 0.0174533f;
    ax = cosf(rad); ay = sinf(rad);
    // normalise the projection over the box's corners
    float t00 = 0, t10 = (w - 1) * ax, t01 = (h - 1) * ay, t11 = t10 + t01;
    tmin = fminf(fminf(t00, t10), fminf(t01, t11));
    tspan = fmaxf(fmaxf(t00, t10), fmaxf(t01, t11)) - tmin;
    if (tspan < 1e-3f) tspan = 1;
  }
  const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
  const float maxR = sqrtf(cx * cx + cy * cy);
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++) {
      int t8;
      switch (mode) {
        case 0:  t8 = (h > 1) ? 255 * yy / (h - 1) : 0; break;
        case 1:  t8 = (w > 1) ? 255 * xx / (w - 1) : 0; break;
        case 2: {
          const float dx = xx - cx, dy = yy - cy;
          t8 = (int)(255.0f * sqrtf(dx * dx + dy * dy) / (maxR > 0 ? maxR : 1));
          break;
        }
        default: t8 = (int)(255.0f * ((xx * ax + yy * ay) - tmin) / tspan); break;
      }
      if (t8 < 0) t8 = 0; else if (t8 > 255) t8 = 255;
      int r = r0 + ((r1 - r0) * t8) / 255;
      int g = g0 + ((g1 - g0) * t8) / 255;
      int b = b0 + ((b1 - b0) * t8) / 255;
      if (dither) {
        panelPixel(x + xx, y + yy, ditherCh(r, x + xx, y + yy, q),
                   ditherCh(g, x + xx, y + yy, q), ditherCh(b, x + xx, y + yy, q));
      } else {
        panelPixel(x + xx, y + yy, (uint8_t)r, (uint8_t)g, (uint8_t)b);
      }
    }
}

static void canvasOpGradientRaw(int x, int y, int w, int h,
                                uint8_t r0, uint8_t g0, uint8_t b0,
                                uint8_t r1, uint8_t g1, uint8_t b1, bool vertical) {
  if (w <= 0 || h <= 0) return;
  const int n = vertical ? h : w, den = (n > 1) ? (n - 1) : 1;
  for (int i = 0; i < n; i++) {
    uint8_t r = (uint8_t)((int)r0 + ((int)r1 - (int)r0) * i / den);
    uint8_t g = (uint8_t)((int)g0 + ((int)g1 - (int)g0) * i / den);
    uint8_t b = (uint8_t)((int)b0 + ((int)b1 - (int)b0) * i / den);
    if (vertical) panelHLine(x, y + i, w, r, g, b);
    else          panelVLine(x + i, y, h, r, g, b);
  }
}
static void canvasOpGradient(int x, int y, int w, int h, JsonVariantConst from, JsonVariantConst to, bool vertical) {
  uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
  canvasColor(from, r0, g0, b0); canvasColor(to, r1, g1, b1);
  canvasOpGradientRaw(x, y, w, h, r0, g0, b0, r1, g1, b1, vertical);
}

// A polyline: connect consecutive [x,y] points with straight lines.
static void canvasOpPolyline(JsonVariantConst pts, uint8_t r, uint8_t g, uint8_t b) {
  int px = 0, py = 0; bool have = false;
  for (JsonVariantConst pt : pts.as<JsonArrayConst>()) {
    int x = pt[0] | 0, y = pt[1] | 0;
    if (have) panelLine(px, py, x, y, r, g, b);
    px = x; py = y; have = true;
  }
}

// POST /api/canvas/transition {"type":"none|crossfade|wipe|slide","ms":100..2000}
// Configures how subsequent full-frame canvas PUTs present. Sticky until changed;
// runtime-only (a reboot returns to hard cuts).
static esp_err_t handleApiCanvasTransition(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* t = doc["type"] | "none";
  uint8_t ty;
  if      (!strcmp(t, "none"))      ty = 0;
  else if (!strcmp(t, "crossfade")) ty = 1;
  else if (!strcmp(t, "wipe"))      ty = 2;
  else if (!strcmp(t, "slide"))     ty = 3;
  else { httpxErr(r, 400, "type must be none|crossfade|wipe|slide"); return ESP_OK; }
  int ms = doc["ms"] | 400;
  if (ms < 100) ms = 100;
  if (ms > 2000) ms = 2000;
  gTransType = ty; gTransMs = (uint16_t)ms;
  cfg.transType = ty; cfg.transMs = (uint16_t)ms;   // persist (v3.7.2): survive reboot/reflash
  saveConfig();
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"type\":\"%s\",\"ms\":%d}", t, ms);
  return httpxSend(r, 200, "application/json", resp);
}

// POST /api/system/reboot -- clean remote restart (v2.2.3). Born of a bench session
// where "please power-cycle it" needed human hands: geometry changes, a wedged
// peripheral, or a committed-but-unbooted OTA all want this. Replies first, then
// reboots via the same deliver-response-then-restart path the OTA upload uses
// (gOtaRebootPending: taskWeb restarts only after the 200 has been flushed).
static esp_err_t handleApiSystemReboot(httpd_req_t* r) {
  logCommand('R', "reboot");
  httpxSend(r, 200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  gOtaRebootPending = true;
  return ESP_OK;
}

// ---- animation library (v2.1) -------------------------------------------------------
// Map a canvasAnim* return code onto the error surface.
static esp_err_t animRcReply(httpd_req_t* r, int rc, const char* okBody) {
  switch (rc) {
    case 0:   return httpxSend(r, 200, "application/json", okBody);
    case 400: return httpxErr(r, 400, "Bad name (1-24 chars a-z 0-9 - _) or bad/truncated file");
    case 404: return httpxErr(r, 404, "No such animation");
    case 409: return httpxErr(r, 409, "Nothing loaded to save -- upload an animation first");
    case 413: return httpxErr(r, 413, "Animation exceeds the store limit");
    case 507: return httpxErr(r, 507, "Filesystem full or write failed");
    default:  return httpxErr(r, 503, "Filesystem or memory unavailable");
  }
}

// POST /api/canvas/anim/save {"name":"x"} -- persist the currently loaded store to FATFS.
static esp_err_t handleApiAnimSave(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* name = doc["name"] | "";
  { char cd[64]; snprintf(cd, sizeof(cd), "anim save '%.24s'", name); logCommand('R', cd); }
  return animRcReply(r, canvasAnimSave(name), "{\"ok\":true}");
}

// POST /api/canvas/anim/play {"name":"x"} -- load a library animation and play it.
static esp_err_t handleApiAnimPlay(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  if (gQuietTime)    { httpxErr(r, 409, "Quiet Time is active"); return ESP_OK; }
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* name = doc["name"] | "";
  { char cd[64]; snprintf(cd, sizeof(cd), "anim play '%.24s'", name); logCommand('R', cd); }
  char okBody[96];
  snprintf(okBody, sizeof(okBody), "{\"ok\":true,\"frames\":%u}", (unsigned)canvasAnimCount());
  int rc = canvasAnimLoadPlay(name);
  if (rc == 0)
    snprintf(okBody, sizeof(okBody), "{\"ok\":true,\"frames\":%u}", (unsigned)canvasAnimCount());
  return animRcReply(r, rc, okBody);
}

// POST /api/canvas/anim/delete {"name":"x"}
static esp_err_t handleApiAnimDelete(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* name = doc["name"] | "";
  { char cd[64]; snprintf(cd, sizeof(cd), "anim delete '%.24s'", name); logCommand('R', cd); }
  return animRcReply(r, canvasAnimDelete(name), "{\"ok\":true}");
}

// GET /api/canvas/anims -- the library, streamed.
static void animListSink(const char* frag) { httpxChunkStr(gStreamReq, frag); wdgWebMs = millis(); }
static esp_err_t handleApiAnimList(httpd_req_t* r) {
  httpd_resp_set_type(r, "application/json");
  gStreamReq = r;
  canvasAnimList(animListSink);
  return httpxChunkEnd(r);
}

// POST /api/canvas/ops -- a JSON array of draw commands, applied in order then presented. Ops: clear
// | pixel | hline | vline | line | rect(+fill) | circle(+fill) | ellipse(+fill) | triangle(+fill) |
// roundrect(+fill) | gradient | polyline | text(+align) | image | scroll | show. Colours are [r,g,b],
// default white (black for clear). Auto-takes-over the panel.
// Execute a JSON array of drawing ops against the back buffer. Returns the count
// applied; *shown reports whether the batch contained an explicit "show". Shared by
// POST /api/canvas/ops and the stream channel's ops record (v3.2). Caller must have
// entered canvas mode (canvasEnter) first.
/* ---- v3.5 ops helpers: thickness, arcs, filled polygons, textbox ------------------ */
// Batch-scoped drawing state for the "origin" and "clip" ops. Reset at every
// canvasOpsRun entry AND exit so no other draw path can inherit them.
static int  gOpsOx = 0, gOpsOy = 0;
static int  gOpsClip[4];
static bool gOpsClipOn = false;

static void opsApplyClip() {
  if (gOpsClipOn) panelSetClip(gOpsClip[0], gOpsClip[1], gOpsClip[2], gOpsClip[3]);
  else            panelClearClip();
}

// Bresenham with a w x w filled-square brush -- reads as a solid thick stroke at
// panel resolutions and costs one fast fillRect per step.
static void canvasThickLine(int x0, int y0, int x1, int y1, int t,
                            uint8_t r, uint8_t g, uint8_t b) {
  if (t <= 1) { panelLine(x0, y0, x1, y1, r, g, b); return; }
  const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int off = t >> 1;
  for (;;) {
    panelFillRect(x0 - off, y0 - off, t, t, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// --- Anti-aliased drawing (v3.8): rides the blend path, passing edge coverage as alpha
// (combined with the op's own colour alpha). AA is for thin strokes/curves where jaggies
// show; thick strokes stay hard-edged. --------------------------------------------------
static inline float aa_fpart(float x)  { return x - floorf(x); }
static inline float aa_rfpart(float x) { return 1.0f - aa_fpart(x); }

static void canvasAAPlot(int x, int y, float cov, uint8_t r, uint8_t g, uint8_t b) {
  if (cov <= 0.003f) return;
  if (cov > 1.0f) cov = 1.0f;
  panelSetBlend((uint8_t)gOpsBlend, (uint8_t)(cov * gColorAlpha));
  panelPixel(x, y, r, g, b);
}

// Xiaolin Wu's line.
static void canvasAALine(float x0, float y0, float x1, float y1,
                         uint8_t r, uint8_t g, uint8_t b) {
  const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
  if (steep) { float t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
  if (x0 > x1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  const float dx = x1 - x0, dy = y1 - y0;
  const float grad = (dx == 0.0f) ? 1.0f : dy / dx;
  #define AAPUT(px, py, c) do { if (steep) canvasAAPlot((int)(py), (int)(px), (c), r, g, b);                                 else        canvasAAPlot((int)(px), (int)(py), (c), r, g, b); } while (0)
  float xend = roundf(x0), yend = y0 + grad * (xend - x0), xgap = aa_rfpart(x0 + 0.5f);
  const int xpxl1 = (int)xend; const float ypxl1 = floorf(yend);
  AAPUT(xpxl1, ypxl1,     aa_rfpart(yend) * xgap);
  AAPUT(xpxl1, ypxl1 + 1, aa_fpart(yend)  * xgap);
  float intery = yend + grad;
  xend = roundf(x1); yend = y1 + grad * (xend - x1); xgap = aa_fpart(x1 + 0.5f);
  const int xpxl2 = (int)xend; const float ypxl2 = floorf(yend);
  for (int x = xpxl1 + 1; x < xpxl2; x++) {
    AAPUT(x, floorf(intery),     aa_rfpart(intery));
    AAPUT(x, floorf(intery) + 1, aa_fpart(intery));
    intery += grad;
  }
  AAPUT(xpxl2, ypxl2,     aa_rfpart(yend) * xgap);
  AAPUT(xpxl2, ypxl2 + 1, aa_fpart(yend)  * xgap);
  #undef AAPUT
}

// AA circle outline (1 px ring, coverage from radial distance).
static void canvasAACircle(int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b) {
  if (rad < 1) return;
  for (int y = -rad - 1; y <= rad + 1; y++)
    for (int x = -rad - 1; x <= rad + 1; x++) {
      const float d = sqrtf((float)(x * x + y * y));
      const float cov = 1.0f - fabsf(d - (float)rad);
      if (cov > 0.02f) canvasAAPlot(cx + x, cy + y, cov, r, g, b);
    }
}

// Bezier (quadratic: 3 points, cubic: 4) flattened to segments, drawn AA or thick.
static void canvasBezier(const int* vx, const int* vy, int n, int t, bool aa,
                         uint8_t r, uint8_t g, uint8_t b) {
  if (n < 3) return;
  // segment count from a rough control-polygon length
  float len = 0;
  for (int i = 1; i < n; i++) len += sqrtf((float)((vx[i]-vx[i-1])*(vx[i]-vx[i-1]) +
                                                    (vy[i]-vy[i-1])*(vy[i]-vy[i-1])));
  int seg = (int)(len / 3.0f); if (seg < 6) seg = 6; if (seg > 96) seg = 96;
  float px = vx[0], py = vy[0];
  for (int i = 1; i <= seg; i++) {
    const float u = (float)i / seg, v = 1.0f - u;
    float qx, qy;
    if (n == 3) { qx = v*v*vx[0] + 2*v*u*vx[1] + u*u*vx[2];
                  qy = v*v*vy[0] + 2*v*u*vy[1] + u*u*vy[2]; }
    else        { qx = v*v*v*vx[0] + 3*v*v*u*vx[1] + 3*v*u*u*vx[2] + u*u*u*vx[3];
                  qy = v*v*v*vy[0] + 3*v*v*u*vy[1] + 3*v*u*u*vy[2] + u*u*u*vy[3]; }
    if (aa) canvasAALine(px, py, qx, qy, r, g, b);
    else    canvasThickLine((int)px, (int)py, (int)qx, (int)qy, t, r, g, b);
    px = qx; py = qy;
  }
}

// --- Stroke styling (v3.8.1): line caps, round joins, dashes. ------------------------
// A thick line with an end cap: 0 butt (default), 1 round (disc at each end), 2 square
// (extend each end by t/2 along the line). Caps matter only for t>1.
static void canvasThickLineCap(float x0, float y0, float x1, float y1, int t, int cap,
                               uint8_t r, uint8_t g, uint8_t b) {
  if (t > 1 && cap == 2) {                          // square: extend the ends
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx*dx + dy*dy);
    if (len > 0.001f) { const float ex = dx/len*(t/2.0f), ey = dy/len*(t/2.0f);
                        x0 -= ex; y0 -= ey; x1 += ex; y1 += ey; }
  }
  canvasThickLine((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(x1), (int)lroundf(y1), t, r, g, b);
  if (t > 1 && cap == 1) {                          // round: a filled disc at each end
    panelCircle((int)lroundf(x0), (int)lroundf(y0), t/2, true, r, g, b);
    panelCircle((int)lroundf(x1), (int)lroundf(y1), t/2, true, r, g, b);
  }
}

// A dashed thick line following the pattern (on,off) px. `phase` is distance already
// consumed into the pattern (threaded across polyline segments so dashes flow round the
// path); returns the ending phase. cap applies to each dash's ends.
static float canvasDashLine(float x0, float y0, float x1, float y1, int t, int cap,
                            float on, float off, float phase,
                            uint8_t r, uint8_t g, uint8_t b) {
  if (on <= 0) { canvasThickLineCap(x0, y0, x1, y1, t, cap, r, g, b); return phase; }
  const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx*dx + dy*dy);
  if (len < 0.001f) return phase;
  const float ux = dx/len, uy = dy/len, period = on + off;
  float d = 0, p = fmodf(phase, period);
  while (d < len) {
    const bool onNow = p < on;
    const float remain = onNow ? (on - p) : (period - p);
    const float seg = (remain < len - d) ? remain : (len - d);
    if (onNow) canvasThickLineCap(x0 + ux*d, y0 + uy*d, x0 + ux*(d+seg), y0 + uy*(d+seg),
                                  t, cap, r, g, b);
    d += seg; p += seg; if (p >= period) p -= period;
  }
  return p;
}

// Parse the optional "cap"/"join"/"dash" style off an op. cap/join: 0 butt/miter, 1
// round, 2 square. dash: [on,off] -> on>0 means dashed. Returns whether dashed.
static bool canvasStrokeStyle(JsonVariantConst op, int& cap, int& join, float& on, float& off) {
  const char* c = op["cap"] | "butt";
  cap = !strcmp(c, "round") ? 1 : !strcmp(c, "square") ? 2 : 0;
  const char* j = op["join"] | "miter";
  join = !strcmp(j, "round") ? 1 : 0;
  on = off = 0;
  if (op["dash"].is<JsonArrayConst>()) {
    JsonArrayConst d = op["dash"].as<JsonArrayConst>();
    if (d.size() >= 1) on  = (float)(d[0].as<int>());
    off = (d.size() >= 2) ? (float)(d[1].as<int>()) : on;
  }
  return on > 0;
}

// Arc / pie: 0 degrees = 12 o'clock, clockwise (the gauge convention). Outline mode
// draws the annulus [rad-t+1 .. rad]; fill draws the whole pie slice. Bounding-box
// scan with one atan2f per candidate pixel -- ops are one-shot draws, not per-frame.
static void canvasArc(int cx, int cy, int rad, int t, int a0, int a1, bool fill,
                      uint8_t r, uint8_t g, uint8_t b) {
  if (rad < 1) return;
  if (t < 1) t = 1;
  if (t > rad) t = rad;
  while (a0 < 0)  { a0 += 360; a1 += 360; }
  while (a1 < a0) a1 += 360;
  if (a1 - a0 > 360) a1 = a0 + 360;
  const long r2o = (long)rad * rad;
  const long r2i = fill ? -1 : (long)(rad - t) * (rad - t);
  for (int yy = -rad; yy <= rad; yy++)
    for (int xx = -rad; xx <= rad; xx++) {
      const long d2 = (long)xx * xx + (long)yy * yy;
      if (d2 > r2o || d2 <= r2i) continue;
      float a = atan2f((float)xx, (float)-yy) * 57.295780f;    // 0 up, CW positive
      if (a < 0) a += 360.0f;
      if (a < (float)a0) a += 360.0f;
      if (a > (float)a1) continue;
      panelPixel(cx + xx, cy + yy, r, g, b);
    }
}

// Even-odd scanline fill for a closed polygon (up to 16 vertices).
static void canvasPolyFillPts(const int* vx, const int* vy, int n,
                              uint8_t r, uint8_t g, uint8_t b);
static void canvasPolyFill(JsonArrayConst pts, uint8_t r, uint8_t g, uint8_t b) {
  int vx[16], vy[16], n = 0;
  for (JsonVariantConst p : pts) {
    if (n >= 16) break;
    vx[n] = (int)p[0] + gOpsOx; vy[n] = (int)p[1] + gOpsOy; n++;
  }
  canvasPolyFillPts(vx, vy, n, r, g, b);
}
static void canvasPolyFillPts(const int* vx, const int* vy, int n,
                              uint8_t r, uint8_t g, uint8_t b) {
  if (n < 3) return;
  int minY = vy[0], maxY = vy[0];
  for (int i = 1; i < n; i++) { if (vy[i] < minY) minY = vy[i]; if (vy[i] > maxY) maxY = vy[i]; }
  for (int y = minY; y <= maxY; y++) {
    int xs[16], k = 0;
    for (int i = 0; i < n; i++) {
      const int j = (i + 1) % n;
      if ((vy[i] <= y && vy[j] > y) || (vy[j] <= y && vy[i] > y))
        xs[k++] = vx[i] + (int)((long)(y - vy[i]) * (vx[j] - vx[i]) / (vy[j] - vy[i]));
    }
    for (int a = 1; a < k; a++) {                 // insertion sort (k <= 16)
      const int v = xs[a]; int b2 = a - 1;
      while (b2 >= 0 && xs[b2] > v) { xs[b2 + 1] = xs[b2]; b2--; }
      xs[b2 + 1] = v;
    }
    for (int a = 0; a + 1 < k; a += 2) panelHLine(xs[a], y, xs[a + 1] - xs[a] + 1, r, g, b);
  }
}

// Word-wrapped, aligned text inside a box, clipped to it. halign/valign: 0 start,
// 1 centre, 2 end. Respects explicit newlines; a word longer than the box hard-breaks.
static void canvasTextBox(int x, int y, int w, int h, const char* s, const Font1252* f,
                          int halign, int valign, uint8_t r, uint8_t g, uint8_t b) {
  char enc[384];
  utf8ToCp1252(s, enc, sizeof(enc));
  const int cw = f->width, lh = f->height + 1;
  const int maxCols = cw ? w / cw : 0;
  if (maxCols < 1 || h < f->height) return;
  struct { int start, len; } lines[10];
  int nl = 0, i = 0;
  const int len = (int)strlen(enc);
  while (i < len && nl < 10) {
    if (enc[i] == ' ') { i++; continue; }
    int end = i, lastSp = -1;
    while (end < len && enc[end] != '\n' && end - i < maxCols) {
      if (enc[end] == ' ') lastSp = end;
      end++;
    }
    int next;
    if (end >= len)            next = end;
    else if (enc[end] == '\n') next = end + 1;
    else if (lastSp > i)     { end = lastSp; next = lastSp + 1; }
    else                       next = end;
    lines[nl].start = i; lines[nl].len = end - i; nl++;
    i = next;
  }
  const int totH = nl * lh - 1;
  int ty = y + (valign == 1 ? (h - totH) / 2 : valign == 2 ? h - totH : 0);
  panelSetClip(x, y, x + w, y + h);
  for (int L = 0; L < nl; L++) {
    const int tw2 = lines[L].len * cw;
    const int tx = x + (halign == 1 ? (w - tw2) / 2 : halign == 2 ? w - tw2 : 0);
    for (int c2 = 0; c2 < lines[L].len; c2++)
      dispDrawGlyph1252(tx + c2 * cw, ty, f, (uint8_t)enc[lines[L].start + c2], 0, 255, r, g, b);
    ty += lh;
  }
  opsApplyClip();                                  // back to the batch's own clip (or none)
}

static int alignIdx(const char* a, const char* mid) {   // "left/top"=0, mid=1, "right/bottom"=2
  if (!strcmp(a, mid)) return 1;
  if (!strcmp(a, "right") || !strcmp(a, "bottom")) return 2;
  return 0;
}

/* ---- binary ops (v3.5): the zero-parse path for game-rate clients -----------------
   A fixed-layout encoding of the ops surface: 4-6x smaller than JSON on the wire and
   ~zero decode cost (the JSON path spends 5-8 ms deserializing a rich frame). Carried
   by stream record 0x06 and by POST /api/canvas/opsb; advertised as canvas.opsBin
   (a format version, currently 1).

   All integers big-endian; coordinates SIGNED int16 (games draw off-panel; the panel
   primitives clip). Each op: u8 opcode + fixed fields. STRICT decode: an unknown
   opcode or a truncated op is fatal to the batch (binary cannot skip what it cannot
   size) -- feature-detect via capabilities, don't probe.

     0x01 CLEAR     rgb
     0x02 PIXEL     x y rgb
     0x03 HLINE     x y w rgb            0x04 VLINE x y h rgb
     0x05 LINE      x y x1 y1 t rgb
     0x06 RECT      x y w h flags t rgb          (flags bit0 = fill)
     0x07 CIRCLE    x y r flags t rgb
     0x08 ELLIPSE   x y rx ry flags t rgb
     0x09 TRIANGLE  x y x1 y1 x2 y2 flags rgb
     0x0A ROUNDRECT x y w h r flags rgb
     0x0B GRADIENT  x y w h from(3) to(3) dir    (dir 0 = vertical, 1 = horizontal)
     0x0C ARC       x y r t start end flags rgb  (flags bit0 = pie fill)
     0x0D POLY      n flags t rgb then n * (x y) (flags bit0 = fill, bit1 = closed
                                                  outline; neither = open polyline)
     0x0E CLIP      x y w h                      (w <= 0 clears)
     0x0F ORIGIN    x y
     0x10 TEXT      x y size flags rgb [outline rgb] [shadow rgb] len bytes
                    (flags: bits0-1 align 0 L / 1 C / 2 R, bit2 aa, bit3 has-outline,
                     bit4 has-shadow; text UTF-8, len u8)
     0x11 SPRITE    i x y flags                  (flags: bit0 flipH, bit1 flipV,
                                                  bits2-3 rot/90, bits4-5 scale-1)
     0x12 SCROLL    dx dy rgb
     0x13 SHOW                                                                      */
static inline int16_t bops16(const uint8_t* p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }

static int canvasOpsRunBin(const uint8_t* p, size_t len, bool* shownOut, bool* okOut) {
  int applied = 0; bool shown = false, ok = true;
  gOpsOx = 0; gOpsOy = 0; gOpsClipOn = false; gOpsBlend = 0;
  panelClearClip(); panelClearBlend();
  uint8_t binAlpha = 255;                         // v3.8: batch alpha (0x15), applied per op
  size_t i = 0;
  #define BOPS_NEED(n) if (i + (n) > len) { ok = false; break; }
  while (i < len) {
    panelSetBlend((uint8_t)gOpsBlend, binAlpha);  // per op: batch mode + batch alpha
    const uint8_t opb = p[i++];
    switch (opb) {
      case 0x01: { BOPS_NEED(3);
        panelFillRect(0, 0, gPanel.panelW, gPanel.panelH, p[i], p[i+1], p[i+2]); i += 3; break; }
      case 0x02: { BOPS_NEED(7);
        panelPixel(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy, p[i+4], p[i+5], p[i+6]);
        i += 7; break; }
      case 0x03: { BOPS_NEED(9);
        panelHLine(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy, bops16(p+i+4),
                   p[i+6], p[i+7], p[i+8]); i += 9; break; }
      case 0x04: { BOPS_NEED(9);
        panelVLine(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy, bops16(p+i+4),
                   p[i+6], p[i+7], p[i+8]); i += 9; break; }
      case 0x05: { BOPS_NEED(12);
        canvasThickLine(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy,
                        bops16(p+i+4) + gOpsOx, bops16(p+i+6) + gOpsOy,
                        p[i+8], p[i+9], p[i+10], p[i+11]); i += 12; break; }
      case 0x06: { BOPS_NEED(13);
        const int x = bops16(p+i) + gOpsOx, y = bops16(p+i+2) + gOpsOy;
        const int w = bops16(p+i+4), h = bops16(p+i+6);
        const uint8_t fl = p[i+8], t = p[i+9] ? p[i+9] : 1;
        if (fl & 1) panelFillRect(x, y, w, h, p[i+10], p[i+11], p[i+12]);
        else {
          panelFillRect(x, y, w, t, p[i+10], p[i+11], p[i+12]);
          panelFillRect(x, y + h - t, w, t, p[i+10], p[i+11], p[i+12]);
          panelFillRect(x, y, t, h, p[i+10], p[i+11], p[i+12]);
          panelFillRect(x + w - t, y, t, h, p[i+10], p[i+11], p[i+12]);
        }
        i += 13; break; }
      case 0x07: { BOPS_NEED(11);
        const int x = bops16(p+i) + gOpsOx, y = bops16(p+i+2) + gOpsOy, r = bops16(p+i+4);
        const uint8_t fl = p[i+6], t = p[i+7] ? p[i+7] : 1;
        if (!(fl & 1) && t > 1) canvasArc(x, y, r, t, 0, 360, false, p[i+8], p[i+9], p[i+10]);
        else panelCircle(x, y, r, fl & 1, p[i+8], p[i+9], p[i+10]);
        i += 11; break; }
      case 0x08: { BOPS_NEED(13);
        const int x = bops16(p+i) + gOpsOx, y = bops16(p+i+2) + gOpsOy;
        const int rx = bops16(p+i+4), ry = bops16(p+i+6);
        const uint8_t fl = p[i+8], t = p[i+9] ? p[i+9] : 1;
        for (int k = 0; k < ((fl & 1) ? 1 : t); k++)
          panelEllipse(x, y, rx - k, ry - k, fl & 1, p[i+10], p[i+11], p[i+12]);
        i += 13; break; }
      case 0x09: { BOPS_NEED(16);
        panelTriangle(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy,
                      bops16(p+i+4) + gOpsOx, bops16(p+i+6) + gOpsOy,
                      bops16(p+i+8) + gOpsOx, bops16(p+i+10) + gOpsOy,
                      p[i+12] & 1, p[i+13], p[i+14], p[i+15]); i += 16; break; }
      case 0x0A: { BOPS_NEED(14);
        panelRoundRect(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy,
                       bops16(p+i+4), bops16(p+i+6), bops16(p+i+8),
                       p[i+10] & 1, p[i+11], p[i+12], p[i+13]); i += 14; break; }
      case 0x0B: { BOPS_NEED(15);
        // dir byte: 0 vertical, 1 horizontal, 2 radial (v3.6; dithered like JSON)
        canvasOpGradientEx(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy,
                           bops16(p+i+4), bops16(p+i+6),
                           p[i+8], p[i+9], p[i+10], p[i+11], p[i+12], p[i+13],
                           p[i+14] <= 2 ? p[i+14] : 0, 0, true); i += 15; break; }
      case 0x0C: { BOPS_NEED(15);
        canvasArc(bops16(p+i) + gOpsOx, bops16(p+i+2) + gOpsOy, bops16(p+i+4), p[i+6],
                  bops16(p+i+7), bops16(p+i+9), p[i+11] & 1,
                  p[i+12], p[i+13], p[i+14]); i += 15; break; }
      case 0x0D: { BOPS_NEED(6);
        const uint8_t n = p[i], fl = p[i+1], t = p[i+2] ? p[i+2] : 1;
        const uint8_t cr = p[i+3], cg = p[i+4], cb = p[i+5];
        size_t q = i + 6;
        BOPS_NEED(6 + (size_t)n * 4);
        int vx[16], vy[16];
        const int keep = n > 16 ? 16 : n;
        for (int k = 0; k < keep; k++) {
          vx[k] = bops16(p + q + k * 4) + gOpsOx;
          vy[k] = bops16(p + q + k * 4 + 2) + gOpsOy;
        }
        if (fl & 1) canvasPolyFillPts(vx, vy, keep, cr, cg, cb);
        else {
          for (int k = 1; k < keep; k++)
            canvasThickLine(vx[k-1], vy[k-1], vx[k], vy[k], t, cr, cg, cb);
          if ((fl & 2) && keep > 2)
            canvasThickLine(vx[keep-1], vy[keep-1], vx[0], vy[0], t, cr, cg, cb);
        }
        i += 6 + (size_t)n * 4; break; }
      case 0x0E: { BOPS_NEED(8);
        const int x = bops16(p+i) + gOpsOx, y = bops16(p+i+2) + gOpsOy;
        const int w = bops16(p+i+4), h = bops16(p+i+6);
        if (w > 0 && h > 0) {
          gOpsClip[0] = x; gOpsClip[1] = y; gOpsClip[2] = x + w; gOpsClip[3] = y + h;
          gOpsClipOn = true;
        } else gOpsClipOn = false;
        opsApplyClip(); i += 8; break; }
      case 0x0F: { BOPS_NEED(4);
        gOpsOx = bops16(p+i); gOpsOy = bops16(p+i+2); i += 4; break; }
      case 0x10: { BOPS_NEED(9);
        const int x = bops16(p+i) + gOpsOx, y = bops16(p+i+2) + gOpsOy;
        const uint8_t size = p[i+4], fl = p[i+5];
        const uint8_t cr = p[i+6], cg = p[i+7], cb = p[i+8];
        size_t q = i + 9;
        uint8_t orr = 0, org = 0, orb = 0, shr = 0, shg = 0, shb = 0;
        if (fl & 0x08) { if (q + 3 > len) { ok = false; break; } orr = p[q]; org = p[q+1]; orb = p[q+2]; q += 3; }
        if (fl & 0x10) { if (q + 3 > len) { ok = false; break; } shr = p[q]; shg = p[q+1]; shb = p[q+2]; q += 3; }
        if (q + 1 > len) { ok = false; break; }
        const uint8_t slen = p[q]; q += 1;
        if (q + slen > len) { ok = false; break; }
        char txt[128];
        const uint8_t keep = slen < sizeof(txt) - 1 ? slen : sizeof(txt) - 1;
        memcpy(txt, p + q, keep); txt[keep] = 0;
        const int align = fl & 0x03;
        if (fl & 0x04) {
          aaTextDraw(x, y, size, txt, align, cr, cg, cb);
        } else {
          const Font1252* f = canvasFace(size);
          char enc[192];
          const int tw = (int)utf8ToCp1252(txt, enc, sizeof(enc)) * f->width;
          const int tx = (align == 1) ? x - tw / 2 : (align == 2) ? x - tw : x;
          if (fl & 0x08)
            for (int dy2 = -1; dy2 <= 1; dy2++)
              for (int dx2 = -1; dx2 <= 1; dx2++)
                { if (dx2 || dy2) canvasText(tx + dx2, y + dy2, txt, orr, org, orb, f); }
          else if (fl & 0x10) canvasText(tx + 1, y + 1, txt, shr, shg, shb, f);
          canvasText(tx, y, txt, cr, cg, cb, f);
        }
        i = q + slen; break; }
      case 0x11: { BOPS_NEED(7);
        const uint16_t ti = (uint16_t)((p[i] << 8) | p[i+1]);
        const int x = bops16(p+i+2) + gOpsOx, y = bops16(p+i+4) + gOpsOy;
        const uint8_t fl = p[i+6];
        canvasAtlasBlitEx(canvasAtlasBoundHandle(), ti, x, y,
                          fl & 1, fl & 2, (uint16_t)(((fl >> 2) & 3) * 90),
                          (uint8_t)(((fl >> 4) & 3) + 1));
        i += 7; break; }
      case 0x12: { BOPS_NEED(7);
        panelScroll(bops16(p+i), bops16(p+i+2), p[i+4], p[i+5], p[i+6]); i += 7; break; }
      case 0x13: panelShow(); shown = true; continue;
      case 0x14: BOPS_NEED(1); gOpsBlend = (p[i] <= 4) ? p[i] : 0; i += 1; break;   // blend mode (v3.8)
      case 0x15: BOPS_NEED(1); binAlpha = p[i]; i += 1; break;                       // batch alpha (v3.8)
      default: ok = false; break;
    }
    if (!ok) break;
    applied++;                                    // counts like the JSON path (state ops too)
  }
  #undef BOPS_NEED
  gOpsOx = 0; gOpsOy = 0; gOpsClipOn = false; gOpsBlend = 0;
  panelClearClip(); panelClearBlend();
  if (shownOut) *shownOut = shown;
  if (okOut) *okOut = ok;
  return applied;
}

static int canvasOpsRun(JsonArrayConst ops, bool* shownOut) {
  int applied = 0; bool shown = false;
  gOpsOx = 0; gOpsOy = 0; gOpsClipOn = false; gOpsBlend = 0;
  panelClearClip(); panelClearBlend();
  for (JsonVariantConst op : ops) {
    panelSetBlend((uint8_t)gOpsBlend, 255);   // per op: batch mode, opaque unless a colour sets alpha
    const char* k = op["op"] | "";
    int x = (op["x"] | 0) + gOpsOx, y = (op["y"] | 0) + gOpsOy;
    int w = op["w"] | 0, h = op["h"] | 0;
    if (!strcmp(k, "clear")) {
      uint8_t r = 0, g = 0, b = 0; canvasColor(op["color"], r, g, b);
      panelFillRect(0, 0, gPanel.panelW, gPanel.panelH, r, g, b);
    } else if (!strcmp(k, "pixel")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelPixel(x, y, r, g, b);
    } else if (!strcmp(k, "hline")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelHLine(x, y, w, r, g, b);
    } else if (!strcmp(k, "vline")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelVLine(x, y, h, r, g, b);
    } else if (!strcmp(k, "rect")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      if (op["fill"] | false) panelFillRect(x, y, w, h, r, g, b);
      else {
        const int t = op["t"] | 1;                       // outline thickness (v3.5)
        panelFillRect(x, y, w, t, r, g, b);              panelFillRect(x, y + h - t, w, t, r, g, b);
        panelFillRect(x, y, t, h, r, g, b);              panelFillRect(x + w - t, y, t, h, r, g, b);
      }
    } else if (!strcmp(k, "line")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const int lx1 = (op["x1"] | 0) + gOpsOx, ly1 = (op["y1"] | 0) + gOpsOy;
      const int lt = op["t"] | 1;
      int lcap, ljoin; float ldon, ldoff;
      const bool ldash = canvasStrokeStyle(op, lcap, ljoin, ldon, ldoff);   // v3.8.1
      if (op["aa"] | false) canvasAALine(x, y, lx1, ly1, r, g, b);          // v3.8: anti-aliased
      else if (ldash) canvasDashLine(x, y, lx1, ly1, lt, lcap, ldon, ldoff, 0, r, g, b);
      else canvasThickLineCap(x, y, lx1, ly1, lt, lcap, r, g, b);
    } else if (!strcmp(k, "circle")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const int t = op["t"] | 1;
      if ((op["aa"] | false) && !(op["fill"] | false)) canvasAACircle(x, y, op["r"] | 0, r, g, b);
      else if (t > 1 && !(op["fill"] | false)) canvasArc(x, y, op["r"] | 0, t, 0, 360, false, r, g, b);
      else panelCircle(x, y, op["r"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "image")) {
      canvasOpImage(op, x, y, w, h);
    } else if (!strcmp(k, "atlas")) {
      // {"op":"atlas","name":"weather"} (v3.1): bind a named sheet for subsequent sprite
      // ops. Sticky (it survives this batch); an unknown name binds nothing -- later
      // sprites no-op rather than failing the batch. Lazy-loads a persisted sheet.
      canvasAtlasBind(op["name"] | "");
    } else if (!strcmp(k, "sprite")) {
      // {"op":"sprite","i":N,"x":X,"y":Y}: blit tile N of the BOUND sheet at (x,y),
      // transparent pixels skipped. Nothing bound or i out of range: skip, don't count.
      const int ti = op["i"] | -1;
      const char* fl = op["flip"] | "";                  // "h" | "v" | "hv" (v3.5)
      if (ti < 0 || !canvasAtlasBlitEx(canvasAtlasBoundHandle(), (uint16_t)ti, x, y,
                                       strchr(fl, 'h') != nullptr, strchr(fl, 'v') != nullptr,
                                       (uint16_t)(op["rot"] | 0), (uint8_t)(op["scale"] | 1)))
        continue;
    } else if (!strcmp(k, "scroll")) {
      uint8_t r = 0, g = 0, b = 0; canvasColor(op["color"], r, g, b);   // vacated pixels: black default
      panelScroll(op["dx"] | 0, op["dy"] | 0, r, g, b);
    } else if (!strcmp(k, "triangle")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelTriangle(x, y, (op["x1"] | 0) + gOpsOx, (op["y1"] | 0) + gOpsOy,
                    (op["x2"] | 0) + gOpsOx, (op["y2"] | 0) + gOpsOy, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "roundrect")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelRoundRect(x, y, w, h, op["r"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "ellipse")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const int t = op["t"] | 1;
      for (int i = 0; i < (op["fill"] | false ? 1 : t); i++)
        panelEllipse(x, y, (op["rx"] | 0) - i, (op["ry"] | 0) - i, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "gradient")) {
      // v3.6: dir "v" | "h" | "r" (radial) | "a" (angled, with "angle" degrees);
      // ordered dithering default ON ("dither":false for the old hard bands).
      uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
      canvasColor(op["from"], r0, g0, b0); canvasColor(op["to"], r1, g1, b1);
      const char* dir = op["dir"] | (op["angle"].is<int>() ? "a" : "v");
      const int mode = !strcmp(dir, "h") ? 1 : !strcmp(dir, "r") ? 2 : !strcmp(dir, "a") ? 3 : 0;
      canvasOpGradientEx(x, y, w, h, r0, g0, b0, r1, g1, b1,
                         mode, op["angle"] | 0, op["dither"] | true);
    } else if (!strcmp(k, "polyline")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const int t = op["t"] | 1;
      const bool aa = op["aa"] | false;
      int pcap, pjoin; float pdon, pdoff;
      const bool pdash = canvasStrokeStyle(op, pcap, pjoin, pdon, pdoff);   // v3.8.1
      if (t <= 1 && !aa && !pdash && !gOpsOx && !gOpsOy) canvasOpPolyline(op["points"], r, g, b);
      else {
        int px = 0, py = 0; bool first = true; float ph = 0;
        for (JsonVariantConst p : op["points"].as<JsonArrayConst>()) {
          const int nx = (int)p[0] + gOpsOx, ny = (int)p[1] + gOpsOy;
          if (!first) {
            if (aa) canvasAALine(px, py, nx, ny, r, g, b);
            else if (pdash) ph = canvasDashLine(px, py, nx, ny, t, pcap, pdon, pdoff, ph, r, g, b);
            else canvasThickLineCap(px, py, nx, ny, t, pcap, r, g, b);
          }
          if (pjoin == 1 && t > 1) panelCircle(nx, ny, t/2, true, r, g, b);   // round joins/caps
          px = nx; py = ny; first = false;
        }
      }
    } else if (!strcmp(k, "text")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const char* s = op["s"] | "";
      const Font1252* f = canvasFace(op["size"] | 10);
      // v2.1: an optional uploaded face -- "custom" is the PSRAM slot, any other name loads
      // /fonts/<name>.fnt into it. Unknown/missing names keep the built-in face, never error.
      if (op["font"].is<const char*>()) {
        const Font1252* cf = canvasFontByName(op["font"].as<const char*>());
        if (cf) f = cf;
      }
      const char* al = op["align"] | "left";
      if (op["aa"] | false) {                            // v3.5: anti-aliased Orbitron
        aaTextDraw(x, y, op["size"] | 24, s, alignIdx(al, "center"), r, g, b);
      } else {
        char twenc[256];   // glyph count, not byte count: multi-byte UTF-8 skewed centre/right
        int tx = x, tw = (int)utf8ToCp1252(s, twenc, sizeof(twenc)) * f->width;
        if      (!strcmp(al, "center")) tx = x - tw / 2;
        else if (!strcmp(al, "right"))  tx = x - tw;
        // v3.5 styles: a 1 px outline (8 neighbours) or a +1,+1 drop shadow, drawn first.
        uint8_t sr, sg, sb;
        if (canvasColorGet(op["outline"], sr, sg, sb)) {
          for (int dy2 = -1; dy2 <= 1; dy2++)
            for (int dx2 = -1; dx2 <= 1; dx2++)
              if (dx2 || dy2) canvasText(tx + dx2, y + dy2, s, sr, sg, sb, f);
        } else if (canvasColorGet(op["shadow"], sr, sg, sb)) {
          canvasText(tx + 1, y + 1, s, sr, sg, sb, f);
        }
        canvasText(tx, y, s, r, g, b, f);
      }
    } else if (!strcmp(k, "arc")) {
      // {"op":"arc",x,y,"r":R,"t":T,"start":deg,"end":deg,"fill":bool} -- 0 deg = 12
      // o'clock, clockwise (the gauge convention). fill=true draws the pie slice.
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      canvasArc(x, y, op["r"] | 0, op["t"] | 2, op["start"] | 0, op["end"] | 360,
                op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "poly")) {
      // Closed polygon: fill (even-odd scanline) or outline with optional thickness.
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      if (op["fill"] | true) canvasPolyFill(op["points"], r, g, b);
      else {
        const int t = op["t"] | 1;
        const bool aa = op["aa"] | false;
        int qcap, qjoin; float qdon, qdoff;
        const bool qdash = canvasStrokeStyle(op, qcap, qjoin, qdon, qdoff);   // v3.8.1
        int px = 0, py = 0, fx = 0, fy = 0; bool first = true; float ph = 0;
        for (JsonVariantConst p : op["points"].as<JsonArrayConst>()) {
          const int nx = (int)p[0] + gOpsOx, ny = (int)p[1] + gOpsOy;
          if (first) { fx = nx; fy = ny; }
          else if (aa) canvasAALine(px, py, nx, ny, r, g, b);
          else if (qdash) ph = canvasDashLine(px, py, nx, ny, t, qcap, qdon, qdoff, ph, r, g, b);
          else canvasThickLineCap(px, py, nx, ny, t, qcap, r, g, b);
          if (qjoin == 1 && t > 1 && !first) panelCircle(px, py, t/2, true, r, g, b);
          px = nx; py = ny; first = false;
        }
        if (!first) {
          if (aa) canvasAALine(px, py, fx, fy, r, g, b);
          else if (qdash) canvasDashLine(px, py, fx, fy, t, qcap, qdon, qdoff, ph, r, g, b);
          else canvasThickLineCap(px, py, fx, fy, t, qcap, r, g, b);
          if (qjoin == 1 && t > 1) { panelCircle(px, py, t/2, true, r, g, b);
                                     panelCircle(fx, fy, t/2, true, r, g, b); }
        }
      }
    } else if (!strcmp(k, "bezier")) {
      // {"op":"bezier","points":[[x,y]x3 or x4],"t":..,"aa":..,"color":..} (v3.8):
      // quadratic (3 points) or cubic (4). aa smooths it.
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      int bx[4], by[4], nb = 0;
      for (JsonVariantConst p : op["points"].as<JsonArrayConst>()) {
        if (nb >= 4) break;
        bx[nb] = (int)p[0] + gOpsOx; by[nb] = (int)p[1] + gOpsOy; nb++;
      }
      if (nb >= 3) canvasBezier(bx, by, nb, op["t"] | 1, op["aa"] | false, r, g, b);
    } else if (!strcmp(k, "clip")) {
      // {"op":"clip",x,y,w,h} clips all later ops to the window; bare {"op":"clip"}
      // clears it. Batch-scoped: canvasOpsRun resets it on entry and exit.
      if (w > 0 && h > 0) {
        gOpsClip[0] = x; gOpsClip[1] = y; gOpsClip[2] = x + w; gOpsClip[3] = y + h;
        gOpsClipOn = true;
      } else gOpsClipOn = false;
      opsApplyClip();
    } else if (!strcmp(k, "origin")) {
      // {"op":"origin",x,y}: absolute translation added to every later coordinate
      // (including points arrays); bare {"op":"origin"} resets. Batch-scoped.
      gOpsOx = op["x"] | 0; gOpsOy = op["y"] | 0;
    } else if (!strcmp(k, "textbox")) {
      // {"op":"textbox",x,y,w,h,"s":...,"size":N,"font":...,"align":...,"valign":...,
      //  "color":[r,g,b]} -- word-wrapped (explicit \n honoured), aligned, clipped.
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      const Font1252* f = canvasFace(op["size"] | 10);
      if (op["font"].is<const char*>()) {
        const Font1252* cf = canvasFontByName(op["font"].as<const char*>());
        if (cf) f = cf;
      }
      canvasTextBox(x, y, w, h, op["s"] | "", f,
                    alignIdx(op["align"] | "left", "center"),
                    alignIdx(op["valign"] | "top", "middle"), r, g, b);
    } else if (!strcmp(k, "blend")) {
      // {"op":"blend","mode":"over|add|multiply|screen|max"} -- batch-scoped compositing
      // mode for subsequent ops (v3.8). Combine with per-colour alpha ([r,g,b,a]).
      const char* bm = op["mode"] | "over";
      gOpsBlend = !strcmp(bm, "add") ? 1 : !strcmp(bm, "multiply") ? 2 :
                  !strcmp(bm, "screen") ? 3 : !strcmp(bm, "max") ? 4 : 0;
    } else if (!strcmp(k, "show")) {
      panelShow(); shown = true; continue;
    } else continue;                      // unknown op: skip, do not count
    applied++;
  }
  gOpsOx = 0; gOpsOy = 0; gOpsClipOn = false; gOpsBlend = 0;
  panelClearClip(); panelClearBlend();
  if (shownOut) *shownOut = shown;
  return applied;
}

// POST /api/canvas/ops
static esp_err_t handleApiCanvasOps(httpd_req_t* r) {
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  if (quietBlocked(r)) return ESP_OK;
  if (csBusy(r)) return ESP_OK;
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  if (!doc.is<JsonArray>()) { httpxErr(r, 400, "Body must be a JSON array of ops"); return ESP_OK; }
  canvasEnter(false);
  bool shown = false;
  const int applied = canvasOpsRun(doc.as<JsonArrayConst>(), &shown);
  // Answer BEFORE the auto-show: panelShow parks ~one frame as its tear-guard, and
  // serializing that inside the request cost every ops frame ~14 ms of round-trip.
  // Sent after the reply, the wait overlaps the client preparing its next frame.
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"applied\":%d}", applied);
  esp_err_t rc = httpxSend(r, 200, "application/json", buf);
  if (!shown) panelShow();
  return rc;
}

// POST /api/canvas/opsb (v3.5) -- the binary twin of /api/canvas/ops: the fixed-layout
// encoding documented at canvasOpsRunBin, as a raw body. Same semantics as the JSON
// path (auto-present unless the batch contained SHOW); a malformed batch is a 400 with
// nothing presented. Mainly for testing and one-shot clients -- game-rate clients
// should carry the same bytes in stream record 0x06 instead.
static esp_err_t handleApiCanvasOpsBin(httpd_req_t* r) {
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
  if (quietBlocked(r)) return ESP_OK;
  if (csBusy(r)) return ESP_OK;
  const size_t len = (size_t)r->content_len;
  if (len < 1 || len > 65536) return httpxErr(r, 400, "Body must be 1..65536 bytes of binary ops");
  uint8_t* body = (uint8_t*)ps_malloc(len);
  if (!body) return httpxErr(r, 503, "Out of memory");
  size_t got = 0;
  while (got < len) {
    int n = httpxRecv(r, (char*)body + got, len - got);
    if (n <= 0) { free(body); return httpxErr(r, 400, "Truncated body"); }
    got += (size_t)n;
  }
  canvasEnter(false);
  bool shown = false, okb = true;
  const int applied = canvasOpsRunBin(body, len, &shown, &okb);
  free(body);
  if (!okb) return httpxErr(r, 400, "Malformed binary op batch");
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"applied\":%d}", applied);
  esp_err_t rc = httpxSend(r, 200, "application/json", buf);
  if (!shown) panelShow();
  return rc;
}

// PUT /api/canvas/frame -- a full raw frame, width*height pixels, row-major, top-left origin.
// The pixel format follows from the body LENGTH: W*H*3 is rgb888 (3 bytes/px), W*H*2 is rgb565
// (2 bytes/px, big-endian) -- the length is authoritative, the client's ?fmt= only a hint.
// Streamed straight to the back buffer so no multi-KB frame is ever buffered whole; presented
// after the last byte. Bodies stream through httpxBuf, the shared raw-body buffer (httpx.h).
static esp_err_t handleApiCanvasFrame(httpd_req_t* r) {
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
  if (quietBlocked(r)) return ESP_OK;
  if (cs.req) return httpxErr(r, 409, "canvas stream active -- close it first");
  const size_t px  = (size_t)gPanel.panelW * gPanel.panelH;
  const size_t len = (size_t)r->content_len;
  uint8_t bpp;
  if      (len == px * 3) bpp = 3;
  else if (len == px * 2) bpp = 2;
  else return httpxErr(r, 400, "Body length must equal width*height*3 (rgb888) or *2 (rgb565)");
  canvasStandDown();                 // stand the wall down and wait for the renderer to park
  // Transition configured: stage the whole frame in PSRAM and tween at the end, instead of
  // painting pixels as they stream. Allocation failure falls back to the direct path -- a
  // hard cut beats a 500.
  const bool staged = (gTransType != 0) && canvasStageBegin(bpp);

  // Row-buffered (v3.1): bytes accumulate into one row and blit whole rows -- the row
  // blitter is ~4-6x the old per-pixel decode, and a row is the natural carry unit
  // across chunk boundaries (no per-pixel state).
  static uint8_t rowBuf[PANEL_MAX_W * 3];
  const size_t rowBytes = (size_t)gPanel.panelW * bpp;
  size_t rowFill = 0;
  uint16_t y = 0;
  size_t   recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) return httpxErr(r, 400, "Truncated body");   // nothing presented; wall stays parked until the next taker
    recvd += (size_t)n;
    if (staged) { canvasStageFeed(httpxBuf, (size_t)n); continue; }
    size_t i = 0;
    while (i < (size_t)n) {
      const size_t take = min(rowBytes - rowFill, (size_t)n - i);
      memcpy(rowBuf + rowFill, httpxBuf + i, take);
      rowFill += take; i += take;
      if (rowFill == rowBytes) {
        if (bpp == 3) panelBlitRow888(0, y, gPanel.panelW, rowBuf);
        else          panelBlitRow565(0, y, gPanel.panelW, rowBuf);
        rowFill = 0; y++;
      }
    }
  }
  if (staged) canvasStagePresent();  // tween old -> new, land on new
  else        panelShow();
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"width\":%u,\"height\":%u,\"pixels\":%lu}",
           (unsigned)gPanel.panelW, (unsigned)gPanel.panelH, (unsigned long)px);
  return httpxSend(r, 200, "application/json", buf);
}

// GET /api/canvas/frame[?fmt=rgb888|rgb565] -- read the live panel back as raw pixels: a screenshot
// of whatever is on screen (wall, effect, canvas, animation, ticker), quantised to the panel depth.
// Reconstructed from the framebuffer into a reused PSRAM buffer, then streamed. Read-only.
static uint8_t* rbBuf = nullptr; static size_t rbCap = 0;
static esp_err_t handleApiCanvasFrameGet(httpd_req_t* r) {
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  // Streaming a ~48 KB screenshot out holds internal TX buffers; on a RAM-tight 256x64 board, done
  // while the companion is also pushing, that can approach loop()'s reboot floor. Same circuit
  // breaker as the large uploads: refuse a preview rather than risk a reboot. A poller just retries.
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP) { httpxErr(r, 507, "Low on memory -- try again in a moment"); return ESP_OK; }
  const bool rgb565 = (httpxArg(r, "fmt") == "rgb565");
  const size_t need = (size_t)gPanel.panelW * gPanel.panelH * (rgb565 ? 2 : 3);
  if (rbCap < need) {
    if (rbBuf) free(rbBuf);
    rbBuf = (uint8_t*)ps_malloc(need);
    if (!rbBuf) rbBuf = (uint8_t*)malloc(need);
    rbCap = rbBuf ? need : 0;
  }
  if (!rbBuf) { httpxErr(r, 503, "Out of memory"); return ESP_OK; }
  panelReadback(rbBuf, rgb565);                    // snapshot fast, then stream at leisure
  // Two SEPARATE buffers, static: httpd_resp_set_hdr stores the POINTER until the
  // response is sent, so a shared stack buffer made both headers read the LAST value
  // written (the width header said "64" -- the sheared-preview bug).
  static char wv[16], hv[16];
  snprintf(wv, sizeof(wv), "%u", (unsigned)gPanel.panelW);  httpd_resp_set_hdr(r, "X-Canvas-Width", wv);
  snprintf(hv, sizeof(hv), "%u", (unsigned)gPanel.panelH);  httpd_resp_set_hdr(r, "X-Canvas-Height", hv);
  httpd_resp_set_hdr(r, "X-Canvas-Format", rgb565 ? "rgb565" : "rgb888");
  httpd_resp_set_type(r, "application/octet-stream");
  for (size_t off = 0; off < need; off += 4096) {   // bigger chunks: fewer writes, faster drain
    size_t c = (need - off < 4096) ? (need - off) : 4096;
    httpxChunk(r, (const char*)(rbBuf + off), c);
    wdgWebMs = millis();                              // feed the web watchdog on a ~48 KB send
  }
  return httpxChunkEnd(r);
}

// PUT /api/canvas/rect -- update one rectangle without resending the whole panel. Body: an 8-byte
// big-endian header [x, y, w, h] (u16 each) then w*h pixels, rgb888 or rgb565 (by remaining length).
// Drawn ON TOP of what is on screen: the back buffer is synced to the live frame first.
static esp_err_t handleApiCanvasRect(httpd_req_t* r) {
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
  if (quietBlocked(r)) return ESP_OK;
  if (cs.req) return httpxErr(r, 409, "canvas stream active -- close it first");
  static const char* BAD = "Body must be an 8-byte x,y,w,h header then w*h*3 or w*h*2 pixels";
  const size_t len = (size_t)r->content_len;
  if (len < 8) return httpxErr(r, 400, BAD);

  uint8_t  hdr[8]; uint8_t hdrN = 0;
  int      x = 0, y = 0, w = 0, h = 0;
  uint8_t  bpp = 3, carry[3], carryN = 0;
  int      col = 0, row = 0;
  bool     started = false;
  size_t   recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) return httpxErr(r, 400, BAD);       // truncated: nothing presented
    recvd += (size_t)n;
    int i = 0;
    while (hdrN < 8 && i < n) hdr[hdrN++] = httpxBuf[i++];
    if (hdrN < 8) continue;                          // header can straddle chunks
    if (!started) {
      x = (hdr[0] << 8) | hdr[1]; y = (hdr[2] << 8) | hdr[3];
      w = (hdr[4] << 8) | hdr[5]; h = (hdr[6] << 8) | hdr[7];
      long cells = (long)w * h;
      long body  = (long)len - 8;
      if (w <= 0 || h <= 0 || (body != cells * 3 && body != cells * 2)) return httpxErr(r, 400, BAD);
      bpp = (body == cells * 3) ? 3 : 2;
      canvasStandDown();                             // take the panel, then start from what is shown
      panelCloneToBack();
      started = true;
    }
    for (; i < n; i++) {
      carry[carryN++] = httpxBuf[i];
      if (carryN < bpp) continue;
      carryN = 0;
      uint8_t cr, cg, cb;
      pxDecode(carry, bpp, cr, cg, cb);
      if (row < h) panelPixel(x + col, y + row, cr, cg, cb);   // panelPixel clamps
      if (++col >= w) { col = 0; row++; }
    }
  }
  if (started && gPanel.ready) panelShow();
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", x, y, w, h);
  return httpxSend(r, 200, "application/json", buf);
}

// Parse and blit a rects body -- u16 count, u8 fmt, u8 0, then per rect u16 x,y,w,h +
// w*h*bpp pixels -- onto a back buffer synced from the live frame. Shared by
// PUT /api/canvas/rects and the stream channel's rects record (v3.2). No present.
// False on a malformed body (*done tells how far it got).
static bool canvasRectsApply(const uint8_t* body, size_t len, int* outDone) {
  if (len < 4) return false;
  const uint16_t count = ((uint16_t)body[0] << 8) | body[1];
  const uint8_t  fmt   = body[2];
  if (!count || count > 256 || (fmt != 2 && fmt != 3)) return false;
  const uint8_t bpp = fmt;
  canvasEnter(false);                    // take the panel (no-op once in canvas mode)...
  panelCloneToBack();                    // ...then start from what is shown
  size_t off = 4;
  int done = 0;
  for (uint16_t i = 0; i < count; i++) {
    if (off + 8 > len) break;
    const int x = (body[off] << 8) | body[off+1];
    const int y = (body[off+2] << 8) | body[off+3];
    const int w = (body[off+4] << 8) | body[off+5];
    const int h = (body[off+6] << 8) | body[off+7];
    off += 8;
    const size_t px = (size_t)w * h * bpp;
    if (!w || !h || off + px > len) break;
    const uint8_t* p = body + off;
    for (int row = 0; row < h; row++, p += (size_t)w * bpp) {
      if (bpp == 3) panelBlitRow888(x, y + row, w, p);
      else          panelBlitRow565(x, y + row, w, p);
    }
    off += px;
    done++;
    if ((i & 15) == 0) wdgWebMs = millis();
  }
  if (outDone) *outDone = done;
  return done == count;
}

// PUT /api/canvas/rects -- multi-rect DELTA update (v3.1): one body carrying N changed
// regions, drawn over what is on screen, presented once. The network fix for full-frame
// re-sends: a client that diffs its frames sends 10-50x less. Body (big-endian):
//   u16 count, u8 fmt (2=rgb565 BE, 3=rgb888), u8 reserved(0),
//   then per rect: u16 x, u16 y, u16 w, u16 h, then w*h*bpp pixels.
// Buffered whole (deltas are small by design; cap = 2 full frames), parsed linearly,
// rows blitted. No transitions -- deltas are incremental updates, not presents.
static esp_err_t handleApiCanvasRects(httpd_req_t* r) {
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
  if (quietBlocked(r)) return ESP_OK;
  if (cs.req) return httpxErr(r, 409, "canvas stream active -- close it first");
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP)
    return httpxErr(r, 507, "Low on memory -- try again in a moment");
  static const char* BAD = "Body must be u16 count, u8 fmt, u8 0, then per rect u16 x,y,w,h + w*h*bpp pixels";
  const size_t len = (size_t)r->content_len;
  const size_t cap = (size_t)gPanel.panelW * gPanel.panelH * 3 * 2 + 4 + 256 * 8;
  if (len < 4)   return httpxErr(r, 400, BAD);
  if (len > cap) return httpxErr(r, 413, "Body exceeds two full frames -- send a frame instead");

  uint8_t* body = (uint8_t*)ps_malloc(len);
  if (!body) return httpxErr(r, 503, "Out of memory");
  size_t got = 0;
  while (got < len) {
    int n = httpxRecv(r, (char*)body + got, len - got);
    if (n <= 0) { free(body); return httpxErr(r, 400, "Truncated body"); }
    got += (size_t)n;
  }

  int done = 0;
  const bool okAll = canvasRectsApply(body, len, &done);
  free(body);
  if (!okAll) { dispReturnToWall(); return httpxErr(r, 400, BAD); }
  // Answer before the present, like ops: the tear-guard wait overlaps the client's next diff.
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"rects\":%d}", done);
  esp_err_t rc = httpxSend(r, 200, "application/json", buf);
  panelShow();
  return rc;
}

// ---- QOI image decode (https://qoiformat.org) --------------------------------------------------
// Decode a full-panel QOI image to the back buffer -- or, with toStage (v3.0.1), into the
// transition stage buffer (the decoder emits pixels row-major, exactly what canvasStageFeed
// expects), so a QOI upload can tween like a raw frame PUT: the companion always sends QOI,
// which is why "transitions do nothing with the companion" was true through v3.0.0.
// False if the magic or the dimensions do not match this panel. Alpha is ignored (the panel
// has none). One pass, 64-entry running index.
static bool qoiDecodeToPanel(const uint8_t* d, size_t len, bool toStage = false) {
  static uint8_t qrow[PANEL_MAX_W * 3];   // one decoded rgb888 row (v3.1: row blits)
  if (len < 14 || d[0] != 'q' || d[1] != 'o' || d[2] != 'i' || d[3] != 'f') return false;
  uint32_t w = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
  uint32_t h = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
  if ((int)w != gPanel.panelW || (int)h != gPanel.panelH) return false;
  size_t p = 14;
  uint8_t ir[64] = {0}, ig[64] = {0}, ib[64] = {0}, ia[64] = {0};
  uint8_t r = 0, g = 0, b = 0, a = 255;
  const long total = (long)w * h;
  int cx = 0, cy = 0, run = 0;
  for (long px = 0; px < total; px++) {
    if (run > 0) run--;
    else if (p < len) {
      uint8_t op = d[p++];
      if      (op == 0xFE) { if (p + 3 > len) return false; r = d[p++]; g = d[p++]; b = d[p++]; }
      else if (op == 0xFF) { if (p + 4 > len) return false; r = d[p++]; g = d[p++]; b = d[p++]; a = d[p++]; }
      else if ((op & 0xC0) == 0x00) { int k = op & 0x3F; r = ir[k]; g = ig[k]; b = ib[k]; a = ia[k]; }
      else if ((op & 0xC0) == 0x40) { r += (uint8_t)(((op >> 4) & 3) - 2); g += (uint8_t)(((op >> 2) & 3) - 2); b += (uint8_t)((op & 3) - 2); }
      else if ((op & 0xC0) == 0x80) { if (p >= len) return false; uint8_t v = d[p++]; int dg = (op & 0x3F) - 32;
                                      r += (uint8_t)(dg + ((v >> 4) & 0xF) - 8); g += (uint8_t)dg; b += (uint8_t)(dg + (v & 0xF) - 8); }
      else                          { run = op & 0x3F; }   // QOI_OP_RUN: this pixel + `run` more
      int k = (r * 3 + g * 5 + b * 7 + a * 11) & 63;
      ir[k] = r; ig[k] = g; ib[k] = b; ia[k] = a;
    } else return false;
    if (toStage) { uint8_t px[3] = {r, g, b}; canvasStageFeed(px, 3); }
    else { qrow[cx * 3] = r; qrow[cx * 3 + 1] = g; qrow[cx * 3 + 2] = b; }
    if (++cx >= (int)w) {
      if (!toStage) panelBlitRow888(0, cy, (int)w, qrow);
      cx = 0; cy++;
    }
  }
  return true;
}
// PUT /api/canvas/qoi -- a full-panel QOI image. Buffered whole in PSRAM (it is compressed) then
// decoded straight to the panel. Same takeover as /frame. The buffer allocation is kept and
// reused across uploads (qoiCap), like the readback buffer.
static uint8_t* qoiBuf = nullptr; static size_t qoiCap = 0;

// Receive a whole raw body into a kept PSRAM buffer (allocating/growing it as needed) -- the
// shape shared by the qoi/gif/font uploads, which all buffer-then-decode. Returns the bytes
// received, or 0 on truncation/allocation failure (buf/cap are updated either way).
static size_t recvWhole(httpd_req_t* r, uint8_t** buf, size_t* cap, size_t need, bool psramOnly = false) {
  if (*cap < need) {
    if (*buf) free(*buf);
    *buf = (uint8_t*)ps_malloc(need);
    if (!*buf && !psramOnly) *buf = (uint8_t*)malloc(need);
    *cap = *buf ? need : 0;
  }
  if (!*buf) return 0;
  size_t got = 0;
  while (got < need) {
    int n = httpxRecv(r, (char*)(*buf + got), need - got);
    if (n <= 0) return 0;
    got += (size_t)n;
  }
  return got;
}

static esp_err_t handleApiCanvasQoi(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (quietBlocked(r)) return ESP_OK;
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running or out of memory");
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP)
    return httpxErr(r, 507, "Low on memory -- try again in a moment");   // stressed: back off
  const size_t need = (size_t)r->content_len;
  const size_t cap  = (size_t)gPanel.panelW * gPanel.panelH * 4 + 1024;   // QOI worst case + header
  if (need < 14 || need > cap) return httpxErr(r, 400, "Not a QOI image matching the panel size");
  canvasStandDown();
  const size_t got = recvWhole(r, &qoiBuf, &qoiCap, need);
  if (!got) { dispReturnToWall(); return httpxErr(r, 503, "Panel not running or out of memory"); }
  // Transition configured: decode into the stage buffer and tween old -> new, exactly
  // like the raw frame PUT. Stage-allocation failure falls back to the hard cut.
  const bool staged = (gTransType != 0) && canvasStageBegin(3);
  if (!qoiDecodeToPanel(qoiBuf, got, staged)) {
    dispReturnToWall();                               // bad image: don't leave the panel parked
    return httpxErr(r, 400, "Not a QOI image matching the panel size");
  }
  if (staged) canvasStagePresent();
  else        panelShow();
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"width\":%u,\"height\":%u}", (unsigned)gPanel.panelW, (unsigned)gPanel.panelH);
  return httpxSend(r, 200, "application/json", buf);
}

// PUT /api/canvas/anim -- upload a looping animation that plays on-device (client can disconnect).
// Header (14 B, big-endian): "MPGA"(4) ver(1)=1 fmt(1: 2=rgb565,3=rgb888) fps(1) flags(1: bit0=loop)
// w(2) h(2) frames(2), then frames*w*h*fmt bytes of frame data. Streamed straight into PSRAM.
static esp_err_t animRawReply(httpd_req_t* r, int e) {
  if (e == 400) return httpxErr(r, 400, "Bad MPGA header or truncated upload");
  if (e == 413) return httpxErr(r, 413, "Animation exceeds the PSRAM budget");
  if (e == 507) return httpxErr(r, 507, "Low on memory -- try again in a moment");
  return httpxErr(r, 503, "Panel not running or out of memory");
}
static esp_err_t handleApiCanvasAnim(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (quietBlocked(r)) return ESP_OK;
  if (!gPanel.ready) return animRawReply(r, 503);
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP) return animRawReply(r, 507);   // stressed: back off
  const size_t len = (size_t)r->content_len;
  if (len < 14) return animRawReply(r, 400);
  canvasStandDown();                              // park the render task while the store refills

  uint8_t hdr[14]; uint8_t hdrN = 0;
  bool    begun = false;
  size_t  recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) { dispReturnToWall(); return animRawReply(r, 400); }   // failed mid-upload: hand the panel back
    recvd += (size_t)n;
    int i = 0;
    while (hdrN < 14 && i < n) hdr[hdrN++] = httpxBuf[i++];
    if (hdrN < 14) continue;
    if (!begun) {
      if (memcmp(hdr, "MPGA", 4) != 0 || hdr[4] != 1) { dispReturnToWall(); return animRawReply(r, 400); }
      uint16_t w  = (hdr[8]  << 8) | hdr[9];
      uint16_t h  = (hdr[10] << 8) | hdr[11];
      uint16_t fr = (hdr[12] << 8) | hdr[13];
      int rc = canvasAnimBegin(hdr[5], hdr[6], hdr[7] & 1, w, h, fr);
      if (rc) { dispReturnToWall(); return animRawReply(r, rc); }
      begun = true;
    }
    if (i < n) canvasAnimFeed(httpxBuf + i, (size_t)(n - i));
  }
  int rc = begun ? canvasAnimCommit() : 400;
  if (rc) { dispReturnToWall(); return animRawReply(r, rc); }
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"frames\":%u}", (unsigned)canvasAnimCount());
  return httpxSend(r, 200, "application/json", buf);
}

// ---- named atlas library (v3.1) ----------------------------------------------------------------
// PUT /api/canvas/atlas/<name> -- upload one named MPTA sheet (12 B header unchanged:
// "MPTA"(4) ver(1)=1 fmt(1: 2=rgb565BE, 3=rgb888) tileW(2) tileH(2) tiles(2), then tile data).
// Built in a NEW allocation and published at Commit, so a bound sheet never blits half-written.
static esp_err_t atlasRawReply(httpd_req_t* r, int e) {
  if (e == 400) return httpxErr(r, 400, "Bad name (1-32 chars a-z 0-9 . _ -), bad MPTA header or truncated upload");
  if (e == 404) return httpxErr(r, 404, "No such atlas");
  if (e == 413) return httpxErr(r, 413, "Sheet exceeds the 2 MB per-sheet cap");
  if (e == 507) return httpxErr(r, 507, "Low on memory -- try again in a moment");
  return httpxErr(r, 503, "Out of memory");
}
static esp_err_t handleApiAtlasPut(httpd_req_t* r) {
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP) return atlasRawReply(r, 507);   // stressed: back off
  const String name = httpxPathTail(r, "/api/canvas/atlas/");
  if (!canvasAtlasNameOk(name.c_str())) return atlasRawReply(r, 400);
  const size_t len = (size_t)r->content_len;
  if (len < 12) return atlasRawReply(r, 400);       // empty body: never reaches the header

  uint8_t hdr[12]; uint8_t hdrN = 0;
  bool    begun = false;
  size_t  recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) return atlasRawReply(r, 400);
    recvd += (size_t)n;
    int i = 0;
    while (hdrN < 12 && i < n) hdr[hdrN++] = httpxBuf[i++];
    if (hdrN < 12) continue;
    if (!begun) {
      if (memcmp(hdr, "MPTA", 4) != 0 || hdr[4] != 1) return atlasRawReply(r, 400);
      uint16_t tw = (hdr[6]  << 8) | hdr[7];
      uint16_t th = (hdr[8]  << 8) | hdr[9];
      uint16_t tn = (hdr[10] << 8) | hdr[11];
      int rc = canvasAtlasBegin(name.c_str(), hdr[5], tw, th, tn);
      if (rc) return atlasRawReply(r, rc);
      begun = true;
    }
    if (i < n) canvasAtlasFeed(httpxBuf + i, (size_t)(n - i));
  }
  int rc = begun ? canvasAtlasCommit() : 400;
  if (rc) return atlasRawReply(r, rc);
  { char cd[64]; snprintf(cd, sizeof(cd), "atlas '%.32s' uploaded", name.c_str()); logCommand('R', cd); }
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"name\":\"%s\",\"bytes\":%u}",
           name.c_str(), (unsigned)(len - 12));
  return httpxSend(r, 200, "application/json", buf);
}

// GET /api/canvas/atlas -- the library, streamed (resident sheets + persisted files).
static esp_err_t handleApiAtlasList(httpd_req_t* r) {
  httpd_resp_set_type(r, "application/json");
  gStreamReq = r;
  canvasAtlasListJson(animListSink);
  return httpxChunkEnd(r);
}

// GET /api/canvas/atlas/<name> -- the sheet back as its MPTA image, from PSRAM when
// resident, else from the persisted file. Mirrors the PUT; the Files tab's preview
// renders it in the browser.
static esp_err_t handleApiAtlasGet(httpd_req_t* r) {
  const String name = httpxPathTail(r, "/api/canvas/atlas/");
  if (!canvasAtlasNameOk(name.c_str())) return atlasRawReply(r, 404);
  httpd_resp_set_type(r, "application/octet-stream");
  uint8_t hdr[12]; size_t bytes = 0;
  const uint8_t* buf = canvasAtlasData(name.c_str(), hdr, &bytes);
  if (buf) {
    httpxChunk(r, (const char*)hdr, 12);
    for (size_t off = 0; off < bytes; off += 4096) {
      size_t c = (bytes - off < 4096) ? (bytes - off) : 4096;
      httpxChunk(r, (const char*)(buf + off), c);
      wdgWebMs = millis();
    }
    return httpxChunkEnd(r);
  }
  if (sfFsReady) {                                    // persisted but not resident
    char path[64];
    snprintf(path, sizeof(path), "/atlas/%s.mpta", name.c_str());
    File f = FFat.open(path, "r");
    if (f && !f.isDirectory()) {
      uint8_t fb[1024];
      size_t got;
      while ((got = f.read(fb, sizeof(fb))) > 0) { httpxChunk(r, (const char*)fb, got); wdgWebMs = millis(); }
      f.close();
      return httpxChunkEnd(r);
    }
    if (f) f.close();
  }
  return atlasRawReply(r, 404);
}

// POST /api/canvas/atlas/<name>/save + DELETE /api/canvas/atlas/<name>
static esp_err_t handleApiAtlasPost(httpd_req_t* r) {
  String tail = httpxPathTail(r, "/api/canvas/atlas/");
  if (!tail.endsWith("/save")) return httpxErr(r, 404, "not found");
  const String name = tail.substring(0, tail.length() - 5);
  int rc = canvasAtlasSave(name.c_str());
  if (rc == 404) return atlasRawReply(r, 404);
  if (rc == 507) return httpxErr(r, 507, "Filesystem full or write failed");
  if (rc)        return httpxErr(r, 503, "No filesystem");
  { char cd[64]; snprintf(cd, sizeof(cd), "atlas '%.32s' saved", name.c_str()); logCommand('R', cd); }
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}
static esp_err_t handleApiAtlasDelete(httpd_req_t* r) {
  const String name = httpxPathTail(r, "/api/canvas/atlas/");
  if (canvasAtlasDelete(name.c_str()) != 0) return atlasRawReply(r, 404);
  { char cd[64]; snprintf(cd, sizeof(cd), "atlas '%.32s' deleted", name.c_str()); logCommand('R', cd); }
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}

// PUT /api/canvas/gif -- import a GIF into the animation store (v2.1). The whole file is
// buffered in PSRAM (it is compressed, like the QOI path), then decoded on RAW_END into the
// same store PUT /api/canvas/anim fills, so it plays immediately and POST /api/canvas/anim/save
// persists it. GIFs smaller than the panel are centred on black; larger ones are rejected.
#define CANVAS_GIF_MAX_BYTES  (4096u * 1024u)
static esp_err_t handleApiCanvasGif(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (quietBlocked(r)) return ESP_OK;
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP)
    return httpxErr(r, 507, "Low on memory -- try again in a moment");   // stressed: back off
  const size_t need = (size_t)r->content_len;
  if (need < 14)                   return httpxErr(r, 400, "Empty or truncated body");
  if (need > CANVAS_GIF_MAX_BYTES) return httpxErr(r, 413, "GIF exceeds the 4 MB budget");
  // Buffered whole, then decoded -- and freed right after: the compressed upload is dead
  // weight once the frames live in the animation store. A multi-MB file only ever fits PSRAM.
  uint8_t* buf = nullptr; size_t cap = 0;
  const size_t got = recvWhole(r, &buf, &cap, need, /*psramOnly=*/true);
  if (!got) {
    if (buf) free(buf);
    // No dispReturnToWall(): the panel is only taken over below (the decode), which a
    // truncated upload never reaches -- whatever was showing keeps showing.
    return httpxErr(r, buf ? 400 : 503, buf ? "Empty or truncated body" : "Out of memory");
  }
  canvasStandDown();                    // park the render task while the store refills
  uint16_t frames = 0; uint8_t fps = 0; const char* errMsg = "";
  int rc = canvasGifImport(buf, got, &frames, &fps, &errMsg);
  free(buf);
  if (rc) { dispReturnToWall(); return httpxErr(r, rc, errMsg[0] ? errMsg : "GIF import failed"); }
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"frames\":%u,\"fps\":%u}",
           (unsigned)frames, (unsigned)fps);
  return httpxSend(r, 200, "application/json", out);
}

// PUT /api/canvas/font -- upload an MPFT font blob (tools/fontpack.py) into the custom-font
// slot (v2.1). Small (<= 64 KB), so it is buffered whole and handed to canvasFontInstall on
// RAW_END -- the same path a library load uses. On success the face answers to the name
// "custom" in the ticker and the ops text op.
static esp_err_t handleApiCanvasFont(httpd_req_t* r) {
  const size_t need = (size_t)r->content_len;
  if (need < 8)                     return httpxErr(r, 400, "Bad MPFT header or truncated upload");
  if (need > CANVAS_FONT_MAX_BYTES) return httpxErr(r, 413, "Font exceeds the 64 KB cap");
  uint8_t* buf = nullptr; size_t cap = 0;   // small (<= 64 KB): buffered whole, freed after install
  const size_t got = recvWhole(r, &buf, &cap, need);
  if (!got) {
    if (buf) free(buf);
    return httpxErr(r, buf ? 400 : 503, buf ? "Bad MPFT header or truncated upload" : "Out of memory");
  }
  int rc = canvasFontInstall(buf, got);
  free(buf);
  if (rc == 400) return httpxErr(r, 400, "Bad MPFT header or truncated upload");
  if (rc == 413) return httpxErr(r, 413, "Font exceeds the 64 KB cap");
  if (rc)        return httpxErr(r, 503, "Out of memory");
  uint8_t w = 0, h = 0, a = 0;
  canvasFontInfo(w, h, a);
  char out[80];
  snprintf(out, sizeof(out), "{\"ok\":true,\"font\":\"custom\",\"w\":%u,\"h\":%u,\"ascent\":%u}",
           (unsigned)w, (unsigned)h, (unsigned)a);
  return httpxSend(r, 200, "application/json", out);
}

// Map a canvasFont* return code onto the error surface (the font twin of animRcReply).
static esp_err_t fontRcReply(httpd_req_t* r, int rc, const char* okBody) {
  switch (rc) {
    case 0:   return httpxSend(r, 200, "application/json", okBody);
    case 400: return httpxErr(r, 400, "Bad name (1-24 chars a-z 0-9 - _) or bad/truncated file");
    case 404: return httpxErr(r, 404, "No such font");
    case 409: return httpxErr(r, 409, "No custom font loaded -- upload one first");
    case 413: return httpxErr(r, 413, "Font exceeds the 64 KB cap");
    case 507: return httpxErr(r, 507, "Filesystem full or write failed");
    default:  return httpxErr(r, 503, "Filesystem or memory unavailable");
  }
}

// POST /api/canvas/font/save {"name":"x"} -- persist the custom-font slot to FATFS.
static esp_err_t handleApiFontSave(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* name = doc["name"] | "";
  { char cd[64]; snprintf(cd, sizeof(cd), "font save '%.24s'", name); logCommand('R', cd); }
  return fontRcReply(r, canvasFontSave(name), "{\"ok\":true}");
}

// POST /api/canvas/font/delete {"name":"x"}
static esp_err_t handleApiFontDelete(httpd_req_t* r) {
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* name = doc["name"] | "";
  { char cd[64]; snprintf(cd, sizeof(cd), "font delete '%.24s'", name); logCommand('R', cd); }
  return fontRcReply(r, canvasFontDelete(name), "{\"ok\":true}");
}

// GET /api/canvas/fonts -- the font library, streamed (like /api/canvas/anims).
static esp_err_t handleApiFontList(httpd_req_t* r) {
  httpd_resp_set_type(r, "application/json");
  gStreamReq = r;
  canvasFontList(animListSink);
  return httpxChunkEnd(r);
}

// POST /api/canvas/ticker -- one line of text scrolling right-to-left, rendered on-device.
// {text, color:[r,g,b], speed:1..20, overlay:bool, band:bool}. overlay=true (v2.1)
// composites the ticker as a lower-third over whatever else is presenting -- wall,
// effect, animation -- and survives page changes. Empty text stops any ticker.
static esp_err_t handleApiCanvasTicker(httpd_req_t* r) {
  if (csBusy(r)) return ESP_OK;
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* text = doc["text"] | "";
  if (!text[0]) {
    canvasTickerStopForce();           // explicit stop kills an overlay ticker too
    dispReturnToWall();
    return httpxSend(r, 200, "application/json", "{\"ok\":true,\"active\":false}");
  }
  if (gQuietTime) { httpxErr(r, 409, "Quiet Time is active"); return ESP_OK; }
  uint8_t cr = 255, cg = 255, cb = 255;
  if (doc["color"].is<JsonArray>() && doc["color"].size() >= 3) {
    cr = (uint8_t)(int)doc["color"][0]; cg = (uint8_t)(int)doc["color"][1]; cb = (uint8_t)(int)doc["color"][2];
  }
  int speed = doc["speed"] | 2;
  bool overlay = doc["overlay"] | false;
  bool band    = doc["band"]    | true;
  // v2.1: optional "font" -- "custom" is the uploaded PSRAM face, any other name loads
  // /fonts/<name>.fnt into the slot. Absent or unresolvable falls back to the built-in
  // panel-sized face (NULL below); a missing font must scroll text, not 500.
  const Font1252* face = nullptr;
  if (doc["font"].is<const char*>()) face = canvasFontByName(doc["font"].as<const char*>());
  canvasTickerSet(text, cr, cg, cb, speed, overlay, band, face);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"active\":true,\"overlay\":%s}",
           overlay ? "true" : "false");
  return httpxSend(r, 200, "application/json", resp);
}

/* ----------------------------------------------------------
   FATFS file browser  (v2.2)
   ----------------------------------------------------------
   The dashboard's Files tab: list, download, delete and upload files on the FATFS
   partition. Everything on the filesystem is the user's -- animations (/anim), fonts
   (/fonts), the companion blob (/compset.gz) -- so nothing is off limits to delete;
   the UI's job is to warn, not this API's to refuse. Paths are a deliberately narrow
   grammar (absolute, [a-z0-9._/-], no "..", <= 48 chars -- everything this firmware
   ever writes fits it), so a request can be validated by inspection.
---------------------------------------------------------- */
#define FS_UPLOAD_MIN_FREE  (64u * 1024u)   // free space an upload must leave behind

// The path grammar above, applied everywhere a client names a file.
static bool fsPathOk(const char* p) {
  if (!p || p[0] != '/' || !p[1]) return false;   // absolute, and never the bare root
  const size_t n = strlen(p);
  if (n > 48) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-' || c == '/')) return false;
  }
  if (strstr(p, "..")) return false;
  return true;
}

// Recursive walk for the list below. Depth-bounded: this filesystem is the root plus
// /anim and /fonts, so four levels is already generous -- and the bound also caps the
// open directory handles the recursion holds at once.
static bool fsListFirst = true;
static void fsListDir(const char* path, int depth) {
  if (depth > 3) return;
  File dir = FFat.open(path);
  if (!dir) return;
  if (!dir.isDirectory()) { dir.close(); return; }
  File f;
  while ((f = dir.openNextFile())) {
    // f.name() is the basename on FFat (see canvasAnimList); rebuild the full path.
    char full[96];
    snprintf(full, sizeof(full), "%s/%s", strcmp(path, "/") ? path : "", f.name());
    if (f.isDirectory()) {
      f.close();
      fsListDir(full, depth + 1);
    } else {
      char row[160];
      snprintf(row, sizeof(row), "%s{\"path\":\"%s\",\"size\":%u}",
               fsListFirst ? "" : ",", full, (unsigned)f.size());
      f.close();
      fsListFirst = false;
      httpxChunkStr(gStreamReq, row);
    }
    wdgWebMs = millis();
  }
  dir.close();
}

// GET /api/fs -- totals plus every file on the FATFS, streamed (one chunk per file,
// like /api/flap/modules -- the list is never held in RAM whole).
static esp_err_t handleApiFsList(httpd_req_t* r) {
  if (!sfFsReady) { httpxErr(r, 503, "No filesystem"); return ESP_OK; }
  wdgWebMs = millis();
  const size_t total = FFat.totalBytes();
  const size_t used  = FFat.usedBytes();
  httpd_resp_set_type(r, "application/json");
  char head[80];
  snprintf(head, sizeof(head), "{\"total\":%u,\"free\":%u,\"files\":[",
           (unsigned)total, (unsigned)(total > used ? total - used : 0));
  gStreamReq = r;
  httpxChunkStr(r, head);
  fsListFirst = true;
  fsListDir("/", 0);
  httpxChunkStr(r, "]}");
  return httpxChunkEnd(r);   // terminate the chunked response
}

// GET /api/fs/file?path=/anim/x.mpg -- stream one file back as a download.
static esp_err_t handleApiFsFile(httpd_req_t* r) {
  if (!sfFsReady) { httpxErr(r, 503, "No filesystem"); return ESP_OK; }
  const String pathArg = httpxArg(r, "path");
  if (!fsPathOk(pathArg.c_str())) {
    httpxErr(r, 400, "Bad path (absolute, a-z 0-9 . _ - /, max 48 chars)"); return ESP_OK;
  }
  File f = FFat.open(pathArg.c_str(), "r");
  if (!f || f.isDirectory()) { if (f) f.close(); httpxErr(r, 404, "No such file"); return ESP_OK; }
  // The browser lands the download under the file's own name, not "file".
  const char* base = strrchr(pathArg.c_str(), '/');
  base = base ? base + 1 : pathArg.c_str();
  char cd[80];
  snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", base);
  httpd_resp_set_hdr(r, "Content-Disposition", cd);
  wdgWebMs = millis();
  httpd_resp_set_type(r, "application/octet-stream");
  uint8_t buf[1024];
  while (size_t got = f.read(buf, sizeof(buf))) {
    httpxChunk(r, (const char*)buf, got);
    wdgWebMs = millis();
  }
  f.close();
  return httpxChunkEnd(r);
}

// POST /api/fs/delete {"path":"/anim/x.mpg"} -- delete a file (or an empty directory).
// Deliberately unrestricted: it is the user's flash, and the dashboard carries the
// warnings (the companion blob's confirm, for one).
static esp_err_t handleApiFsDelete(httpd_req_t* r) {
  if (!sfFsReady) { httpxErr(r, 503, "No filesystem"); return ESP_OK; }
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  const char* path = doc["path"] | "";
  if (!fsPathOk(path)) {
    httpxErr(r, 400, "Bad path (absolute, a-z 0-9 . _ - /, max 48 chars)"); return ESP_OK;
  }
  if (!FFat.exists(path)) { httpxErr(r, 404, "No such file"); return ESP_OK; }
  bool isDir = false;
  { File f = FFat.open(path, "r"); isDir = f && f.isDirectory(); if (f) f.close(); }
  if (!(isDir ? FFat.rmdir(path) : FFat.remove(path))) {
    httpxErr(r, 507, isDir ? "Directory not empty or remove failed" : "Remove failed");
    return ESP_OK;
  }
  { char cd[64]; snprintf(cd, sizeof(cd), "fs delete %.48s", path); logCommand('R', cd); }
  return httpxSend(r, 200, "application/json", "{\"ok\":true}");
}

// POST /api/fs/upload?name=<filename> -- a raw-body file upload onto FATFS (v3.0
// breaking change: the body is the file itself, no multipart, and the filename rides
// the `name` query param -- the Files tab and curl --data-binary both send exactly
// this). The name is sanitized and routed by extension -- .mpg lands in /anim/, .fnt
// in /fonts/, anything else in the root -- so an uploaded animation is immediately
// playable by name. Streamed to a .tmp then renamed (the canvasAnimSave pattern), so
// a failed upload never clobbers a good file.

// Client filename -> target path. Lowercase and keep [a-z0-9._-]; a name that
// sanitizes to nothing, or to more than 40 chars, is a reject (a truncated name
// would silently save under a name the user never chose).
static bool fsUploadTarget(const char* fn, char* out, size_t outLen) {
  char name[41];
  size_t n = 0;
  for (const char* p = fn; *p; p++) {
    const char c = (char)tolower((unsigned char)*p);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-')) continue;   // dropped, not fatal
    if (n >= sizeof(name) - 1) return false;             // sanitized name > 40 chars
    name[n++] = c;
  }
  name[n] = 0;
  if (!n) return false;
  const char* dir = "/";
  if      (n > 4 && strcmp(name + n - 4, ".mpg") == 0) dir = "/anim/";
  else if (n > 4 && strcmp(name + n - 4, ".fnt") == 0) dir = "/fonts/";
  else if (n > 5 && strcmp(name + n - 5, ".mpta") == 0) dir = "/atlas/";
  snprintf(out, outLen, "%s%s", dir, name);
  return fsPathOk(out);   // "." and friends still cannot sneak through
}

static esp_err_t handleFsUpload(httpd_req_t* r) {
  if (!sfFsReady) return httpxErr(r, 503, "No filesystem");
  char path[64], tmp[72];                       // <final> and <final>.tmp while in flight
  const String name = httpxArg(r, "name");
  const size_t len  = (size_t)r->content_len;
  if (!name.length() || len == 0 || !fsUploadTarget(name.c_str(), path, sizeof(path)))
    return httpxErr(r, 400, "Bad filename (a-z 0-9 . _ -, 1-40 chars after sanitizing)");
  // Free-space gate, decided before anything touches flash: an upload may never leave
  // less than FS_UPLOAD_MIN_FREE behind. Content-Length IS the file (no multipart
  // framing since v3.0), so the gate is exact.
  const size_t total = FFat.totalBytes(), used = FFat.usedBytes();
  const size_t freeB = total > used ? total - used : 0;
  if (freeB < len || freeB - len < FS_UPLOAD_MIN_FREE)
    return httpxErr(r, 413, "Not enough free space (64 KB must remain)");
  if (!strncmp(path, "/anim/",  6)) FFat.mkdir("/anim");    // idempotent
  if (!strncmp(path, "/fonts/", 7)) FFat.mkdir("/fonts");
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FFat.remove(tmp);                                         // clear a stale temp
  File f = FFat.open(tmp, "w");
  if (!f) return httpxErr(r, 507, "Write failed");

  size_t recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) { f.close(); FFat.remove(tmp); return httpxErr(r, 400, "Truncated upload"); }
    if (f.write(httpxBuf, (size_t)n) != (size_t)n) { f.close(); FFat.remove(tmp); return httpxErr(r, 507, "Write failed"); }
    recvd += (size_t)n;
  }
  f.close();
  // Publish atomically: an existing file survives intact until the rename lands.
  FFat.remove(path);
  if (!FFat.rename(tmp, path)) { FFat.remove(tmp); return httpxErr(r, 507, "Write failed"); }
  { char cd[80]; snprintf(cd, sizeof(cd), "fs upload %s (%u B)", path, (unsigned)recvd); logCommand('R', cd); }
  char buf[112];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u}", path, (unsigned)recvd);
  return httpxSend(r, 200, "application/json", buf);
}

void webInit() {
  sseInit();                                // slot mutex + frame buffer for /api/events
  httpxOn("/",                       HTTP_GET,  handleRoot);
  httpxOn("/api/events",             HTTP_GET,  sseHandleRequest);   // SSE live-preview stream (v3.0)
  httpxOn("/favicon.svg",            HTTP_GET,  handleFavicon);
  httpxOn("/logo.svg",               HTTP_GET,  handleLogo);
  // v1.1: one route per UI language, "/lang/<code>", all onto one handler (which reads
  // the code back out of the URI). An unknown code falls through to the normal 404.
  // English is never registered -- it is the text already in the page. The URI strings
  // must outlive registration (httpxOn keeps the pointer), hence the static table.
  static char langUri[UI_LANG_COUNT][16];
  for (size_t i = 0; i < UI_LANG_COUNT; i++) {
    snprintf(langUri[i], sizeof(langUri[i]), "/lang/%s", UI_LANGS[i].code);
    httpxOn(langUri[i], HTTP_GET, handleLang);
  }
  httpxOn("/ota",                    HTTP_GET,  handleOTAPage);
  httpxOn("/api/ota/upload",         HTTP_POST, handleOTAUpload);
  httpxOn("/api/log",                HTTP_GET,  handleApiMessages);
  httpxOn("/api/frames/send",        HTTP_POST, handleApiSend);
  httpxOn("/api/frames/batch",       HTTP_POST, handleApiSendBatch);
  httpxOn("/api/flap/modules",       HTTP_GET,  handleApiModules);
  httpxOn("/api/display/state",      HTTP_GET,  handleApiDisplayState);
  httpxOn("/api/display/cells",      HTTP_POST, handleApiDisplayCells);
  httpxOn("/api/display/brightness", HTTP_GET,  handleApiDisplayBrightness);
  httpxOn("/api/display/brightness", HTTP_POST, handleApiDisplayBrightness);
  // v2.2: the FATFS file browser behind the dashboard's Files tab.
  httpxOn("/api/fs",                 HTTP_GET,  handleApiFsList);
  httpxOn("/api/fs/file",            HTTP_GET,  handleApiFsFile);
  httpxOn("/api/fs/delete",          HTTP_POST, handleApiFsDelete);
  httpxOn("/api/fs/upload",          HTTP_POST, handleFsUpload);
  httpxOn("/api/flap/char",          HTTP_POST, handleApiChar);
  httpxOn("/api/flap/index",         HTTP_POST, handleApiIndex);
  httpxOn("/api/flap/text",          HTTP_POST, handleApiText);
  httpxOn("/api/flap/home",          HTTP_POST, handleApiHome);
  httpxOn("/api/capabilities",       HTTP_GET,  handleApiCapabilities);
  httpxOn("/api/status",             HTTP_GET,  handleApiStatus);
  httpxOn("/api/quiet",              HTTP_GET,  handleApiQuiet);
  httpxOn("/api/quiet",              HTTP_POST, handleApiQuiet);
  httpxOn("/api/quiet/schedule",     HTTP_GET,  handleApiQuietSchedule);
  httpxOn("/api/quiet/schedule",     HTTP_POST, handleApiQuietSchedule);
  httpxOn("/api/companion",          HTTP_GET,  handleApiCompanion);
  httpxOn("/api/companion",          HTTP_POST, handleApiCompanion);
  // v3.1 blob store: the PUT body is binary gzip, streamed to FATFS.
  httpxOn("/api/companion/settings", HTTP_GET,  handleApiCompanionSettingsGet);
  httpxOn("/api/companion/settings", HTTP_PUT,  handleApiCompanionSettingsPut);
  httpxOn("/api/config",             HTTP_GET,  handleApiConfigGet);
  httpxOn("/api/config/wifi",        HTTP_POST, handleApiConfigWifi);
  httpxOn("/api/config/settings",    HTTP_POST, handleApiConfigSettings);
  // Raw canvas (Matrix-only): direct pixel control of the panel.
  httpxOn("/api/canvas",             HTTP_GET,  handleApiCanvas);
  httpxOn("/api/canvas",             HTTP_POST, handleApiCanvas);
  httpxOn("/api/canvas/frame",       HTTP_GET,  handleApiCanvasFrameGet);
  httpxOn("/api/canvas/frame",       HTTP_PUT,  handleApiCanvasFrame);
  httpxOn("/api/canvas/rect",        HTTP_PUT,  handleApiCanvasRect);
  httpxOn("/api/canvas/rects",       HTTP_PUT,  handleApiCanvasRects);
  httpxOn("/api/canvas/stream",      HTTP_PUT,  handleApiCanvasStream);
  httpxOn("/api/canvas/stream",      HTTP_GET,  handleApiCanvasStreamGet);
  httpxOn("/api/canvas/audio",       HTTP_GET,  handleApiCanvasAudio);
  httpxOn("/api/environment",        HTTP_GET,  handleApiEnvironment);
  httpxOn("/api/sound",              HTTP_GET,  handleApiSound);
  httpxOn("/api/sound",              HTTP_POST, handleApiSound);
  httpxOn("/openapi.yaml",           HTTP_GET,  handleOpenapiSpec);
  httpxOn("/.well-known/api-catalog", HTTP_GET, handleApiCatalog);
  httpxOn("/api/canvas/qoi",         HTTP_PUT,  handleApiCanvasQoi);
  httpxOn("/api/canvas/anim",        HTTP_PUT,  handleApiCanvasAnim);
  httpxOn("/api/canvas/atlas",        HTTP_GET,    handleApiAtlasList);
  httpxOnPrefix("/api/canvas/atlas/", HTTP_GET,    handleApiAtlasGet);
  httpxOnPrefix("/api/canvas/atlas/", HTTP_PUT,    handleApiAtlasPut);
  httpxOnPrefix("/api/canvas/atlas/", HTTP_POST,   handleApiAtlasPost);
  httpxOnPrefix("/api/canvas/atlas/", HTTP_DELETE, handleApiAtlasDelete);
  httpxOn("/api/canvas/gif",         HTTP_PUT,  handleApiCanvasGif);
  httpxOn("/api/canvas/font",        HTTP_PUT,  handleApiCanvasFont);
  httpxOn("/api/canvas/font/save",   HTTP_POST, handleApiFontSave);
  httpxOn("/api/canvas/font/delete", HTTP_POST, handleApiFontDelete);
  httpxOn("/api/canvas/fonts",       HTTP_GET,  handleApiFontList);
  httpxOn("/api/canvas/anim/save",   HTTP_POST, handleApiAnimSave);
  httpxOn("/api/canvas/anim/play",   HTTP_POST, handleApiAnimPlay);
  httpxOn("/api/canvas/anim/delete", HTTP_POST, handleApiAnimDelete);
  httpxOn("/api/canvas/anims",       HTTP_GET,  handleApiAnimList);
  httpxOn("/api/system/reboot",      HTTP_POST, handleApiSystemReboot);
  httpxOn("/api/canvas/transition",  HTTP_POST, handleApiCanvasTransition);
  httpxOn("/api/canvas/ticker",      HTTP_POST, handleApiCanvasTicker);
  httpxOn("/api/canvas/ops",         HTTP_POST, handleApiCanvasOps);
  httpxOn("/api/canvas/opsb",        HTTP_POST, handleApiCanvasOpsBin);
  httpxOn("/api/canvas/effect",      HTTP_POST, handleApiCanvasEffect);
  httpxStart();
  printf("[Web] HTTP server %s (port 80), %d routes\n",
         httpxUp() ? "started" : "FAILED to start -- taskWeb will retry", httpxRouteCount());
}

// httpxStart() is called once at boot; if it failed (transient no-memory spell before the
// network stack settled), nothing would ever retry it. This is that retry -- a no-op on a
// healthy server. Called every 20s from taskWeb. Returns true when the server is up.
bool webEnsureListening() {
  if (httpxUp()) return true;
  printf("[Web] HTTP server is down -- re-establishing\n");
  httpxStart();
  const bool up = httpxUp();
  printf("[Web] server %s\n", up ? "restored" : "still down (will retry)");
  return up;
}
