#include "gateway.h"
#include "panel.h"   // panelSetColourOrder: a BGR panel is a runtime fact, not a build one
#include "effects.h"
#include "canvas.h"
#include "sse.h"     // GET /api/events: the live-preview push stream (v3.0)
#include "web_ui.h"
#include <mbedtls/base64.h>   // the canvas "image" op decodes a base64 sprite



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
  wdgWebMs = millis();                 // streaming response can take a while
  // Cap per-write blocking so a stalled browser cannot wedge taskWeb. This must be
  // setConnectionTimeout(): NetworkClient keeps its own `_timeout` (which is what
  // seeds SO_SNDTIMEO) and does NOT override Stream::setTimeout(), so the old
  // setTimeout(3000) here set the *read* timeout and capped nothing at all.
  // The page is baked into the firmware, so its bytes change only when the firmware
  // is rebuilt -- and every rebuild changes __TIME__. Serve it with that as an ETag
  // and honour If-None-Match: navigating away and back then costs a tiny 304 instead
  // of re-downloading the whole ~65 KB page, while a reflash still busts the cache and
  // delivers the new UI. (The old no-store forced a full re-download on every visit.)
  httpd_resp_set_hdr(r, "ETag", BUILD_ETAG);
  httpd_resp_set_hdr(r, "Cache-Control", "no-cache");   // revalidate, but cheaply (304)
  if (httpxHeader(r, "If-None-Match") == BUILD_ETAG) { return httpxSend(r, 304, "text/html", ""); }
  httpd_resp_set_type(r, "text/html");
  // Stream the static page (web_ui.h), substituting the single {FWVER} token
  // with the firmware version so the page stays tied to the FW_VERSION macro.
  const char*  page  = PAGE_HTML;
  const size_t total = sizeof(PAGE_HTML) - 1;
  const char*  mark  = strstr(page, "{FWVER}");
  if (mark) {
    streamPage(r, page, (size_t)(mark - page));
    httpxChunkStr(r, FW_VERSION);
    streamPage(r, mark + 7, total - (size_t)(mark + 7 - page));  // 7 = strlen("{FWVER}")
  } else {
    streamPage(r, page, total);
  }
  return httpxChunkEnd(r);             // terminate the chunked response
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
  { char cv[480];
    snprintf(cv, sizeof(cv),
             "\"canvas\":{\"formats\":[\"rgb888\",\"rgb565\",\"qoi\"],\"width\":%u,\"height\":%u,"
             "\"rect\":true,\"anim\":true,\"ticker\":true,\"readback\":true,"
             "\"ops\":[\"clear\",\"pixel\",\"hline\",\"vline\",\"line\",\"rect\",\"circle\",\"ellipse\","
             "\"triangle\",\"roundrect\",\"gradient\",\"polyline\",\"text\",\"image\",\"sprite\",\"scroll\",\"show\"]},"
             "\"effects\":%s,\"effectParams\":[\"hue\",\"density\"],",
             (unsigned)gPanel.panelW, (unsigned)gPanel.panelH, effectListJson());
    capPut(cv); }

  // What the wall can DO, not just show, so a client reads this instead of sniffing the
  // firmware version and guessing.
  capPut("\"features\":[\"cells\",\"colors\",\"index\",\"lowercase\",\"pictographs\","
         "\"quiet\",\"ota\",\"canvas\",\"effects\",\"ticker\",\"brightness\",\"events\"]}");
  capFlush();
  return httpxChunkEnd(r);
}

