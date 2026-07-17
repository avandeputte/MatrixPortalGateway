#include "gateway.h"
#include "panel.h"   // panelSetColourOrder: a BGR panel is a runtime fact, not a build one
#include "effects.h"
#include "web_ui.h"



// web.cpp -- HTTP server: the dashboard page and the REST API.
// Runs entirely on taskWeb (one request at a time). handleRoot streams the
// static dashboard from web_ui.h; each handleApi* function serves one REST
// route (the HTTP method + path is noted above each, and registered in
// webInit). Handlers are static -- only webInit is exported.
// ---- file-private forward declarations ----
static void handleApiChar();
static void handleApiConfigGet();
static void handleApiConfigMqtt();
static void handleApiConfigSettings();
static void handleApiConfigWifi();
static void handleApiDisplayState();
static void handleApiHome();
static void handleApiIndex();
static void handleApiMaintenance();
static void handleApiMessages();
static void handleApiModules();
static void handleApiMqttTest();
static void handleApiQuiet();
static void handleApiQuietSchedule();
static void handleApiCompanion();
static void handleApiCompanionSettingsGet();
static void handleApiCompanionSettingsPut();
static void handleApiCompanionSettingsRaw();
static void handleApiSend();
static void handleApiSendBatch();
static void handleApiDisplayCells();
static void handleApiCapabilities();
static void handleApiStatus();
static void handleApiText();
static void handleFavicon();
static void handleLogo();
static void handleOptions();
static void handleRoot();
static void sendJsonError(int code, const char* msg);

/* ----------------------------------------------------------
   Web server
---------------------------------------------------------- */
static void sendJsonError(int code, const char* msg) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
  server.send(code, "application/json", buf);
}

// -- GET /  (main dashboard)
// Browser tab icon (favicon): a split-flap tile -- two flaps, the signature
// horizontal seam with axle pivots, and a character bisected by it. Served at
// /favicon.svg and linked from each page <head>. SVG keeps it crisp at any size
// with no binary blob; single-quoted attributes let it sit in a plain C string.
const char FAVICON_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64' role='img' aria-label='Split-Flap Gateway'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient><clipPath id='sfTile'><rect x='7' y='7' width='50' height='50' rx='10'/></clipPath></defs><rect x='7' y='8.5' width='50' height='50' rx='10' fill='#000' opacity='0.35'/><g clip-path='url(#sfTile)'><rect x='7' y='7' width='50' height='25' fill='url(#sfTop)'/><rect x='7' y='32' width='50' height='25' fill='url(#sfBot)'/><text x='32' y='46' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='40' fill='#f3eee3'>S</text><rect x='7' y='30.9' width='50' height='2.2' fill='#0c0d10'/><rect x='7' y='33.1' width='50' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='4.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='55.5' y='29.5' width='4' height='5' rx='1.6' fill='#0c0d10'/><rect x='7' y='7' width='50' height='50' rx='10' fill='none' stroke='#0a0b0d' stroke-width='1'/></svg>";
// GET /favicon.svg
static void handleFavicon() {
  static const char* ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == ETAG) { server.send(304, "image/svg+xml", ""); return; }
  server.send(200, "image/svg+xml", FAVICON_SVG);
}

// Web UI wordmark (header logo): the app name on a split-flap board -- the same
// two-flap tiles, seam and pivots as the favicon, one cell per character. Served
// at /logo.svg and used in <h1> in place of the title text.
const char LOGO_SVG[] =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 44' role='img' aria-label='Split-Flap'><defs><linearGradient id='sfTop' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#3c424c'/><stop offset='1' stop-color='#2d323b'/></linearGradient><linearGradient id='sfBot' x1='0' y1='0' x2='0' y2='1'><stop offset='0' stop-color='#272b32'/><stop offset='1' stop-color='#181b20'/></linearGradient></defs><rect x='3' y='2' width='250' height='40' rx='6' fill='#000' opacity='0.30'/><clipPath id='clip1'><rect x='3' y='0' width='250' height='40' rx='6'/></clipPath><g clip-path='url(#clip1)'><rect x='3' y='0' width='250' height='20' fill='url(#sfTop)'/><rect x='3' y='20' width='250' height='20' fill='url(#sfBot)'/><rect x='27.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='28.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='52.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='53.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='77.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='78.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='102.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='103.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='127.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='128.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='152.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='153.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='177.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='178.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='202.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='203.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><rect x='227.4' y='0' width='1.2' height='40' fill='#0c0d10'/><rect x='228.6' y='0' width='0.5' height='40' fill='#454b56' opacity='0.45'/><text x='15.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>S</text><text x='40.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><text x='65.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='90.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>I</text><text x='115.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>T</text><text x='140.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>-</text><text x='165.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>F</text><text x='190.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>L</text><text x='215.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>A</text><text x='240.5' y='27.7' text-anchor='middle' font-family='Arial, Helvetica, sans-serif' font-weight='700' font-size='22' fill='#f3eee3'>P</text><rect x='3' y='18.9' width='250' height='2.2' fill='#0c0d10'/><rect x='3' y='21.1' width='250' height='0.8' fill='#565c68' opacity='0.7'/></g><rect x='1.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='251.2' y='17.5' width='3.6' height='5' rx='1.4' fill='#0c0d10'/><rect x='3' y='0' width='250' height='40' rx='6' fill='none' stroke='#0a0b0d' stroke-width='0.8'/></svg>";
// GET /logo.svg
static void handleLogo() {
  // ETag = build time, revalidated every request (like the page and /lang). A plain 7-day
  // max-age with NO validator was a bug: when the logo changed, the browser kept serving its
  // OLD cached copy for a week -- the header text updated but the wordmark did not.
  static const char* ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == ETAG) { server.send(304, "image/svg+xml", ""); return; }
  server.send(200, "image/svg+xml", LOGO_SVG);
}

