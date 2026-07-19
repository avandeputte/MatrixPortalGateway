#include "gateway.h"



// ota.cpp -- firmware over-the-air update: the browser/raw-body upload
// (handleOTAUpload via the Update library). During an upload gOtaInProgress
// throttles other work and the AP/modem-sleep are tuned to free heap for the
// transfer; otaRestoreWifi puts things back afterwards.
//
// v3.0 dropped ArduinoOTA (the Arduino-IDE espota push path, and its taskOTA +
// UDP listener): it was never used -- every flash here is the web upload or
// esptool over USB -- and it cost a 4 KB task plus sockets. otaInit() now just
// brings up mDNS, which ArduinoOTA.begin() used to do as a side effect.
// ---- file-private forward declarations ----
static void otaRestoreWifi();

// ---------------------------------------------------------------------------
// mDNS -- http://<hostname>.local
// ---------------------------------------------------------------------------
void otaInit() {
  MDNS.begin(cfgHostname());
  MDNS.addService("http", "tcp", 80);
  printf("[OTA] Ready (hostname: %s, web UI at http://%s.local)\n",
         cfgHostname(), cfgHostname());
}

// ---------------------------------------------------------------------------
// Web-based OTA firmware upload
// ---------------------------------------------------------------------------
esp_err_t handleOTAPage(httpd_req_t* r) {
  // Served as a standalone page at /ota so the upload iframe works cleanly.
  // A compile-time constant, streamed with send_P: the old code copied it into
  // a heap String on every request for no benefit.
  static const char html[] = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Firmware Update</title>"
    "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#eaeaea;padding:30px}"
    "h2{color:#e94560}progress{width:100%;height:20px;margin-top:10px}"
    "input[type=file]{color:#eaeaea}button{margin-top:10px;padding:8px 20px;"
    "background:#e94560;border:none;color:#fff;border-radius:4px;cursor:pointer}"
    "#status{margin-top:14px;font-size:.9rem}</style></head><body>"
    "<h2>Firmware Update</h2>"
    "<p style='color:#888;font-size:.85rem'>Select a compiled .bin file. "
    "The gateway will reboot automatically after a successful upload.</p>"
    "<input type='file' id='fw' accept='.bin'>"
    "<br><button onclick='upload()'>Upload Firmware</button>"
    "<progress id='prog' value='0' max='100' style='display:none'></progress>"
    "<div id='status'></div>"
    "<script>"
    "function upload(){"
    "var f=document.getElementById('fw').files[0];"
    "if(!f){document.getElementById('status').textContent='No file selected.';return;}"
    "var xhr=new XMLHttpRequest();"
    "xhr.upload.onprogress=function(e){"
    "if(e.lengthComputable){"
    "var p=Math.round(e.loaded*100/e.total);"
    "document.getElementById('prog').style.display='';"
    "document.getElementById('prog').value=p;"
    "document.getElementById('status').textContent='Uploading: '+p+'%';}"
    "};"
    "xhr.onload=function(){"
    "if(xhr.status===200){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(76,175,80)\">Upload successful! Rebooting... This page will stop responding; wait ~20s and reload.</span>';"
    "}else{"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Error: '+(xhr.responseText||'upload failed')+'</span>';}"
    "};"
    "xhr.onerror=function(){"
    "document.getElementById('status').innerHTML="
    "'<span style=\"color:rgb(233,69,96)\">Upload failed (connection error).</span>';"
    "};"
    "xhr.open('POST','/api/ota/upload');"
    "xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "xhr.send(f);"      // v3.0: raw body, no multipart -- the server streams it straight to flash
    "}"
    "</script></body></html>";
  return httpxSend(r, 200, "text/html", html);
}

// Bring the fallback SoftAP up or down, switching WiFi mode accordingly.
//   AP up   -> WIFI_AP_STA (AP for config + station keeps trying/holding link)
//   AP down -> WIFI_STA    (station only; no AP buffers/beacons)
// Only acts on an actual change so we never thrash the radio. The AP is a
// fallback: callers bring it up when the station is down (or no credentials are
// configured) and drop it once the station connects.
void wifiSetApActive(bool up) {
  if (up == gApActive) return;
  if (up) {
    WiFi.mode(WIFI_AP_STA);
    // The AP is the first-time-setup path, so it must be unique too: two gateways in
    // setup mode on one bench would otherwise both shout "Matrix-Portal-GW".
    WiFi.softAP(cfgHostname(), DEFAULT_AP_PASS);
    IPAddress b = WiFi.softAPIP();
    printf("[WiFi] Fallback AP up: %s  %d.%d.%d.%d\n", cfgHostname(), b[0],b[1],b[2],b[3]);
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    printf("[WiFi] Fallback AP down -- station-only\n");
  }
  gApActive = up;
}


// Restore normal WiFi after a failed/aborted OTA. During an upload we drop the AP
// (if it was up) to free RAM; afterwards we reconcile: AP stays down while the
// station is connected, and comes back as a fallback only if it is not.
//
// Modem sleep stays OFF -- setup() keeps it off permanently, so there is nothing to
// restore here. The upload path below also forces it off for the same reason: with modem
// sleep on, RX buffers pile up during a large transfer and the peer resets the connection.
static void otaRestoreWifi() {
  // AP is a fallback: bring it back only if the station is not connected.
  bool staUp = (WiFi.status() == WL_CONNECTED);
  wifiSetApActive(!staUp);
}

