#include "gateway.h"

// main.cpp -- boot sequence and supervisor.
// setup() brings the system up in dependency order (mutexes, config, clock,
// filesystem, virtual modules, panel, emulated bus, WiFi, servers, then tasks).
// loop() is the watchdog supervisor: it logs periodic telemetry and reboots if a
// task stalls or the heap runs critically low.
//
// Ordering constraints worth knowing:
//   * cfg must be loaded before dispPlan(), which decides the wall's real size.
//   * dispPlan() must precede vmInit(): the plan says how many modules exist,
//     because a cell too small to hold a glyph is not a module.
//   * sfFsInit() must precede vmInit(), which restores /vmods.dat.
//   * vmInit() must precede dispInit() and vbusBegin(), both of which read vmCount.
//   * dispInit() runs before WiFi so the panel driver gets first claim on the
//     internal SRAM its DMA framebuffer needs (quad PSRAM is far too slow).

void setup() {
  // 1. Mutexes first -- must exist before any task touches shared data
  msgMutex   = xSemaphoreCreateMutexStatic(&msgMutexBuf);
  sfMutex    = xSemaphoreCreateMutexStatic(&sfMutexBuf);
  timeMutex  = xSemaphoreCreateMutexStatic(&timeMutexBuf);
  mqttQMutex = xSemaphoreCreateMutexStatic(&mqttQMutexBuf);
  txMutex    = xSemaphoreCreateMutexStatic(&txMutexBuf);
  txQMutex   = xSemaphoreCreateMutexStatic(&txQMutexBuf);
  vmMutex    = xSemaphoreCreateMutexStatic(&vmMutexBuf);
  psramAllocInit();   // ring + MQTT queue + module registry -> PSRAM

  // Debug output over native USB CDC (the board boots with CDC on).
  Serial.begin(115200);
  // This board has no USB-UART bridge: Serial IS the native USB CDC (HWCDC), so
  // CDC-on-boot cannot be turned off without losing the serial monitor entirely.
  // The hazard is that HWCDC::write() blocks on its TX ring for tx_timeout_ms
  // (100 ms by default) whenever a host has enumerated the port but is not
  // draining it -- a monitor left paused, or a USB power brick. DBG() is called
  // from inside web handlers, so that blocks taskWeb. 0 = drop instead of block.
  Serial.setTxTimeoutMs(0);
  { unsigned long t = millis(); while (!Serial && millis() - t < 3000) delay(10); }
  delay(200);
  printf("\n[Boot] %s v%s (gateway API %s)\n", PRODUCT_NAME, FW_VERSION, API_VERSION);
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rs = "OTHER";
    switch (rr) {
      case ESP_RST_POWERON:  rs = "POWERON";  break;
      case ESP_RST_SW:       rs = "SW";       break;
      case ESP_RST_PANIC:    rs = "PANIC";    break;
      case ESP_RST_INT_WDT:  rs = "INT_WDT";  break;
      case ESP_RST_TASK_WDT: rs = "TASK_WDT"; break;
      case ESP_RST_WDT:      rs = "WDT";      break;
      case ESP_RST_BROWNOUT: rs = "BROWNOUT"; break;
      case ESP_RST_DEEPSLEEP:rs = "DEEPSLEEP";break;
      default: break;
    }
    printf("[Boot] reset=%s heap=%u psram=%u flash=%uKB sdk=%s\n",
           rs, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(),
           (unsigned)ESP.getFlashChipSize()/1024, ESP.getSdkVersion());
  }

  // 2. Config, then the gateway's module registry
  cfgSetDefaults();
  loadConfig();
  memset(sfModules, 0, sizeof(SFModule) * MAX_MODULES);
  for (int i = 0; i < MAX_MODULES; i++) sfModules[i].id = 255;
  sfModuleCount = 0;

  // 3. Clock (before WiFi so timestamps work from boot; invalid until NTP)
  rtcHwInit();
  rtcRead();

  // 4. Plan the panel geometry. The module grid can be clamped by the panel (a
  //    15-column wall does not fit 64 px), and the wall IS the module list, so
  //    the count must come from the plan rather than from the raw config.
  gPanel = dispPlan(cfg.panelW, cfg.panelH, cfg.gridCols, cfg.gridRows);
  if (gPanel.cols != cfg.gridCols || gPanel.rows != cfg.gridRows)
    printf("[PANEL] wall %ux%u does not fit a %ux%u panel -- using %ux%u\n",
           cfg.gridCols, cfg.gridRows, cfg.panelW, cfg.panelH, gPanel.cols, gPanel.rows);

  // 5. Filesystem, then the two things that restore from it: the virtual
  //    modules' own state and the gateway's sticky registry.
  sfFsInit();
  vmBuildReel();      // the shared reel: every CP1252 glyph, then the colours
  vmInit((int)gPanel.cols * (int)gPanel.rows);
  // The reels just came up HOMED (autoHome), so seed the wall mirror to match. Without this
  // the mirror reads "?" -- "present, character unknown" -- for every cell, which is a lie we
  // would be telling ourselves: we homed them, so we know exactly what they show. The mirror
  // is what /api/display/state and the Live Preview read.
  for (int i = 0; i < vmCount; i++)
    if (vmods[i].id < (int)sizeof(gWallChars))
      gWallChars[vmods[i].id] = vmFlapCharAt(vmods[i].curIndex);
  sfModulesLoad();

  // 6. Panel. Before WiFi: the DMA framebuffer must come out of internal SRAM
  //    (this board's PSRAM is quad SPI and too slow to feed it), and the WiFi
  //    stack is the other big claimant on that pool.
  dispInit();
  dispMarkDirty();

  // 7. The emulated bus
  rs485Begin();
  vbusBegin();

  // 8. WiFi -- MUST be initialised here, on the main Arduino task.
  // The SoftAP is a FALLBACK only: start in station mode and connect to the
  // configured network. With no network configured, bring the AP up immediately
  // so the gateway is reachable for first-time setup.
  // Must precede WiFi.mode()/begin(): the DHCP client latches the name at association.
  WiFi.setHostname(cfgHostname());
  printf("[WiFi] hostname %s (http://%s.local)\n", cfgHostname(), cfgHostname());
  WiFi.mode(WIFI_STA);
  // Modem sleep OFF. Upstream leaves it on and gets away with it, but on this board
  // it correlates with packets going missing at an RSSI of -45: outbound SYNs never
  // answered (the SYN-ACK arrives while the radio is parked and the AP does not
  // re-deliver it), and established sockets reset by the peer. Inbound traffic hides
  // it, because serving a request keeps the radio awake -- which is why the web UI
  // limped while every outbound MQTT connect failed.
  //
  // The saving it buys is irrelevant here: this board is mains-powered and is
  // driving a HUB75 panel that dwarfs the radio's draw. Reliability wins. If a
  // future board is battery-powered, make this a config flag rather than flipping
  // it back blindly.
  WiFi.setSleep(false);
  if (strlen(cfg.wifiSSID)) {
    WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
    staDownSince = millis();
    printf("[WiFi] STA connecting to %s...\n", cfg.wifiSSID);
  } else {
    wifiSetApActive(true);
    printf("[WiFi] No network configured -- fallback AP only\n");
  }

  // 9. Servers
  otaInit();
  mqttInit();
  webInit();

  // 10. Tasks. Display sits on core 1 with the network stack, leaving core 0 for
  //    the bus, the web server and the clock -- the same split the RS-485
  //    gateway uses, with the panel taking the slot the OTA task shares.
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     2048, NULL, 2, &hTaskRTC,   0);
  xTaskCreatePinnedToCore(taskRS485,   "Bus",     6144, NULL, 3, &hTaskRS485, 0);
  xTaskCreatePinnedToCore(taskOTA,     "OTA",     4096, NULL, 1, &hTaskOTA,   1);
  xTaskCreatePinnedToCore(taskWeb,     "Web",     8192, NULL, 2, &hTaskWeb,   0);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 8192, NULL, 1, &hTaskNet,   1);
  xTaskCreatePinnedToCore(taskDisplay, "Display", 4096, NULL, 2, &hTaskDisp,  1);

  printf("[Boot] Ready\n");
}