// Stream a byte range of the static page in watchdog-friendly chunks so a slow
// client can't trip the stall detector mid-send.
static void streamPage(const char* p, size_t n) {
  const size_t CHUNK = 1024;
  for (size_t off = 0; off < n; off += CHUNK) {
    size_t c = (n - off < CHUNK) ? (n - off) : CHUNK;
    server.sendContent_P(p + off, c);
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
// One route is registered per language in webInit(), so an unknown code simply 404s.
static void handleLang(size_t idx) {
  const UiLang& L = UI_LANGS[idx];
  wdgWebMs = millis();
  server.client().setConnectionTimeout(3000);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Dictionaries live in the firmware image, so they change only on reflash -- the same
  // ETag as the page busts them at exactly the right moment.
  static const char* LANG_ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", LANG_ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  if (server.header("If-None-Match") == LANG_ETAG) { server.send(304, "application/json", ""); return; }
  server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(L.len);
  server.send(200, "application/json", "");
  server.sendContent_P((PGM_P)L.gz, L.len);
  wdgWebMs = millis();
}

// GET /
static void handleRoot() {
  wdgWebMs = millis();                 // streaming response can take a while
  // Cap per-write blocking so a stalled browser cannot wedge taskWeb. This must be
  // setConnectionTimeout(): NetworkClient keeps its own `_timeout` (which is what
  // seeds SO_SNDTIMEO) and does NOT override Stream::setTimeout(), so the old
  // setTimeout(3000) here set the *read* timeout and capped nothing at all.
  server.client().setConnectionTimeout(3000);   // 3s per socket write
  // The page is baked into the firmware, so its bytes change only when the firmware
  // is rebuilt -- and every rebuild changes __TIME__. Serve it with that as an ETag
  // and honour If-None-Match: navigating away and back then costs a tiny 304 instead
  // of re-downloading the whole ~65 KB page, while a reflash still busts the cache and
  // delivers the new UI. (The old no-store forced a full re-download on every visit.)
  static const char* PAGE_ETAG = "\"" __DATE__ "-" __TIME__ "\"";
  server.sendHeader("ETag", PAGE_ETAG);
  server.sendHeader("Cache-Control", "no-cache");   // revalidate, but cheaply (304)
  if (server.header("If-None-Match") == PAGE_ETAG) { server.send(304, "text/html", ""); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  // Stream the static page (web_ui.h), substituting the single {FWVER} token
  // with the firmware version so the page stays tied to the FW_VERSION macro.
  const char*  page  = PAGE_HTML;
  const size_t total = sizeof(PAGE_HTML) - 1;
  const char*  mark  = strstr(page, "{FWVER}");
  if (mark) {
    streamPage(page, (size_t)(mark - page));
    server.sendContent(FW_VERSION);
    streamPage(mark + 7, total - (size_t)(mark + 7 - page));  // 7 = strlen("{FWVER}")
  } else {
    streamPage(page, total);
  }
  server.sendContent("");             // terminate the chunked response
}

// GET /api/log -- the command log (REST + MQTT), newest entries since last poll
static void logSink(const char* frag) { server.sendContent(frag); wdgWebMs = millis(); }
static void handleApiMessages() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Chunked: streamed one entry at a time, never one contiguous allocation.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  logDrainTo(logSink);
  server.sendContent("");   // terminate the chunked response
}

// POST /api/rs485/send
static void handleApiSend() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* d = doc["data"] | "";
  bool raw = doc["raw"] | false;   // optional: send verbatim, bypassing sanitization
  uint8_t outBuf[TX_MAX_BYTES];
  size_t  outLen = min(strlen(d), (size_t)TX_MAX_BYTES);
  memcpy(outBuf, d, outLen);
  if (!outLen) { sendJsonError(400, "Empty data"); return; }
  { char cd[LOG_TEXT_MAX]; snprintf(cd, sizeof(cd), "send %s", d); logCommand('R', cd); }
  rs485Send(outBuf, outLen, raw);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%zu,\"raw\":%s}", outLen, raw ? "true" : "false");
  server.send(200, "application/json", resp);
}

// POST /api/rs485/batch  (v3.0) -- send many frames in one request.
// Body: {"frames":["m00-A\n","m01-B\n",...], "step_ms":15}. Each frame is sent
// normalized (like /api/rs485/send); an optional step_ms paces the cascade
// device-side. Lets the companion draw a whole animated page in ONE HTTP call
// instead of one request per module. Caps keep the request bounded and the web
// watchdog fed.
static void handleApiSendBatch() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  JsonArray frames = doc["frames"].as<JsonArray>();
  if (frames.isNull()) { sendJsonError(400, "'frames' array required"); return; }
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
    // TCP window buffers stack in internal RAM). taskRS485 sends each frame when due.
    // step==0 or a frame too long / a full queue falls back to an immediate send.
    if (step > 0 && rs485SendScheduled(outBuf, outLen, due)) {
      due += (uint32_t)step;
    } else {
      rs485Send(outBuf, outLen, false);
    }
    sent++;
    wdgWebMs = millis();             // this loop is now fast, but stay watchdog-safe
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"sent\":%d}", sent);
  server.send(200, "application/json", resp);
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
static void handleApiDisplayCells() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  JsonArray cells = doc["cells"].as<JsonArray>();
  if (cells.isNull()) { sendJsonError(400, "'cells' array required"); return; }

  int start = doc["start"] | 0;
  if (start < 0 || start > 254) { sendJsonError(400, "'start' must be 0..254"); return; }
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
        sendJsonError(400, e); return;
      }
      flap[n++] = (int16_t)idx;
      continue;
    }

    if (c["ch"].is<const char*>()) {
      const char* ch = c["ch"].as<const char*>();
      uint32_t cp = 0;
      if (!ch || !*ch || utf8Next(ch, &cp) == 0) {
        char e[64]; snprintf(e, sizeof(e), "cell %d: 'ch' is not valid UTF-8", n);
        sendJsonError(400, e); return;
      }
      int idx = vmFlapIndexOfCodepoint(cp);
      if (idx < 0) {
        char e[96];
        snprintf(e, sizeof(e), "cell %d: no flap for U+%04X -- the reel cannot show it",
                 n, (unsigned)cp);
        sendJsonError(400, e); return;
      }
      flap[n++] = (int16_t)idx;
      continue;
    }

    char e[80];
    snprintf(e, sizeof(e), "cell %d: need one of ch, color, blank, skip", n);
    sendJsonError(400, e); return;
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
    if (step > 0 && rs485SendScheduled((const uint8_t*)f, (size_t)len, due)) {
      due += (uint32_t)step;
    } else {
      rs485Send((const uint8_t*)f, (size_t)len, false);
    }
    sent++;
    wdgWebMs = millis();
  }
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"cells\":%d,\"sent\":%d}", n, sent);
  server.send(200, "application/json", resp);
}

/* GET /api/flap/modules
 *
 * The wall IS the modules. There is no registry to consult: every cell of the wall is a
 * module, its id is its cell index, its serial is a deterministic function of the MAC and
 * that index, and its firmware version is a compile-time constant. This reads vmods[]
 * directly -- the one place that actually knows.
 *
 * (There used to be a shadow copy: a sticky SFModule registry, persisted to FATFS, with a
 * stale-probe pruner and a duplicate-ID heuristic. It existed to track modules that appear
 * and disappear on a real bus. Nothing here appears or disappears.)
 */