// POST /api/ota/upload -- the firmware binary as the RAW request body (v3.0 breaking
// change: no multipart -- curl --data-binary, or the /ota page's xhr.send(file)).
// One linear pass: quiesce, stream to flash with heap backpressure, verify, reply,
// then reboot via gOtaRebootPending (taskWeb restarts only after the 200 has flushed).
esp_err_t handleOTAUpload(httpd_req_t* r) {
  const size_t len = (size_t)r->content_len;
  printf("[OTA] Web upload start: %u bytes\n", (unsigned)len);
  if (!len) return httpxErr(r, 400, "Empty body");
  // Quiesce the gateway for the duration of the upload so the WiFi/TCP stack
  // has the contiguous heap the transfer needs.
  gOtaInProgress = true;
  // Blank the wall and stand the display + frame tasks down. The DMA engine keeps
  // clocking a black frame with no CPU help, so the panel stays quiet even while
  // Update.write() has the instruction cache disabled on both cores.
  dispBlank();
  // Free internal RAM for the transfer. A large firmware streams in faster than
  // flash can absorb it, so WiFi/lwIP receive buffers pile up; on this already
  // heap-constrained gateway that can exhaust the heap mid-upload (observed
  // min-free-heap dropping to ~512 bytes -> connection reset). Two levers help:
  // (a) drop the SoftAP so its interface buffers/housekeeping are released (only
  // safe when the station is connected, else we'd lose access), and (b) disable
  // modem sleep so the station drains the RX queue at full speed, reducing
  // buffer buildup. Both are restored if the upload fails.
  if (WiFi.status() == WL_CONNECTED) {
    wifiSetApActive(false);   // drop fallback AP if it happens to be up
    WiFi.setSleep(false);
    printf("[OTA] AP down + modem sleep off for upload (heap=%u)\n", (unsigned)ESP.getFreeHeap());
  }
  // Undo the quiesce and report a failure; every non-success exit funnels through here.
  // (A successful upload never comes back: the board reboots into the new image.)
  auto fail = [&](const char* what) {
    Update.abort();
    gOtaInProgress = false;
    otaRestoreWifi();
    dispResume();             // resume the panel refresh and repaint
    printf("[OTA] %s (%s) -- image discarded\n", what, Update.errorString());
    return httpxSend(r, 500, "text/plain",
                     "Update failed -- firmware not flashed. Device left unchanged.");
  };
  // UPDATE_SIZE_UNKNOWN lets the Update library size the partition itself.
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) return fail("Begin failed");

  size_t recvd = 0;
  while (recvd < len) {
    // Feed the web watchdog on every chunk so a large/slow upload can't trip the
    // 120 s web-stall reboot.
    wdgWebMs = millis();
    // Heap backpressure (v2.2.1). On a fast sender the upload's TCP segments can
    // queue in internal RAM faster than flash writes drain them; observed on a
    // board with 115 KB free at rest: min-heap driven to 1.2 KB mid-upload, at
    // which point lwIP fails an allocation and RESETS the connection -- the
    // upload dies with no crash and each side blames the other. Slow the
    // consumer when heap runs low: lwIP's receive window closes and TCP flow
    // control paces the sender, bounding the queue. Costs at most ~25 s on a
    // full-size image, well inside the 120 s web watchdog.
    // Graded and EARLY: by the time heap is scarce the queue has already
    // ballooned (observed: 95 KB consumed within seconds on a board with lots
    // of free heap -- more headroom just lets the queue grow further before
    // anything pushes back, then loop()'s heap floor reboots the board).
    { uint32_t h = ESP.getFreeHeap();
      if      (h < 40000) { delay(40); }
      else if (h < 60000) { delay(15); }
      else if (h < 85000) { delay(6);  }
      wdgWebMs = millis(); }
    int n = httpd_req_recv(r, (char*)httpxBuf, min(len - recvd, (size_t)sizeof(httpxBuf)));
    if (n <= 0) return fail("Upload aborted by client");
    if (Update.write(httpxBuf, (size_t)n) != (size_t)n) return fail("Write error");
    recvd += (size_t)n;
  }
  if (!Update.end(true)) return fail("Update.end failed");   // true = set the new image as bootable
  printf("[OTA] Web upload complete (%u bytes) -- verified, rebooting\n", (unsigned)recvd);
  // gOtaInProgress stays set: we reboot momentarily; no need to resume the panel.
  esp_err_t rc = httpxSend(r, 200, "text/plain", "OK");
  // The DMA engine can stop now: the image is verified and the reboot is certain.
  dispStop();
  // Do NOT restart here: ESP.restart() before the socket flushes sends the browser an
  // RST and it reports a connection error on an upload that in fact succeeded. Hand
  // the reboot to taskWeb, which waits for the 200 to reach the wire.
  gOtaRebootPending = true;
  return rc;
}
