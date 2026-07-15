#include "gateway.h"



// tasks.cpp -- the long-lived FreeRTOS task loops.
// taskRS485 (core 0): drains the emulated bus, frames messages, parses replies.
// taskRTC  (core 0): refreshes the wall clock once a second.
// taskWeb  (core 0): services the HTTP server.
// taskNetwork (core 1): WiFi/MQTT reconnect, status publishing, virtual-module and
// config persistence.
// taskDisplay (core 1, in display.cpp): steps the reels and repaints the panel.
// Each feeds its watchdog timestamp; loop() in main.cpp reboots if one stalls.
/* ----------------------------------------------------------
   FreeRTOS tasks
---------------------------------------------------------- */

// Bus receive + response parsing (Core 0)
//
// The split-flap protocol uses newline-terminated ASCII messages that always
// start with 'm'. Replies arrive from vbusPoll() as whole frames, but they are
// still fed through the same byte accumulator the UART fed: it is the accumulator
// that guarantees every ring-buffer entry is exactly one complete protocol
// message, and running the emulated bus down the identical path is what keeps the
// gateway above it honest.

// Frame accumulator. Written only by taskRS485, hence file-static rather than a
// parameter. A frame from a virtual module is at most an 'A' reply (~117 bytes
// with the 64-flap reel); MSG_MAX_BYTES is sized so it fits in one
// monitor-ring entry untruncated.
static uint8_t busLineBuf[TX_MAX_BYTES];
static size_t  busLineLen = 0;

// Feed one received byte to the accumulator; on a completed frame, log it, mirror
// it to MQTT and hand it to the response parser.
static void busIngestByte(uint8_t c) {
  // Touch the watchdog per byte: a sustained burst of bus traffic (each completed
  // frame triggers rtcFormatTime + MQTT + parse) could otherwise keep us in this
  // loop past the 30 s bus-watchdog threshold and trigger a false stall reboot.
  wdgRS485Ms = millis();
  gLastRxMs  = wdgRS485Ms;   // bus activity marker for the TX quiet guard

  // If we see an 'm' and the buffer already holds something that doesn't start
  // with 'm', discard the stale partial frame and start fresh.
  if (c == 'm' && busLineLen > 0 && busLineBuf[0] != 'm') busLineLen = 0;

  // Start accumulating only once we've seen the leading 'm'.
  if (busLineLen == 0 && c != 'm') return;

  if (busLineLen < TX_MAX_BYTES - 1) {
    busLineBuf[busLineLen++] = c;
  } else {
    busLineLen = 0;   // full without a terminator -- oversized frame, discard
    return;
  }
  if (c != '\n') return;

  // Newline = end of message. Commit to the ring buffer.
  rxCount = rxCount + 1;
  RS485Msg m;
  m.timestamp = millis();
  m.dir       = 'R';
  m.origin    = 0;       // origin only labels 'C' (command) rows, not bus TX/RX
  m.sanitized = false;   // RX frames are never sanitized
  // The ring entry is fixed-size; store at most MSG_MAX_BYTES. The full frame is
  // still parsed below -- this copy is display-only.
  size_t ringLen = (busLineLen > MSG_MAX_BYTES) ? MSG_MAX_BYTES : busLineLen;
  m.len = ringLen;
  memcpy(m.data, busLineBuf, ringLen);
  rtcFormatTime(m.wallTime, sizeof(m.wallTime));
  m.epoch = rtcEpochNow();   // UTC epoch for browser-local display
  // No [RX] serial line either: these replies were synthesized by the virtual modules a
  // microsecond ago, in this same process. Echoing them as 'received' was theatre.
  // Mirrored to MQTT (<prefix>/rx) but NOT to the web Monitor -- these replies are
  // synthesized by the virtual modules a microsecond ago; showing them as "received
  // from the bus" was theatre.
  mqttPublishMsg(m);
  // Nothing parses the reply any more: it used to feed the module registry, and the
  // registry is gone. The frame is still mirrored to MQTT above, which is the only
  // consumer it ever had besides that.
  busLineLen = 0;
}