static void handleApiModules() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // setConnectionTimeout(), NOT setTimeout(): the latter sets Stream's *read* timeout and
  // leaves SO_SNDTIMEO at its 3 s default. This loop does one socket write per module, so on
  // a wedged client 160 modules x 3 s is far past the 120 s web watchdog, which would reboot
  // the board. Bound each write instead.
  server.client().setConnectionTimeout(1500);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  const int n = vmCount;
  for (int i = 0; i < n; i++) {
    // Snapshot one module under the lock, format outside it: the drawing task walks this
    // array at 100 Hz and must not wait on a socket write.
    uint8_t id = 0; char sn[VM_SN_CHARS + 1] = ""; int flap = 0;
    if (vmMutex && xSemaphoreTake(vmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      id   = vmods[i].id;
      flap = vmods[i].curIndex;
      strlcpy(sn, vmods[i].sn, sizeof(sn));
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

    char row[160];
    snprintf(row, sizeof(row),
             "%s{\"id\":%u,\"sn\":\"%s\",\"provisioned\":true,\"fwVersion\":\"%d\","
             "\"flapIndex\":%d,\"flapChar\":\"%s\"}",
             i ? "," : "", (unsigned)id, sn, VM_FW_VERSION, flap, utf8);
    server.sendContent(row);
    wdgWebMs = millis();
  }
  server.sendContent("]");
  server.sendContent("");
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
static void handleApiDisplayState() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.client().setConnectionTimeout(1500);
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

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char head[48];
  snprintf(head, sizeof(head), "{\"rows\":%d,\"cols\":%d,\"cells\":[", rows, cols);
  server.sendContent(head);
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
    server.sendContent(one);
    if ((i & 31) == 0) wdgWebMs = millis();
  }
  server.sendContent("]}");
  server.sendContent("");
}


// POST /api/flap/char   {"id":5,"char":"A"}   id=-1 for broadcast
static void handleApiChar() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  const char* ch = doc["char"] | "";
  if (!ch[0]) { sendJsonError(400, "Missing char"); return; }
  // `ch` is UTF-8: a euro/accented glyph is multi-byte. Transcode to a single
  // Windows-1252 byte and display the first character (see charset.h).
  char enc[8];
  utf8ToFlap(ch, enc, sizeof(enc));
  if (!enc[0]) { sendJsonError(400, "Unsupported character"); return; }
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "char -> all modules");
    else        snprintf(cd, sizeof(cd), "char -> module %d", id);
    logCommand('R', cd); }
  sfSendChar(id, enc[0]);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/index  {"id":5,"index":3}
static void handleApiIndex() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id  = doc["id"]    | -1;
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= 64) { sendJsonError(400, "Invalid index (0-63)"); return; }
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "index %d -> all modules", idx);
    else        snprintf(cd, sizeof(cd), "index %d -> module %d", idx, id);
    logCommand('R', cd); }
  sfSendIndex(id, idx);
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/flap/text   {"text":"HELLO","start":0}
static void handleApiText() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  const char* text = doc["text"] | "";
  int start = doc["start"] | 0;
  if (!text[0]) { sendJsonError(400, "Empty text"); return; }
  { char cd[LOG_TEXT_MAX]; snprintf(cd, sizeof(cd), "text from module %d: \"%.60s\"", start, text);
    logCommand('R', cd); }
  sfSendText(start, text, false);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"chars\":%zu}", strlen(text));
  server.send(200, "application/json", resp);
}

// POST /api/flap/home   {"id":5}  or  {"id":-1}
static void handleApiHome() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  int id = doc["id"] | -1;
  { char cd[LOG_TEXT_MAX];
    if (id < 0) snprintf(cd, sizeof(cd), "home all modules");
    else        snprintf(cd, sizeof(cd), "home module %d", id);
    logCommand('R', cd); }
  sfHome(id);
  server.send(200, "application/json", "{\"ok\":true}");
}


/* ----------------------------------------------------------
   GET /api/capabilities -- what this wall can actually show
   ----------------------------------------------------------
   One call, made once when a client connects, that answers "what characters can this display
   show?" without the client having to know what kind of gateway it is talking to. The RS-485
   gateway answers the same question at the same URL, with the same shape -- which is the whole
   point of it: a client asks the wall, not the hardware.

   On THIS board the answer is short, because the wall is drawn: every module renders from one
   shared reel (reel.h), so there is exactly one flap set and `uniform` is true by construction.
   On the RS-485 gateway every module owns its own reel and they need not agree, so the same
   response can carry several sets -- and there, `union` and `common` genuinely differ.

   `sets` reports each DISTINCT reel once, with the ids that use it, rather than one entry per
   module. A 75-module wall with one reel is one entry and one range, a few hundred bytes; a
   mixed wall costs one entry per genuinely different reel, which is the information itself
   rather than a repetition of it.

   Streamed, like /api/flap/modules: the union string alone is ~300 bytes and nothing here needs
   to exist in RAM all at once.                                                                */
// A BUFFERED writer for the response below.
//
// server.sendContent() is one HTTP chunk and one TCP write. Streaming this response a CHARACTER
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
  server.sendContent(capBuf);
  capLen = 0;
  wdgWebMs = millis();
}
static void capPut(const char* str) {
  size_t n = strlen(str);
  if (capLen + n >= sizeof(capBuf) - 1) capFlush();
  if (n >= sizeof(capBuf) - 1) { server.sendContent(str); return; }   // never truncate a caller
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

static void handleApiCapabilities() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.client().setConnectionTimeout(1500);
  const int rows = gPanel.rows ? gPanel.rows : 1;
  const int cols = gPanel.cols ? gPanel.cols : 1;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
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
  // talking to -- the RS-485 gateway answers this same URL, where they genuinely differ.
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
  // client pacing full-wall animations. The RS-485 gateway answers the same key with kind
  // "mechanical", where the number IS a physical constraint. Stated directly, so a client
  // never has to infer motion from which endpoints exist.
  { char motion[64];
    snprintf(motion, sizeof(motion), "\"motion\":{\"kind\":\"drawn\",\"settleMs\":%lu},",
             (unsigned long)cfg.flapMs * (unsigned long)cfg.flapMax);
    capPut(motion); }

  // Raw canvas and on-device effects are Matrix-only -- the RS-485 wall has no framebuffer to
  // hand out, and answers this URL without these keys. Stated here so the companion lights up
  // canvas/effect controls from capabilities, not from a firmware-version sniff: `canvas` is the
  // framebuffer a client would push frames to, `effects` the on-device animation set.
  { char cv[144];
    snprintf(cv, sizeof(cv),
             "\"canvas\":{\"formats\":[\"rgb888\",\"rgb565\"],\"width\":%u,\"height\":%u},"
             "\"effects\":%s,",
             (unsigned)gPanel.panelW, (unsigned)gPanel.panelH, effectListJson());
    capPut(cv); }

  // What the wall can DO, not just show, so a client reads this instead of sniffing the
  // firmware version and guessing.
  capPut("\"features\":[\"cells\",\"colors\",\"index\",\"lowercase\",\"pictographs\","
         "\"quiet\",\"maintenance\",\"ha\",\"ota\",\"canvas\",\"effects\"]}");
  capFlush();
  server.sendContent("");
}