// GET /api/status
static esp_err_t handleApiStatus(httpd_req_t* r) {
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
  char out[900];
  snprintf(out, sizeof(out),
    "{\"uptime\":%lu,\"tx\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"minheap\":%u,\"modules\":%d,"
    "\"stk\":{\"frames\":%u,\"web\":%u,\"net\":%u,\"httpd\":%u,\"rtc\":%u,\"disp\":%u},"
    "\"panel\":{\"ok\":%s,\"w\":%u,\"h\":%u,\"cols\":%u,\"rows\":%u,"
    "\"cellW\":%u,\"cellH\":%u,\"depth\":%u,\"font\":\"%s\",\"vmods\":%d},"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"quiet\":%s,"
    "\"companion\":{\"url\":\"%s\",\"status\":\"%s\",\"age\":%ld}}",
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
    cfg.companionUrl, gCompanionStatus, compAge);
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
static void canvasEnter(bool clear) {
  if (!gCanvasMode) canvasStandDown();
  if (clear && gPanel.ready) { panelClear(); panelShow(); }
}
static void canvasLeave() { dispReturnToWall(); }              // hand the panel back (no-op if idle)

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
// One CP1252 string at (x,y) top-left, one flap-font glyph per byte, solid colour. Fixed-width
// faces, so the advance is just the face width; bit 15 of each row is the leftmost column.
static void canvasText(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b,
                       const Font1252* f) {
  int cx = x;
  for (const uint8_t* p = (const uint8_t*)s; *p; ++p) {
    dispDrawGlyph1252(cx, y, f, *p, 0, 255, r, g, b);
    cx += f->width;
  }
}
// A [r,g,b] triple from an op field, leaving the caller's defaults untouched when absent.
static void canvasColor(JsonVariantConst c, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (c.is<JsonArrayConst>() && c.size() >= 3) {
    r = (uint8_t)c[0].as<int>(); g = (uint8_t)c[1].as<int>(); b = (uint8_t)c[2].as<int>();
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
    JsonDocument doc;
    if (!httpxReadJson(r, doc)) return ESP_OK;
    if (doc["active"] | false) canvasEnter(true); else canvasLeave();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"active\":%s}", gCanvasMode ? "true" : "false");
    return httpxSend(r, 200, "application/json", buf);
    return ESP_OK;
  }
  // The atlas field says what the "sprite" op can blit right now: the loaded sheet's
  // shape, or null when none has been uploaded yet.
  char atlas[48];
  uint16_t atTiles = 0, atW = 0, atH = 0;
  if (canvasAtlasInfo(atTiles, atW, atH))
    snprintf(atlas, sizeof(atlas), "{\"tiles\":%u,\"w\":%u,\"h\":%u}",
             (unsigned)atTiles, (unsigned)atW, (unsigned)atH);
  else
    strlcpy(atlas, "null", sizeof(atlas));
  char buf[288];
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
  if (e == EFFECT_NONE) {
    dispReturnToWall();             // stop -> reel wall
  } else {
    gCanvasMode = false;            // an effect owns the panel via taskDisplay, which runs
    gEffectReq  = e;                // effectReset() + starts it -- no effect state touched off-core
  }
  char buf[112];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"effect\":\"%s\",\"speed\":%u,\"hue\":%d,\"density\":%d}",
           effectName(e), (unsigned)gEffectSpeed, gEffectHue, gEffectDensity);
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
static void canvasOpGradient(int x, int y, int w, int h, JsonVariantConst from, JsonVariantConst to, bool vertical) {
  if (w <= 0 || h <= 0) return;
  uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
  canvasColor(from, r0, g0, b0); canvasColor(to, r1, g1, b1);
  const int n = vertical ? h : w, den = (n > 1) ? (n - 1) : 1;
  for (int i = 0; i < n; i++) {
    uint8_t r = (uint8_t)((int)r0 + ((int)r1 - (int)r0) * i / den);
    uint8_t g = (uint8_t)((int)g0 + ((int)g1 - (int)g0) * i / den);
    uint8_t b = (uint8_t)((int)b0 + ((int)b1 - (int)b0) * i / den);
    if (vertical) panelHLine(x, y + i, w, r, g, b);
    else          panelVLine(x + i, y, h, r, g, b);
  }
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
static esp_err_t handleApiCanvasOps(httpd_req_t* r) {
  if (!gPanel.ready) { httpxErr(r, 503, "Panel not running"); return ESP_OK; }
  JsonDocument doc;
  if (!httpxReadJson(r, doc)) return ESP_OK;
  if (!doc.is<JsonArray>()) { httpxErr(r, 400, "Body must be a JSON array of ops"); return ESP_OK; }
  canvasEnter(false);
  int applied = 0; bool shown = false;
  for (JsonVariantConst op : doc.as<JsonArrayConst>()) {
    const char* k = op["op"] | "";
    int x = op["x"] | 0, y = op["y"] | 0, w = op["w"] | 0, h = op["h"] | 0;
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
      else { panelHLine(x, y, w, r, g, b); panelHLine(x, y + h - 1, w, r, g, b);
             panelVLine(x, y, h, r, g, b); panelVLine(x + w - 1, y, h, r, g, b); }
    } else if (!strcmp(k, "line")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelLine(x, y, op["x1"] | 0, op["y1"] | 0, r, g, b);
    } else if (!strcmp(k, "circle")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelCircle(x, y, op["r"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "image")) {
      canvasOpImage(op, x, y, w, h);
    } else if (!strcmp(k, "sprite")) {
      // {"op":"sprite","i":N,"x":X,"y":Y}: blit atlas tile N (PUT /api/canvas/atlas) at (x,y),
      // transparent pixels skipped. No atlas loaded or i out of range: skip, do not count.
      const int ti = op["i"] | -1;
      if (ti < 0 || !canvasAtlasBlit((uint16_t)ti, x, y)) continue;
    } else if (!strcmp(k, "scroll")) {
      uint8_t r = 0, g = 0, b = 0; canvasColor(op["color"], r, g, b);   // vacated pixels: black default
      panelScroll(op["dx"] | 0, op["dy"] | 0, r, g, b);
    } else if (!strcmp(k, "triangle")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelTriangle(x, y, op["x1"] | 0, op["y1"] | 0, op["x2"] | 0, op["y2"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "roundrect")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelRoundRect(x, y, w, h, op["r"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "ellipse")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      panelEllipse(x, y, op["rx"] | 0, op["ry"] | 0, op["fill"] | false, r, g, b);
    } else if (!strcmp(k, "gradient")) {
      canvasOpGradient(x, y, w, h, op["from"], op["to"], strcmp(op["dir"] | "v", "h") != 0);
    } else if (!strcmp(k, "polyline")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      canvasOpPolyline(op["points"], r, g, b);
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
      int tx = x, tw = (int)strlen(s) * f->width;
      if      (!strcmp(al, "center")) tx = x - tw / 2;
      else if (!strcmp(al, "right"))  tx = x - tw;
      canvasText(tx, y, s, r, g, b, f);
    } else if (!strcmp(k, "show")) {
      panelShow(); shown = true; continue;
    } else continue;                      // unknown op: skip, do not count
    applied++;
  }
  if (!shown) panelShow();
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"applied\":%d}", applied);
  return httpxSend(r, 200, "application/json", buf);
}

// PUT /api/canvas/frame -- a full raw frame, width*height pixels, row-major, top-left origin.
// The pixel format follows from the body LENGTH: W*H*3 is rgb888 (3 bytes/px), W*H*2 is rgb565
// (2 bytes/px, big-endian) -- the length is authoritative, the client's ?fmt= only a hint.
// Streamed straight to the back buffer so no multi-KB frame is ever buffered whole; presented
// after the last byte. Bodies stream through httpxBuf, the shared raw-body buffer (httpx.h).
static esp_err_t handleApiCanvasFrame(httpd_req_t* r) {
  if (!gPanel.ready) return httpxErr(r, 503, "Panel not running");
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

  uint8_t  carry[3];                 // partial pixel across a chunk boundary
  uint8_t  carryN = 0;
  uint16_t x = 0, y = 0;             // running write position: no per-pixel divide
  size_t   recvd = 0;
  while (recvd < len) {
    int n = httpxRecv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) return httpxErr(r, 400, "Truncated body");   // nothing presented; wall stays parked until the next taker
    recvd += (size_t)n;
    if (staged) { canvasStageFeed(httpxBuf, (size_t)n); continue; }
    for (int i = 0; i < n; i++) {
      carry[carryN++] = httpxBuf[i];
      if (carryN < bpp) continue;
      carryN = 0;
      uint8_t cr, cg, cb;
      pxDecode(carry, bpp, cr, cg, cb);
      panelPixel(x, y, cr, cg, cb);            // panelPixel clamps, so an over-long body is safe
      if (++x >= gPanel.panelW) { x = 0; y++; }
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
  for (size_t off = 0; off < need; off += 1024) {
    size_t c = (need - off < 1024) ? (need - off) : 1024;
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

// ---- QOI image decode (https://qoiformat.org) --------------------------------------------------
// Decode a full-panel QOI image to the back buffer -- or, with toStage (v3.0.1), into the
// transition stage buffer (the decoder emits pixels row-major, exactly what canvasStageFeed
// expects), so a QOI upload can tween like a raw frame PUT: the companion always sends QOI,
// which is why "transitions do nothing with the companion" was true through v3.0.0.
// False if the magic or the dimensions do not match this panel. Alpha is ignored (the panel
// has none). One pass, 64-entry running index.
static bool qoiDecodeToPanel(const uint8_t* d, size_t len, bool toStage = false) {
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
    else         panelPixel(cx, cy, r, g, b);
    if (++cx >= (int)w) { cx = 0; cy++; }
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

// PUT /api/canvas/atlas -- upload a sprite tile sheet for the ops path's "sprite" op (v2.1).
// Header (12 B, big-endian): "MPTA"(4) ver(1)=1 fmt(1: 2=rgb565BE, 3=rgb888) tileW(2) tileH(2)
// tiles(2), then tiles*tileW*tileH*fmt bytes of tile data. Streamed straight into PSRAM; kept
// across uses and replaced by the next upload. No panel takeover -- it is data, not pixels.
static esp_err_t atlasRawReply(httpd_req_t* r, int e) {
  if (e == 400) return httpxErr(r, 400, "Bad MPTA header or truncated upload");
  if (e == 413) return httpxErr(r, 413, "Atlas exceeds the 2 MB budget");
  if (e == 507) return httpxErr(r, 507, "Low on memory -- try again in a moment");
  return httpxErr(r, 503, "Out of memory");
}
static esp_err_t handleApiCanvasAtlas(httpd_req_t* r) {
  if (ESP.getFreeHeap() < CANVAS_MIN_UPLOAD_HEAP) return atlasRawReply(r, 507);   // stressed: back off
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
      int rc = canvasAtlasBegin(hdr[5], tw, th, tn);
      if (rc) return atlasRawReply(r, rc);
      begun = true;
    }
    if (i < n) canvasAtlasFeed(httpxBuf + i, (size_t)(n - i));
  }
  int rc = begun ? canvasAtlasCommit() : 400;
  if (rc) return atlasRawReply(r, rc);
  uint16_t tiles = 0, w = 0, h = 0;
  canvasAtlasInfo(tiles, w, h);
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"tiles\":%u,\"w\":%u,\"h\":%u}",
           (unsigned)tiles, (unsigned)w, (unsigned)h);
  return httpxSend(r, 200, "application/json", buf);
}

// PUT /api/canvas/gif -- import a GIF into the animation store (v2.1). The whole file is
// buffered in PSRAM (it is compressed, like the QOI path), then decoded on RAW_END into the
// same store PUT /api/canvas/anim fills, so it plays immediately and POST /api/canvas/anim/save
// persists it. GIFs smaller than the panel are centred on black; larger ones are rejected.
#define CANVAS_GIF_MAX_BYTES  (4096u * 1024u)
static esp_err_t handleApiCanvasGif(httpd_req_t* r) {
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
  httpxOn("/api/canvas/qoi",         HTTP_PUT,  handleApiCanvasQoi);
  httpxOn("/api/canvas/anim",        HTTP_PUT,  handleApiCanvasAnim);
  httpxOn("/api/canvas/atlas",       HTTP_PUT,  handleApiCanvasAtlas);
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
  httpxOn("/api/canvas/effect",      HTTP_POST, handleApiCanvasEffect);
  httpxStart();
  printf("[Web] HTTP server %s (port 80)\n",
         httpxUp() ? "started" : "FAILED to start -- taskWeb will retry");
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