void loop() {
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  if (now - lastWdgCheck >= 30000UL) {
    lastWdgCheck = now;
    // Rich periodic telemetry. No 'frag' percentage: 100 - maxblk/heap is a ratio of two
    // independently noisy numbers, and it IMPROVES as the free heap shrinks -- at heap
    // 82908/maxblk 31732 it read 62%, and at heap 62276 with the SAME maxblk it read 50%.
    // The two numbers that actually predict an allocation failure are the watermarks:
    // min (lowest free heap ever) and minblk (smallest largest-block ever).
    //
    // heap/min/maxblk are INTERNAL RAM only -- that is what
    // (unsigned)ESP.getFreeHeap() reports -- and the panel's DMA framebuffer is the big claimant
    // on that pool, so a modest maxblk is expected. psram is counted separately; the
    // registry, monitor ring, MQTT queue and virtual modules all live there.
    // Heap + min-ever heap + largest free block
    // (fragmentation: a big gap between freeHeap and maxAlloc is a common
    // pre-crash signature). Per-task stack high-water marks catch the
    // canary-overflow class before it fires. rx/tx/reject/drop counters surface
    // bus health.
    unsigned freeHeap = (unsigned)ESP.getFreeHeap();
    unsigned minHeap  = (unsigned)ESP.getMinFreeHeap();
    unsigned maxBlk   = ESP.getMaxAllocHeap();
    unsigned sBus = hTaskRS485 ? uxTaskGetStackHighWaterMark(hTaskRS485) : 0;
    unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
    unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
    unsigned sOta = hTaskOTA   ? uxTaskGetStackHighWaterMark(hTaskOTA)   : 0;
    unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
    unsigned sDsp = hTaskDisp  ? uxTaskGetStackHighWaterMark(hTaskDisp)  : 0;
    static unsigned minBlkEver = 0xFFFFFFFFu;
    if (maxBlk < minBlkEver) minBlkEver = maxBlk;
    printf("[WDG] up=%lus heap=%u min=%u maxblk=%u minblk=%u "
           "stk(bus/web/net/ota/rtc/disp)=%u/%u/%u/%u/%u/%u "
           "rx=%lu tx=%lu drop=%lu psram=%u panel=%d "
           "wifi=%d ap=%d rssi=%d mqtt=%d mods=%d/%d\n",
           now/1000, freeHeap, minHeap, maxBlk, minBlkEver,
           sBus, sWeb, sNet, sOta, sRtc, sDsp,
           rxCount, txCount, vbusDropped,
           (unsigned)ESP.getFreePsram(), (int)gPanel.ready,
           (int)(WiFi.status()==WL_CONNECTED),
           (int)gApActive,
           (WiFi.status()==WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
           (int)mqtt.connected(), sfModuleCount, vmCount);

    // Boot grace period: skip stall detection for the first 60 s. The first boot
    // after flashing formats the FATFS partition (a long blocking flash
    // operation), and WiFi/MQTT bring-up can briefly skew task scheduling.
    if (now >= 60000UL) {
      // A heartbeat in the future (wdg > now) can only come from transient boot
      // skew -- treat it as healthy rather than letting the unsigned subtraction
      // underflow into a 49-day "stall".
      bool okBus  = (wdgRS485Ms == 0 || wdgRS485Ms > now || now - wdgRS485Ms < 30000UL);
      bool okWeb  = (wdgWebMs   == 0 || wdgWebMs   > now || now - wdgWebMs  < 120000UL);
      bool okNet  = (wdgNetMs   == 0 || wdgNetMs   > now || now - wdgNetMs  < 30000UL);
      bool okDisp = (wdgDispMs  == 0 || wdgDispMs  > now || now - wdgDispMs < 30000UL);
      if (!okBus || !okWeb || !okNet || !okDisp) {
        printf("[WDG] STALL: Bus=%d Web=%d Net=%d Disp=%d (heap=%u) -- rebooting\n",
               okBus, okWeb, okNet, okDisp, (unsigned)ESP.getFreeHeap());
        delay(200);
        ESP.restart();
      }
    }
    // Emergency reboot if heap falls critically low.
    if ((unsigned)ESP.getFreeHeap() < 20000) {
      printf("[WDG] CRITICAL: heap=%u -- rebooting\n", (unsigned)ESP.getFreeHeap());
      delay(200);
      ESP.restart();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}