// GET /api/status
static void handleApiStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // Use snprintf to avoid JsonDocument heap allocation (called every 3s by browser)
  char rtcBuf[24]; rtcFormatTime(rtcBuf, sizeof(rtcBuf));
  IPAddress lip = WiFi.localIP(), aip = WiFi.softAPIP();
  // Per-task minimum-ever free stack (bytes). A value trending toward 0 is an
  // early warning of the stack-canary crash class.
  unsigned stk485 = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
  unsigned stkWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
  unsigned stkNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
  unsigned stkOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
  unsigned stkRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
  // v3.0: seconds since the companion last checked in (-1 = never / deregistered)
  long compAge = gCompanionSeenMs ? (long)((millis() - gCompanionSeenMs) / 1000UL) : -1;
  unsigned stkDsp = hTaskDisp ? uxTaskGetStackHighWaterMark(hTaskDisp) : 0;
  char out[900];
  snprintf(out, sizeof(out),
    "{\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu,"
    "\"wifi\":%s,\"ip\":\"%d.%d.%d.%d\",\"apip\":\"%d.%d.%d.%d\","
    "\"heap\":%u,\"minheap\":%u,\"mqtt\":%s,\"modules\":%d,"
    "\"stk\":{\"rs485\":%u,\"web\":%u,\"net\":%u,\"ota\":%u,\"rtc\":%u,\"disp\":%u},"
    "\"panel\":{\"ok\":%s,\"w\":%u,\"h\":%u,\"cols\":%u,\"rows\":%u,"
    "\"cellW\":%u,\"cellH\":%u,\"font\":\"%s\",\"vmods\":%d,\"drop\":%lu},"
    "\"time\":\"%s\",\"ntpSynced\":%s,\"maint\":%s,\"quiet\":%s,"
    "\"companion\":{\"url\":\"%s\",\"status\":\"%s\",\"age\":%ld}}",
    millis()/1000, rxCount, txCount,
    (WiFi.status()==WL_CONNECTED)?"true":"false",
    lip[0],lip[1],lip[2],lip[3],
    aip[0],aip[1],aip[2],aip[3],
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    mqtt.connected()?"true":"false",
    vmCount,
    stk485, stkWeb, stkNet, stkOta, stkRtc, stkDsp,
    gPanel.ready?"true":"false", gPanel.panelW, gPanel.panelH,
    gPanel.cols, gPanel.rows, gPanel.cellW, gPanel.cellH,
    dispFontName(), vmCount, vbusDropped,
    rtcBuf,
    ntpSynced?"true":"false",
    gMaintenanceMode?"true":"false",
    gQuietTime?"true":"false",
    cfg.companionUrl, gCompanionStatus, compAge);
  server.send(200, "application/json", out);
}

// GET /api/config
static void handleApiConfigGet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  JsonDocument doc;
  // "version" is the GATEWAY API level, not this firmware's version. The
  // companion parses MAJOR.MINOR out of it and enables its gateway-stored
  // settings on >= 3.1; this firmware implements that surface exactly, so it
  // must answer 3.1.0. "product" and "fwVersion" are what tell the two apart.
  doc["version"]   = API_VERSION;
  doc["product"]   = PRODUCT_NAME;
  doc["fwVersion"] = FW_VERSION;
  doc["wSSID"]    = cfg.wifiSSID;
  doc["mqHost"]   = cfg.mqttHost;
  doc["mqPort"]   = cfg.mqttPort;
  doc["mqUser"]   = cfg.mqttUser;
  doc["mqPfx"]    = cfg.mqttPrefix;
  doc["posixTZ"]    = cfg.posixTZ;
  doc["ntpServer"]  = cfg.ntpServer;
  doc["gridRows"]   = cfg.gridRows;
  doc["gridCols"]   = cfg.gridCols;
  doc["serialDebug"]   = cfg.serialDebug;
  doc["haEnabled"]     = cfg.haEnabled;
  doc["otaPasswordSet"] = (strlen(cfg.otaPassword) > 0);
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
  doc["gridColor"]     = cfg.gridColor;   // 0xRRGGBB seam colour
  doc["gridBright"]    = cfg.gridBright;   // 0 = grid off
  doc["maxFlaps"]      = SF_MAX_FLAPS;   // 163: the reel carries every CP1252 glyph
  char out[1280];   // headroom for identity + panel + JSON-escaped SSID/TZ/hostname
  serializeJson(doc, out, sizeof(out));
  server.send(200, "application/json", out);
}

// POST /api/config/wifi
static void handleApiConfigWifi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  saveConfig();
  DBG("[CFG] WiFi SSID set to '%s'\n", cfg.wifiSSID);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  WiFi.disconnect();
}

// POST /api/config/mqtt
static void handleApiConfigMqtt() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  strlcpy(cfg.mqttHost,   doc["host"]   | "", sizeof(cfg.mqttHost));
  cfg.mqttPort          = doc["port"]   | DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttUser,   doc["user"]   | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   doc["pass"]   | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, doc["prefix"] | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  saveConfig();
  DBG("[CFG] MQTT broker set to %s:%d  prefix=%s\n", cfg.mqttHost, cfg.mqttPort, cfg.mqttPrefix);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  mqtt.disconnect();
  // PubSubClient caches the broker it was last told to dial. mqttInit() is the only
  // other caller and runs once at boot, so without this the new broker is saved and
  // logged but never actually dialled: the reconnect keeps using the boot-time target
  // (or none at all, if none was configured then) and fails forever with rc=-2.
  if (strlen(cfg.mqttHost)) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttFailCount = 0;   // a fresh broker gets a fresh 5-strike budget
}