void taskRS485(void* pv) {
  static uint8_t rxFrame[TX_MAX_BYTES];   // one polled reply; this task is its only reader

  // No startup discovery. There is nothing to discover: the wall IS the modules, all of them
  // exist from vmInit(), and none can appear or vanish. The m*v broadcast, the registry
  // reconciliation it fed, and the "45 known modules of 75" bug that came with it are all
  // gone with the registry.

  while (true) {
    // Stand down while a firmware image is streaming: no bus traffic, no flash
    // contention. Feed the watchdog so the stall check stays happy.
    if (gOtaInProgress) { wdgRS485Ms = millis(); vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // Send any scheduled batch frames now due. This is where /api/rs485/batch's
    // cascade pacing actually happens -- moved off taskWeb so the HTTP server never
    // blocks on delay() (see rs485.h). At most one 5 ms tick of latency per frame.
    rs485PollScheduled(millis());

    // Drain what the modules have queued, bounded per iteration so a long
    // broadcast reply train cannot monopolise the task -- the rest arrives on the
    // next 5 ms tick.
    for (int frames = 0; frames < 8; frames++) {
      size_t rxLen = 0;
      if (!vbusPoll(millis(), rxFrame, sizeof(rxFrame), &rxLen)) break;
      for (size_t i = 0; i < rxLen; i++) busIngestByte(rxFrame[i]);
    }

    wdgRS485Ms = millis();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// v3.0: evaluate the Quiet-Time schedule and toggle Quiet Time when local time
// crosses a window boundary. Transition-based (only acts on a change), so a
// manual Quiet toggle within a window is respected until the next boundary.
// True if the schedule is enabled and the current user-local time is in-window.
// The schedule (start/end/days) is entered in the browser's local time, and the
// browser sends its UTC offset with it, so we apply that offset and read the
// result as UTC fields -- independent of the gateway posixTZ (which defaults to
// UTC), and consistent with how the web UI renders every other timestamp.
bool quietSchedInWindow() {
  if (!cfg.quietSchedEnabled) return false;
  time_t utc = (time_t)rtcEpochNow();
  if (!utc) return false;            // RTC/NTP not valid yet
  time_t local = utc + (time_t)cfg.quietTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&local, &lt);             // offset already applied -> read fields directly
  int cur = lt.tm_hour * 60 + lt.tm_min;
  int sh = 22, sm = 0, eh = 7, em = 0;
  sscanf(cfg.quietStart, "%d:%d", &sh, &sm);
  sscanf(cfg.quietEnd,   "%d:%d", &eh, &em);
  int s = sh * 60 + sm, e = eh * 60 + em;
  bool dayOn = (cfg.quietDays >> lt.tm_wday) & 1;
  bool inWin = (s <= e) ? (cur >= s && cur < e) : (cur >= s || cur < e);  // overnight ok
  return dayOn && inWin;
}

static void quietScheduleTick() {
  static int prevWant = -1;
  if (gOtaInProgress) return;        // never touch quiet/resync mid-flash (OTA safety)

  // Does the schedule currently want Quiet ON? A DISABLED schedule counts as
  // want=0 (not a hard early-return) so that disabling an active schedule flows
  // through the same falling-edge turn-off below and RELEASES Quiet Time, rather
  // than leaving it stuck on.
  int want;
  if (!cfg.quietSchedEnabled) {
    want = 0;
  } else {
    if (!rtcEpochNow()) return;      // enabled but no valid time yet -- don't change state
    want = quietSchedInWindow() ? 1 : 0;
  }

  // Self-healing, not edge-only: inside the window keep Quiet ON, re-asserting if
  // anything turned it off (external OFF is also refused mid-window; see the
  // MQTT/REST guards). On the falling edge -- window end OR the schedule being
  // disabled -- turn OFF, but only if the schedule was the one holding it
  // (prevWant==1), so a manual Quiet Time outside the schedule is left alone.
  if (want) {
    if (!gQuietTime) { DBG("[QUIET] schedule: in window, asserting ON\n"); sfSetQuietTime(true); }
  } else if (prevWant == 1 && gQuietTime) {
    DBG("[QUIET] schedule: released (window ended or disabled), turning OFF\n");
    sfSetQuietTime(false);
  }
  prevWant = want;
}

void taskRTC(void* pv) {
  uint32_t lastSched = 0;
  while (true) {
    rtcRead();
    if (lastSched == 0 || millis() - lastSched > 5000UL) {
      lastSched = millis();
      quietScheduleTick();     // evaluate the quiet window every 5s (prompt flip)
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void taskWeb(void* pv) {
  unsigned long clientSince = 0;   // millis() when current client first seen
  while (true) {
    wdgWebMs = millis();      // touch BEFORE handling (covers in-handler stalls)
    server.handleClient();

    // Proactively close any client that lingers connected for too long.
    // The ESP32 WebServer keeps a half-open connection in HC_WAIT_READ for
    // up to HTTP_MAX_DATA_WAIT; a browser (notably Chrome/Safari) that opens
    // a speculative socket and never completes the request can otherwise
    // wedge handleClient() and stall the web task -> "Web=0" watchdog reboot.
    WiFiClient c = server.client();
    if (c && c.connected()) {
      if (clientSince == 0) clientSince = millis();
      else if (millis() - clientSince > 8000UL) {   // 8s hard cap per connection
        c.stop();                                    // force-close the stale socket
        clientSince = 0;
      }
    } else {
      clientSince = 0;   // no client connected -- reset the timer
    }

    wdgWebMs = millis();      // touch AFTER handling

    // Self-heal a silently-dead port-80 listener (see webEnsureListening): a ground-truth
    // listening() check every 20s, acted on only when the listener is genuinely down.
    static unsigned long lastListenCheck = 0;
    if (millis() - lastListenCheck >= 20000UL) {
      lastListenCheck = millis();
      webEnsureListening();
    }

    // handleClient() has returned, so the 200 is on the wire and the socket is closed.
    // Now it is safe to boot the image we just flashed.
    if (gOtaRebootPending) {
      printf("[OTA] response delivered -- rebooting into the new image\n");
      vTaskDelay(pdMS_TO_TICKS(250));
      ESP.restart();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool          staWasUp    = false;
unsigned long wifiRetryMs = 0;

void taskNetwork(void* pv) {
  // WiFi init done in setup() - this task only polls and reconnects.
  // Seed the retry clock from NOW, not from 0: setup() has just called WiFi.begin()
  // and the association is in flight. An unseeded wifiRetryMs makes the first
  // re-association fire at an absolute millis() of 15000 -- i.e. it interrupts the
  // very association we are waiting on. Give the stack a full interval to succeed.
  wifiRetryMs = millis();
  while (true) {
    bool staUp = (WiFi.status() == WL_CONNECTED);
    if (staUp && !staWasUp) {
      staWasUp = true;
      wifiSetApActive(false);   // station is up -> drop the fallback AP
      if (!ntpSynced) ntpSynced = rtcNTPSync();
      { IPAddress _a = WiFi.localIP();
  printf("[WiFi] Connected IP=%d.%d.%d.%d\n", _a[0],_a[1],_a[2],_a[3]); }
    } else if (!staUp && staWasUp) {
      staWasUp = false;
      staDownSince = millis();
      printf("[WiFi] Disconnected\n");
    }
    // Fallback AP: if the station has been down for a grace period (and a
    // network is configured), bring the AP up so the gateway stays reachable.
    // If no network is configured the AP was already raised at boot. Skipped
    // during OTA (the AP is intentionally down to free RAM for the upload).
    if (!staUp && !gApActive && !gOtaInProgress && strlen(cfg.wifiSSID) &&
        staDownSince && millis() - staDownSince > 20000UL) {
      printf("[WiFi] Station down 20s -- raising fallback AP\n");
      wifiSetApActive(true);
    }
    // Manual re-association, as a backstop only. The esp_wifi stack already retries
    // on its own, and WiFi.disconnect() here ABORTS whatever it is doing -- so this
    // must be slow enough that a merely-slow association is never interrupted. It was
    // 15 s from an unseeded (0) wifiRetryMs, which meant the very first attempt fired
    // at millis()==15000 on every boot; an AP that took >10 s to associate got torn
    // down mid-handshake ("Reason: 8 - ASSOC_LEAVE"), re-begun, and could livelock
    // there. 30 s, seeded at task start, leaves the stack alone unless it is truly stuck.
    if (!staUp && strlen(cfg.wifiSSID) && millis() - wifiRetryMs > 30000UL) {
      printf("[WiFi] no association after 30s -- re-associating\n");
      wifiRetryMs = millis();
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    }
    if (staUp && strlen(cfg.mqttHost) && !gOtaInProgress) {
      if (!mqtt.connected() && millis() - mqttRetryMs > mqttRetryDelayMs) {
        mqttRetryMs = millis();
        mqttConnect();
        if (!mqtt.connected()) {
          if (mqttFailCount < 255) mqttFailCount++;
          // Back off: 30s, 60, 120, 240, then every 300s. A broker that is
          // unreachable stays unreachable, and retrying it every 30 s forever
          // buys nothing while each attempt blocks this task for the 5 s connect
          // timeout and churns an lwIP socket.
          //
          // Deliberately NOT forcing a WiFi reconnect here. The old code did that
          // after 5 straight failures, on the theory that MQTT-won't-connect means
          // the TCP stack is wedged. It does not: MQTT also fails when the BROKER
          // is unreachable -- and then tearing down a working station drops every
          // in-flight HTTP response, so the web UI dies every ~150 s and the user
          // cannot even reach the page to switch MQTT off. The cure was worse than
          // the disease, and it hid the real fault. If WiFi is genuinely down,
          // staUp is false and the reconnect above this block handles it.
          unsigned long d = 30000UL << (mqttFailCount > 4 ? 4 : mqttFailCount - 1);
          mqttRetryDelayMs = d > 300000UL ? 300000UL : d;
        } else {
          mqttFailCount    = 0;
          mqttRetryDelayMs = 30000UL;
        }
      }
      if (mqtt.connected()) {
        mqtt.loop();
        // Drain the outbound queue -- all mqtt.publish calls happen here
        if (mqttQMutex && mqttQueue && xSemaphoreTake(mqttQMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          while (mqttQTail != mqttQHead) {
            MqttQItem& item = mqttQueue[mqttQTail];
            mqtt.publish(item.topic, (uint8_t*)item.payload, item.len, false);
            mqttQTail = (mqttQTail + 1) % MQTT_Q_SIZE;
          }
          xSemaphoreGive(mqttQMutex);
        }
      }
    }
    if (!gOtaInProgress && millis() - lastStatusMs > STATUS_INTERVAL_MS) {
      lastStatusMs = millis();
      mqttPublishStatus();
    }
    // Refresh the HA display sensor when the wall changed, rate-limited to avoid
    // spamming HA's recorder. No-op unless HA integration is enabled. Skipped
    // during an OTA upload to keep heap/CPU free for the transfer.
    if (!gOtaInProgress && gDisplayDirty && cfg.haEnabled && millis() - lastDispPubMs > 1500) {
      lastDispPubMs = millis();
      // Clear the dirty flag only if it actually published. A snapshot skipped because the reel
      // lock was momentarily busy must be retried on the next tick, not dropped -- otherwise HA
      // keeps the prior state until the wall next changes.
      if (mqttPublishDisplayState()) gDisplayDirty = false;
    }

    // Persist the companion URL only once it has stopped moving. See the note on
    // gCompanionUrlDirty in common.h: an unconditional save turned two co-resident
    // companions into an NVS write every heartbeat, forever.
    if (gCompanionUrlDirty && millis() - gCompanionUrlDirtyMs > COMPANION_SAVE_DEBOUNCE_MS) {
      gCompanionUrlDirty = false;
      saveConfig();
      DBG("[CFG] companion URL settled -- persisted: %s\n", cfg.companionUrl);
    }

    // The virtual modules' own state, debounced to limit flash wear. 'N', 'i' and the
    // restore command dirty it; showing a character does not.
    if (vmDirty) {
      if (vmDirtyMs == 0) vmDirtyMs = millis();
      if (millis() - vmDirtyMs > VMODULE_SAVE_DEBOUNCE_MS) {
        vmSave();
        vmDirty   = false;
        vmDirtyMs = 0;
      }
    }
    wdgNetMs = millis();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