// POST /api/config/settings  -- save all settings in one call
static void handleApiConfigSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Bad JSON"); return;
  }
  // WiFi
  if (doc["ssid"].is<const char*>()) strlcpy(cfg.wifiSSID, doc["ssid"] | "", sizeof(cfg.wifiSSID));
  if (doc["pass"].is<const char*>()) strlcpy(cfg.wifiPass, doc["pass"] | "", sizeof(cfg.wifiPass));
  // MQTT
  if (doc["mqHost"].is<const char*>())   strlcpy(cfg.mqttHost,   doc["mqHost"]   | "", sizeof(cfg.mqttHost));
  if (doc["mqPort"].is<int>())           cfg.mqttPort = doc["mqPort"];
  if (doc["mqUser"].is<const char*>())   strlcpy(cfg.mqttUser,   doc["mqUser"]   | "", sizeof(cfg.mqttUser));
  if (doc["mqPass"].is<const char*>())   strlcpy(cfg.mqttPass,   doc["mqPass"]   | "", sizeof(cfg.mqttPass));
  if (doc["mqPfx"].is<const char*>())    strlcpy(cfg.mqttPrefix, doc["mqPfx"]    | DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  // Timezone
  // OTA password update
  if (doc["otaPassword"].is<const char*>()) {
    strlcpy(cfg.otaPassword, doc["otaPassword"] | "", sizeof(cfg.otaPassword));
    saveConfig();
    if (strlen(cfg.otaPassword) > 0) {
      ArduinoOTA.setPassword(cfg.otaPassword);
    }
    printf("[CFG] OTA password updated\n");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Serial debug toggle
  if (doc["serialDebug"].is<bool>()) {
    cfg.serialDebug = doc["serialDebug"].as<bool>();
    gSerialDebug    = cfg.serialDebug;
    saveConfig();
    printf("[CFG] Serial debug %s\n", cfg.serialDebug ? "enabled" : "disabled");
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  // Home Assistant integration toggle
  if (doc["haEnabled"].is<bool>()) {
    bool was = cfg.haEnabled;
    cfg.haEnabled = doc["haEnabled"].as<bool>();
    saveConfig();
    printf("[CFG] Home Assistant integration %s\n", cfg.haEnabled ? "enabled" : "disabled");
    if (mqtt.connected()) {
      if (cfg.haEnabled && !was) { haPublishDiscovery(true); mqttPublishStateTopics(); }
      else if (!cfg.haEnabled && was) { haPublishDiscovery(false); }  // remove entities
    }
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (doc["posixTZ"].is<const char*>()) {
    strlcpy(cfg.posixTZ, doc["posixTZ"] | "UTC0", sizeof(cfg.posixTZ));
    strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
    setenv("TZ", gPosixTZ, 1);
    tzset();
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
    // Unlike the RS-485 gateway, this grid is the PHYSICAL wall: every cell is a
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
    if (v >= 1 && v <= 255) cfg.panelBright = (uint8_t)v; }
  if (doc["flapMs"].is<int>())        { int v = doc["flapMs"];
    if (v >= 2 && v <= 500) cfg.flapMs = (uint16_t)v; }
  if (doc["flapMax"].is<int>())       { int v = doc["flapMax"];
    if (v >= 1 && v <= FLAP_ANIM_MAX) cfg.flapMax = (uint8_t)v; }
  // Grid seam colour + intensity. Both live (dispMarkDirty below repaints the wall).
  if (doc["gridColor"].is<int>())     { long v = doc["gridColor"].as<long>();
    if (v >= 0 && v <= 0xFFFFFF) cfg.gridColor = (uint32_t)v; }
  if (doc["gridBright"].is<int>())    { int v = doc["gridBright"];
    if (v >= 0 && v <= 255) cfg.gridBright = (uint8_t)v; }
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
    else { sendJsonError(400, "hostname must be 1-31 chars of a-z 0-9 -"); return; }
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
  server.send(200, "application/json", resp);
  delay(100);
  // Only disconnect/reconnect if WiFi or MQTT credentials were in the payload
  bool hasWifi = doc["ssid"].is<const char*>() || doc["pass"].is<const char*>();
  bool hasMqtt = doc["mqHost"].is<const char*>() || doc["mqPort"].is<int>();
  if (hasMqtt) mqtt.disconnect();
  if (hasWifi) WiFi.disconnect();
}

static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  // PUT is used by /api/companion/settings (v3.1); the rest of the API is GET/POST.
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}


// ?? New command handlers ??????????????????????????????????????????






// POST /api/mqtt/test {host?,port?,user?,pass?} -- tries the given (or saved)
// broker settings WITHOUT touching the live connection, so settings can be
// verified before saving. Two phases: TCP reachability (3s cap), then a real
// MQTT CONNECT/CONNACK using a throwaway client. Runs on taskWeb (8KB stack;
// the temporary client objects are small and its packet buffer is heap).
static void handleApiMqttTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  char host[64]; int port = cfg.mqttPort;
  char user[48], pass[64];
  strlcpy(host, cfg.mqttHost, sizeof(host));
  strlcpy(user, cfg.mqttUser, sizeof(user));
  strlcpy(pass, cfg.mqttPass, sizeof(pass));
  if (server.hasArg("plain") && server.arg("plain").length() > 1) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      if (doc["host"].is<const char*>()) strlcpy(host, doc["host"] | "", sizeof(host));
      if (doc["port"].is<int>())         port = doc["port"] | cfg.mqttPort;
      if (doc["user"].is<const char*>()) strlcpy(user, doc["user"] | "", sizeof(user));
      if (doc["pass"].is<const char*>()) strlcpy(pass, doc["pass"] | "", sizeof(pass));
    }
  }
  if (!host[0]) { sendJsonError(400, "no broker host configured"); return; }
  DBG("[MQTT] testing %s:%d\n", host, port);

  wdgWebMs = millis();
  WiFiClient testNet;
  // Phase 1: TCP reachability with an explicit 3s cap.
  if (!testNet.connect(host, (uint16_t)port, 3000)) {
    server.send(200, "application/json",
      "{\"ok\":false,\"tcp\":false,\"mqtt\":false,"
      "\"error\":\"TCP connect failed (host/port unreachable)\"}");
    return;
  }
  wdgWebMs = millis();
  // Phase 2: real MQTT CONNECT on the already-open socket. PubSubClient skips
  // its own TCP connect when the client is connected, so this only exchanges
  // CONNECT/CONNACK. CONNACK from a live broker arrives in milliseconds.
  PubSubClient testMq(testNet);
  testMq.setBufferSize(128);   // CONNECT/CONNACK only -- keep the heap use tiny
  bool mqOk;
  if (user[0]) mqOk = testMq.connect("sfgw-test", user, pass);
  else         mqOk = testMq.connect("sfgw-test");
  int state = testMq.state();
  testMq.disconnect();
  testNet.stop();
  wdgWebMs = millis();

  const char* why = "";
  switch (state) {                       // PubSubClient state codes
    case  0: why = "connected";                       break;
    case  1: why = "bad protocol version";            break;
    case  2: why = "client id rejected";              break;
    case  3: why = "broker unavailable";              break;
    case  4: why = "bad username or password";        break;
    case  5: why = "not authorized";                  break;
    case -2: why = "network failed during handshake"; break;
    case -4: why = "broker did not respond (timeout)";break;
    default: why = "connection failed";               break;
  }
  char out[160];
  snprintf(out, sizeof(out),
    "{\"ok\":%s,\"tcp\":true,\"mqtt\":%s,\"state\":%d,\"detail\":\"%s\"}",
    mqOk ? "true" : "false", mqOk ? "true" : "false", state, why);
  server.send(200, "application/json", out);
}

// GET/POST /api/maintenance
static void handleApiMaintenance() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  // GET returns current state; POST {"on":true|false} sets it.
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    gMaintenanceMode = doc["on"].as<bool>();
    printf("[MAINT] Maintenance mode %s\n", gMaintenanceMode ? "ENABLED" : "disabled");
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}",
           gMaintenanceMode ? "true" : "false");
  server.send(200, "application/json", out);
}

// GET returns Quiet Time state; POST {"on":true|false} sets it.
static void handleApiQuiet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
    if (!doc["on"].is<bool>()) { sendJsonError(400, "'on' (bool) required"); return; }
    bool on = doc["on"].as<bool>();
    // The schedule wins inside its window: refuse a manual OFF here too (see the
    // MQTT handler for the rationale). Disable the schedule to override.
    if (!on && quietSchedInWindow()) {
      printf("[QUIET] REST quiet OFF ignored -- schedule active (in window)\n");
    } else {
      sfSetQuietTime(on);
    }
    mqttPublishStateTopics();
  }
  char out[40];
  snprintf(out, sizeof(out), "{\"ok\":true,\"on\":%s}", gQuietTime ? "true" : "false");
  server.send(200, "application/json", out);
}

// GET/POST /api/quiet/schedule  -- daily Quiet-Time schedule (v3.0).
// The schedule is evaluated once a second in taskRTC; when the current local
// time enters/leaves the window, Quiet Time is toggled automatically.
static void handleApiQuietSchedule() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
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
  server.send(200, "application/json", buf);
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
static const char* const GW_TAB_ID[]  = {"display", "monitor", "settings", "status"};
static const char* const GW_TAB_LBL[] = {"Display", "Monitor", "Settings", "Status"};
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

static void handleApiCompanion() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { sendJsonError(400, "No body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Bad JSON"); return;
    }
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
  server.send(200, "application/json", buf);
}

/* ----------------------------------------------------------
   Companion settings blob  (v3.1)

   A stateless companion keeps its settings/playlists/triggers here instead of on
   its own disk. The payload is gzip(minified JSON) whose schema belongs entirely
   to the companion -- the gateway stores the bytes verbatim and never parses them.

   The body is binary, which rules out server.arg("plain"): the WebServer copies a
   non-form body through String(char*), so it would stop at the first NUL byte --
   and a gzip header carries one at offset 3. Instead the PUT registers an upload
   callback, which makes the WebServer take its "raw" path and stream the body to
   us in HTTP_RAW_BUFLEN chunks. handleApiCompanionSettingsRaw writes those chunks
   straight to a temp file; handleApiCompanionSettingsPut then sends the response.
---------------------------------------------------------- */
static File   compFile;          // temp file, open across the RAW_WRITE chunks
static size_t compRecvd = 0;     // bytes written so far this request
static int    compErr   = 0;     // 0 = ok, else the HTTP status to report

// Give up on the transfer: close + delete the temp file, remember the status.
// The WebServer keeps draining the socket either way (we cannot stop its read
// loop), so every later chunk is simply dropped and the response still lands.
static void compAbort(int status) {
  if (compFile) compFile.close();
  if (sfFsReady) FFat.remove(COMPANION_TMP);
  compErr = status;
}

// PUT /api/companion/settings -- raw body callback (one call per chunk)
static void handleApiCompanionSettingsRaw() {
  HTTPRaw& raw = server.raw();
  wdgWebMs = millis();          // a slow client must not trip the web watchdog

  switch (raw.status) {
    case RAW_START: {
      compRecvd = 0;
      compErr   = 0;
      size_t len = (size_t)server.clientContentLength();
      // Decide before opening anything, so a bad request never touches flash.
      if (!sfFsReady)                  { compErr = 503; break; }  // no filesystem mounted
      if (len == 0)                    { compErr = 400; break; }  // nothing to store
      if (len > COMPANION_MAX_BYTES)   { compErr = 413; break; }
      FFat.remove(COMPANION_TMP);                                 // clear a stale temp file
      compFile = FFat.open(COMPANION_TMP, "w");
      if (!compFile) compErr = 507;
      break;
    }

    case RAW_WRITE:
      if (compErr || !compFile) break;                         // already failed -- drain
      // Content-Length was checked up front, but a body may overrun it; re-check
      // so a lying header still cannot fill the flash.
      if (compRecvd + raw.currentSize > COMPANION_MAX_BYTES) { compAbort(413); break; }
      if (compFile.write(raw.buf, raw.currentSize) != raw.currentSize) { compAbort(507); break; }
      compRecvd += raw.currentSize;
      break;

    case RAW_END:
      if (compErr) { compAbort(compErr); break; }              // reuse the cleanup path
      compFile.close();
      // A truncated body (fewer bytes than promised) must not overwrite good settings.
      if (compRecvd != (size_t)server.clientContentLength()) { compAbort(400); break; }
      // Publish atomically: the old blob survives intact until the rename lands.
      FFat.remove(COMPANION_FILE);
      if (!FFat.rename(COMPANION_TMP, COMPANION_FILE)) { compAbort(507); break; }
      DBG("[CFG] Companion settings stored (%u bytes)\n", (unsigned)compRecvd);
      break;

    case RAW_ABORTED:
      compAbort(400);
      break;
  }
}

// PUT /api/companion/settings -- response, after the raw body has been consumed
static void handleApiCompanionSettingsPut() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int err = compErr;
  compErr = 0;                  // never leak this request's verdict into the next
  switch (err) {
    case 0:   break;
    case 400: sendJsonError(400, "Empty or truncated body"); return;
    case 413: sendJsonError(413, "Settings blob too large"); return;
    case 503: sendJsonError(503, "No filesystem");         return;
    default:  sendJsonError(507, "Write failed");          return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%u}", (unsigned)compRecvd);
  server.send(200, "application/json", buf);
}

// GET /api/companion/settings -- hand the stored blob back byte-for-byte
static void handleApiCompanionSettingsGet() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  if (!sfFsReady || !FFat.exists(COMPANION_FILE)) { sendJsonError(404, "No settings stored"); return; }
  File f = FFat.open(COMPANION_FILE, "r");
  if (!f) { sendJsonError(404, "No settings stored"); return; }
  size_t n = f.size();
  if (n == 0) { f.close(); sendJsonError(404, "No settings stored"); return; }

  wdgWebMs = millis();
  server.client().setTimeout(3000);        // cap per-write blocking on a stalled client
  server.setContentLength(n);
  // Deliberately NOT "Content-Encoding: gzip": these bytes are the payload, not a
  // transfer encoding of it. Declaring the encoding would make HTTP clients gunzip
  // the body transparently, and the companion -- which decompresses itself -- would
  // then be handed plain JSON it tries to decompress again.
  server.send(200, "application/gzip", "");
  uint8_t buf[512];
  while (size_t got = f.read(buf, sizeof(buf))) {
    server.sendContent((const char*)buf, got);
    wdgWebMs = millis();
  }
  f.close();
}

/* ----------------------------------------------------------
   Raw canvas  (Matrix gateway only; the RS-485 gateway has no framebuffer to expose)
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
  gEffect = EFFECT_NONE;          // a raw canvas supersedes any on-device effect
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
static void canvasLeave() {                                    // hand the panel back to the wall
  if (gCanvasMode || gEffect != EFFECT_NONE) {
    gCanvasMode = false; gEffect = EFFECT_NONE; dispMarkDirty();
  }
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
// One CP1252 string at (x,y) top-left, one flap-font glyph per byte, solid colour. Fixed-width
// faces, so the advance is just the face width; bit 15 of each row is the leftmost column.
static void canvasText(int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b,
                       const Font1252* f) {
  int cx = x;
  for (const uint8_t* p = (const uint8_t*)s; *p; ++p) {
    uint8_t gi = FONT1252_INDEX[*p];
    if (gi != 0xFF)
      for (int row = 0; row < f->height; row++) {
        uint16_t bits = font1252Row(*f, gi, row);
        for (int col = 0; col < f->width; col++)
          if (bits & (uint16_t)(0x8000 >> col)) panelPixel(cx + col, y + row, r, g, b);
      }
    cx += f->width;
  }
}
// A [r,g,b] triple from an op field, leaving the caller's defaults untouched when absent.
static void canvasColor(JsonVariantConst c, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (c.is<JsonArrayConst>() && c.size() >= 3) {
    r = (uint8_t)c[0].as<int>(); g = (uint8_t)c[1].as<int>(); b = (uint8_t)c[2].as<int>();
  }
}

// GET  /api/canvas -> {active,width,height,formats}   POST {"active":bool} take over / release.
static void handleApiCanvas() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJsonError(400, "Invalid JSON"); return;
    }
    if (doc["active"] | false) canvasEnter(true); else canvasLeave();
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"active\":%s}", gCanvasMode ? "true" : "false");
    server.send(200, "application/json", buf);
    return;
  }
  char buf[224];
  snprintf(buf, sizeof(buf),
           "{\"active\":%s,\"width\":%u,\"height\":%u,\"formats\":[\"rgb888\",\"rgb565\"],"
           "\"effect\":\"%s\",\"effects\":%s}",
           gCanvasMode ? "true" : "false", (unsigned)gPanel.panelW, (unsigned)gPanel.panelH,
           effectName(gEffect), effectListJson());
  server.send(200, "application/json", buf);
}

// POST /api/canvas/effect  {"type":"plasma|fire|matrix|none","speed":1..10}
// Start an on-device effect (rendered by taskDisplay at the panel's native rate) or, with
// "none", return to the wall. Supersedes raw-canvas mode -- the display task, not HTTP, owns the
// panel -- so it clears gCanvasMode too.
static void handleApiCanvasEffect() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!gPanel.ready) { sendJsonError(503, "Panel not running"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonError(400, "Invalid JSON"); return;
  }
  uint8_t e  = effectByName(doc["type"] | "none");
  int     sp = doc["speed"] | (int)gEffectSpeed;
  gEffectSpeed = (uint8_t)(sp < 1 ? 1 : sp > 10 ? 10 : sp);
  gEffect = EFFECT_NONE;            // stop any running effect first...
  gCanvasMode = false;
  delay(40);                        // ...and let an in-flight effectRender() finish before we reset
  if (e == EFFECT_NONE) {
    dispMarkDirty();                // return to the reel wall
  } else {
    effectReset(e);
    gEffect = e;                    // set last: taskDisplay renders only once its state is ready
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"effect\":\"%s\",\"speed\":%u}",
           effectName(gEffect), (unsigned)gEffectSpeed);
  server.send(200, "application/json", buf);
}

// POST /api/canvas/ops -- a JSON array of draw commands, applied in order then presented.
// Ops: clear | pixel | hline | vline | rect(+fill) | text | show. Colours are [r,g,b], default
// white (black for clear). Auto-takes-over the panel.
static void handleApiCanvasOps() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!gPanel.ready) { sendJsonError(503, "Panel not running"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok || !doc.is<JsonArray>()) {
    sendJsonError(400, "Body must be a JSON array of ops"); return;
  }
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
    } else if (!strcmp(k, "text")) {
      uint8_t r = 255, g = 255, b = 255; canvasColor(op["color"], r, g, b);
      canvasText(x, y, op["s"] | "", r, g, b, canvasFace(op["size"] | 10));
    } else if (!strcmp(k, "show")) {
      panelShow(); shown = true; continue;
    } else continue;                      // unknown op: skip, do not count
    applied++;
  }
  if (!shown) panelShow();
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"applied\":%d}", applied);
  server.send(200, "application/json", buf);
}

// PUT /api/canvas/frame -- a full raw frame, width*height pixels, row-major, top-left origin.
// The pixel format follows from the body LENGTH: W*H*3 is rgb888 (3 bytes/px), W*H*2 is rgb565
// (2 bytes/px, big-endian). The ?fmt= hint is not read here -- the WebServer discards URL query
// args once it begins streaming a raw body -- so the length is authoritative. Streamed straight
// to the back buffer so no multi-KB frame is ever buffered whole; presented on RAW_END.
static volatile int canvasErr = 0;
static uint32_t     canvasPix = 0;      // pixels written this frame
static uint8_t      canvasCarry[3];     // partial pixel across a chunk boundary
static uint8_t      canvasCarryN = 0;
static uint8_t      canvasBpp = 3;
static void handleApiCanvasFrameRaw() {
  HTTPRaw& raw = server.raw();
  wdgWebMs = millis();                   // a slow client must not trip the web watchdog
  switch (raw.status) {
    case RAW_START: {
      canvasErr = 0; canvasPix = 0; canvasCarryN = 0;
      if (!gPanel.ready) { canvasErr = 503; break; }
      // Infer the pixel format from the body length -- a full frame is unambiguously W*H*3
      // (rgb888) or W*H*2 (rgb565). server.arg("fmt") reads empty in a raw handler, so the
      // length is the source of truth; the client's ?fmt= is only a hint.
      const size_t px  = (size_t)gPanel.panelW * gPanel.panelH;
      const size_t len = (size_t)server.clientContentLength();
      if      (len == px * 3) canvasBpp = 3;
      else if (len == px * 2) canvasBpp = 2;
      else { canvasErr = 400; break; }
      canvasStandDown();                 // stand the wall down and wait for the renderer to park
      break;
    }
    case RAW_WRITE:
      if (canvasErr) break;
      for (size_t i = 0; i < raw.currentSize; i++) {
        canvasCarry[canvasCarryN++] = raw.buf[i];
        if (canvasCarryN < canvasBpp) continue;
        canvasCarryN = 0;
        uint8_t r, g, b;
        if (canvasBpp == 3) { r = canvasCarry[0]; g = canvasCarry[1]; b = canvasCarry[2]; }
        else { uint16_t p = ((uint16_t)canvasCarry[0] << 8) | canvasCarry[1];
               r = (uint8_t)(((p >> 11) & 0x1F) << 3);
               g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
               b = (uint8_t)(( p        & 0x1F) << 3); }
        panelPixel((int)(canvasPix % gPanel.panelW), (int)(canvasPix / gPanel.panelW), r, g, b);
        canvasPix++;
      }
      break;
    case RAW_END:
      if (!canvasErr && gPanel.ready) panelShow();
      break;
    case RAW_ABORTED:
      canvasErr = 400;
      break;
  }
}
static void handleApiCanvasFrame() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int err = canvasErr; canvasErr = 0;
  switch (err) {
    case 0:   break;
    case 400: sendJsonError(400, "Body length must equal width*height*3 (rgb888) or *2 (rgb565)"); return;
    case 503: sendJsonError(503, "Panel not running"); return;
    default:  sendJsonError(500, "Canvas frame failed"); return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"width\":%u,\"height\":%u,\"pixels\":%lu}",
           (unsigned)gPanel.panelW, (unsigned)gPanel.panelH, (unsigned long)canvasPix);
  server.send(200, "application/json", buf);
}

void webInit() {
  static const char* COLLECT_HDRS[] = { "If-None-Match" };
  server.collectHeaders(COLLECT_HDRS, 1);   // so handleRoot can honour conditional GETs
  server.on("/",                     HTTP_GET,     handleRoot);
  server.on("/favicon.svg",          HTTP_GET,     handleFavicon);
  server.on("/logo.svg",             HTTP_GET,     handleLogo);
  // v1.1: one route per UI language, "/lang/<code>". Registering them individually
  // (rather than parsing a path parameter) keeps the URI matcher plain and makes an
  // unknown code fall through to the normal 404. English is never registered -- it is
  // the text already in the page.
  for (size_t i = 0; i < UI_LANG_COUNT; i++) {
    server.on((String("/lang/") + UI_LANGS[i].code).c_str(), HTTP_GET, [i]() { handleLang(i); });
  }
  server.on("/ota",                  HTTP_GET,     handleOTAPage);
  server.on("/api/ota/upload",       HTTP_POST,    sendOTAUploadResult, handleOTAUpload);
  server.on("/api/log",              HTTP_GET,     handleApiMessages);
  server.on("/api/rs485/send",       HTTP_POST,    handleApiSend);
  server.on("/api/rs485/send",       HTTP_OPTIONS, handleOptions);
  server.on("/api/rs485/batch",      HTTP_POST,    handleApiSendBatch);
  server.on("/api/rs485/batch",      HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/modules",     HTTP_GET,     handleApiModules);
  server.on("/api/display/state",    HTTP_GET,     handleApiDisplayState);
  server.on("/api/display/cells",    HTTP_POST,    handleApiDisplayCells);
  server.on("/api/display/cells",    HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/char",        HTTP_POST,    handleApiChar);
  server.on("/api/flap/char",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/index",       HTTP_POST,    handleApiIndex);
  server.on("/api/flap/index",       HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/text",        HTTP_POST,    handleApiText);
  server.on("/api/flap/text",        HTTP_OPTIONS, handleOptions);
  server.on("/api/flap/home",        HTTP_POST,    handleApiHome);
  server.on("/api/flap/home",        HTTP_OPTIONS, handleOptions);
  server.on("/api/capabilities",     HTTP_GET,     handleApiCapabilities);
  server.on("/api/capabilities",     HTTP_OPTIONS, handleOptions);
  server.on("/api/status",           HTTP_GET,     handleApiStatus);
  server.on("/api/mqtt/test",        HTTP_POST,    handleApiMqttTest);
  server.on("/api/mqtt/test",        HTTP_OPTIONS, handleOptions);
  server.on("/api/maintenance",      HTTP_GET,     handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_POST,    handleApiMaintenance);
  server.on("/api/maintenance",      HTTP_OPTIONS, handleOptions);
  server.on("/api/quiet",            HTTP_GET,     handleApiQuiet);
  server.on("/api/quiet",            HTTP_POST,    handleApiQuiet);
  server.on("/api/quiet",            HTTP_OPTIONS, handleOptions);
  server.on("/api/quiet/schedule",   HTTP_GET,     handleApiQuietSchedule);
  server.on("/api/quiet/schedule",   HTTP_POST,    handleApiQuietSchedule);
  server.on("/api/quiet/schedule",   HTTP_OPTIONS, handleOptions);
  server.on("/api/companion",        HTTP_GET,     handleApiCompanion);
  server.on("/api/companion",        HTTP_POST,    handleApiCompanion);
  server.on("/api/companion",        HTTP_OPTIONS, handleOptions);
  // v3.1 blob store. Passing the 4th (upload) callback is what puts the PUT on the
  // WebServer's raw-body path, so the binary gzip arrives intact.
  server.on("/api/companion/settings", HTTP_GET,     handleApiCompanionSettingsGet);
  server.on("/api/companion/settings", HTTP_PUT,     handleApiCompanionSettingsPut,
                                                     handleApiCompanionSettingsRaw);
  server.on("/api/companion/settings", HTTP_OPTIONS, handleOptions);
  server.on("/api/config",           HTTP_GET,     handleApiConfigGet);
  server.on("/api/config/wifi",      HTTP_POST,    handleApiConfigWifi);
  server.on("/api/config/wifi",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/mqtt",      HTTP_POST,    handleApiConfigMqtt);
  server.on("/api/config/mqtt",      HTTP_OPTIONS, handleOptions);
  server.on("/api/config/settings",  HTTP_POST,    handleApiConfigSettings);
  server.on("/api/config/settings",  HTTP_OPTIONS, handleOptions);
  // Raw canvas (Matrix-only): direct pixel control of the panel.
  server.on("/api/canvas",           HTTP_GET,     handleApiCanvas);
  server.on("/api/canvas",           HTTP_POST,    handleApiCanvas);
  server.on("/api/canvas",           HTTP_OPTIONS, handleOptions);
  server.on("/api/canvas/frame",     HTTP_PUT,     handleApiCanvasFrame, handleApiCanvasFrameRaw);
  server.on("/api/canvas/frame",     HTTP_OPTIONS, handleOptions);
  server.on("/api/canvas/ops",       HTTP_POST,    handleApiCanvasOps);
  server.on("/api/canvas/ops",       HTTP_OPTIONS, handleOptions);
  server.on("/api/canvas/effect",    HTTP_POST,    handleApiCanvasEffect);
  server.on("/api/canvas/effect",    HTTP_OPTIONS, handleOptions);
  server.begin();
  printf("[Web] HTTP server %s (port 80)\n",
         server.listening() ? "started" : "FAILED to bind -- taskWeb will retry");
}

// server.begin() is called exactly once, at boot, and its result was never checked; a silent bind
// failure there refuses every request for the whole uptime because nothing re-calls begin(). This
// is that retry. listening() is the ground truth (NetworkServer::_listening), so on a healthy
// server this is a no-op -- it re-establishes the listener only when it is genuinely down, which
// also recovers the case where begin() ran (at setup) before the network stack was ready. Called
// every 20s from taskWeb. Returns true when the listener is live.
bool webEnsureListening() {
  if (server.listening()) return true;
  printf("[Web] port 80 listener is down -- re-establishing\n");
  server.begin();
  const bool up = server.listening();
  printf("[Web] listener %s\n", up ? "restored" : "still down (will retry)");
  return up;
}
