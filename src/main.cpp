#include "gateway.h"
#include "audio.h"
#include "sound.h"
#include "sensor.h"
#include "sdcard.h"
#include "imu.h"
#include "canvas.h"   // canvasAnimLoadPlay: the boot animation
#include <esp_ota_ops.h>   // esp_ota_get_running_partition(): which slot are we actually running?

// After this many crash/watchdog reboots in a row, the boot logic reformats FATFS -- a corrupt
// filesystem is the one thing that can crash a flash write and boot-loop the board. See setup().
#define PANIC_REFORMAT_THRESHOLD 3
// Consecutive crash/watchdog reboots, kept in RTC memory: it survives a reboot but is garbage on
// a cold power-up. Written in setup(), cleared in loop() once the board has run healthy for 60s.
RTC_NOINIT_ATTR static uint32_t sfPanicBoots;
// Reset-cause history (v3.13.1): the last 8 reset reasons + uptime-at-death, kept in RTC
// memory so they survive warm reboots (a cold power-up zeroes them, which reads as
// POWERON -- correct). Exposed in /api/status as "resets" so a crash self-reports even
// with nothing attached to USB: the one that got away on 2026-07-30 taught us that.
RTC_NOINIT_ATTR uint8_t  sfResetLog[8];       // esp_reset_reason_t values, newest first
RTC_NOINIT_ATTR uint32_t sfResetUpMin[8];     // minutes of uptime when each reset hit
RTC_NOINIT_ATTR static uint32_t sfResetMagic; // 0x52534C47 = the ring is initialised
static uint32_t sfBootMs = 0;                 // set in setup(); loop() stamps uptime
static char     sfBootNote[128];              // boot line, written to the SD log after mount

// main.cpp -- boot sequence and supervisor.
// setup() brings the system up in dependency order (mutexes, config, clock,
// filesystem, virtual modules, panel, frame link, WiFi, servers, then tasks).
// loop() is the watchdog supervisor: it logs periodic telemetry and reboots if a
// task stalls or the heap runs critically low.
//
// Ordering constraints worth knowing:
//   * cfg must be loaded before dispPlan(), which decides the wall's real size.
//   * dispPlan() must precede vmInit(): the plan says how many modules exist,
//     because a cell too small to hold a glyph is not a module.
//   * sfFsInit() must precede vmInit(), which restores /vmods.dat.
//   * vmInit() must precede dispInit(), which reads vmCount.
//   * dispInit() runs before WiFi so the panel driver gets first claim on the
//     internal SRAM its default framebuffer needs (fbPsram moves it to PSRAM, v3.11).

void setup() {
  // 1. Mutexes first -- must exist before any task touches shared data
  msgMutex   = xSemaphoreCreateMutexStatic(&msgMutexBuf);
  timeMutex  = xSemaphoreCreateMutexStatic(&timeMutexBuf);
  txMutex    = xSemaphoreCreateMutexStatic(&txMutexBuf);
  txQMutex   = xSemaphoreCreateMutexStatic(&txQMutexBuf);
  vmMutex    = xSemaphoreCreateMutexStatic(&vmMutexBuf);
  psramAllocInit();   // monitor ring + TX queue -> PSRAM

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
  printf("\n[Boot] %s v%s\n", PRODUCT_NAME, FW_VERSION);
  // Which app slot is actually running, and when it was built. A serial reflash writes ota_0 and
  // resets otadata, but an OTA leaves the selector on the other slot -- so this is the first thing
  // to check when a board seems to be running firmware you did not just flash.
  { const esp_partition_t* rp = esp_ota_get_running_partition();
    printf("[Boot] running slot=%s, built %s %s\n", rp ? rp->label : "?", __DATE__, __TIME__); }
  bool fatfsRecover = false;   // set by the panic-recovery check inside the block below
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

    // Panic-recovery safeguard. sfPanicBoots lives in RTC memory: it survives a reboot -- even a
    // crash reboot -- and is only garbage on a cold power-up, which POWERON zeroes below. A
    // corrupted FATFS can crash a flash write and boot-loop the board (which is exactly what
    // happened once). After PANIC_REFORMAT_THRESHOLD crash/watchdog reboots IN A ROW -- each
    // before the board runs healthy for 60s, at which point loop() clears the count -- reformat
    // FATFS on this boot to break the loop: one self-healing reboot instead of a brick.
    const bool crashReset = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                             rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) sfPanicBoots = 0;   // cold boot: init RTC
    else if (crashReset)                                  sfPanicBoots++;
    else                                                  sfPanicBoots = 0;   // clean SW reset/OTA
    // Reset-cause ring (v3.13.1): shift in this boot's cause. Cold boot re-inits.
    if (rr == ESP_RST_POWERON || sfResetMagic != 0x52534C47) {
      memset(sfResetLog, 0, sizeof(sfResetLog));
      memset((void*)sfResetUpMin, 0, sizeof(sfResetUpMin));
      sfResetMagic = 0x52534C47;
    }
    for (int i = 7; i > 0; i--) { sfResetLog[i] = sfResetLog[i-1]; sfResetUpMin[i] = sfResetUpMin[i-1]; }
    sfResetLog[0] = (uint8_t)rr;
    sfResetUpMin[0] = 0;                       // this boot's death-uptime; stamped by loop()
    sfBootMs = millis();
    // The post-mortem line (v3.13.2): sdInit() has not run yet, so remember it and
    // write it once the card is up -- see the sdLog call after sdInit() below.
    snprintf(sfBootNote, sizeof(sfBootNote),
             "BOOT %s v%s reset=%s prev-uptime=%lum panicBoots=%lu",
             PRODUCT_NAME, FW_VERSION, rs, (unsigned long)sfResetUpMin[1],
             (unsigned long)sfPanicBoots);
    if (sfPanicBoots >= PANIC_REFORMAT_THRESHOLD) {
      printf("[RECOVERY] %u crash reboots in a row -- reformatting FATFS to break the loop\n",
             (unsigned)sfPanicBoots);
      sfPanicBoots = 0;
      fatfsRecover = true;
    }
  }

  // 2. Config
  cfgSetDefaults();
  loadConfig();

  // 3. Clock (before WiFi so timestamps work from boot; invalid until NTP)
  rtcHwInit();
  rtcRead();

  // 3b. Microphone ADC (ES7210) register bring-up -- HERE, single-threaded, because
  //     rtc.cpp's raw Wire access holds no bus lock; after this the audio module
  //     never touches I2C again (capture start/stop is I2S-only). See audio.h.
  audioInit();
  soundInit();   // ES8311 speaker DAC -- same single-threaded I2C window
  sensorInit();  // SHTC3 temp/humidity -- same single-threaded I2C window
  imuInit();     // QMI8658 tap engine -- same single-threaded I2C window (v3.15)

  // 4. Plan the panel geometry. The module grid can be clamped by the panel (a
  //    15-column wall does not fit 64 px), and the wall IS the module list, so
  //    the count must come from the plan rather than from the raw config.
  gPanel = dispPlan(cfg.panelW, cfg.panelH, cfg.gridCols, cfg.gridRows);
  if (gPanel.cols != cfg.gridCols || gPanel.rows != cfg.gridRows)
    printf("[PANEL] wall %ux%u does not fit a %ux%u panel -- using %ux%u\n",
           cfg.gridCols, cfg.gridRows, cfg.panelW, cfg.panelH, gPanel.cols, gPanel.rows);

  // 5. Filesystem, then the thing that restores from it: the virtual modules' own
  //    state (/vmods.dat). Nothing else on this board is sticky.
  sfFsInit(fatfsRecover);
  sdInit();           // microSD (v3.10): mount the TF card if one is fitted (setup() only)
  sdLog("%s", sfBootNote);   // the post-mortem line: why THIS boot happened (v3.13.2)
  gTransType = cfg.transType; gTransMs = cfg.transMs;   // restore persisted transition (v3.7.2)
  vmBuildReel();      // the shared reel: every CP1252 glyph, then the colours
  vmInit((int)gPanel.cols * (int)gPanel.rows);

  // 6. Panel. Before WiFi: the default (internal-SRAM) framebuffer must be claimed
  //    before the WiFi stack, the other big claimant on that pool. (fbPsram moves the
  //    framebuffer to octal PSRAM instead -- v3.11.)
  dispInit();
  dispMarkDirty();

  // 7. Boot animation (v2.1): if a library animation is configured, play it now --
  //    before WiFi -- so the wall is alive seconds after power-on. The first
  //    split-flap command (or canvas/effect start) supersedes it, as always.
  if (cfg.bootAnim[0] && gPanel.ready) {
    int rc = canvasAnimLoadPlay(cfg.bootAnim);
    if (rc) printf("[ANIM] boot animation '%s' failed (%d)\n", cfg.bootAnim, rc);
    else    printf("[ANIM] boot animation '%s' playing\n", cfg.bootAnim);
  }

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
  // limped while every outbound TCP connect failed.
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
  webInit();

  // 10. Tasks. Display sits on core 1 with the network stack, leaving core 0 for
  //    frames, the web server and the clock -- the same split the physical
  //    gateway uses, with the panel taking the slot the OTA task shares.
  // 3072 (was 2048, v3.13.2): the brightness schedule added sscanf+gmtime_r to this
  // task's tick and the soak flagged a 496-byte minimum -- the thinnest margin of any
  // task, and a plausible cause of the unexplained 2026-07-30 reboot. 1 KB well spent.
  xTaskCreatePinnedToCore(taskRTC,     "RTC",     3072, NULL, 2, &hTaskRTC,   0);
  xTaskCreatePinnedToCore(taskFrames,  "Frames",  5120, NULL, 3, &hTaskFrames, 0);
  // v3.0: HTTP requests are served by esp_http_server's own task (httpx.h); taskWeb
  // is just the SSE pump + supervisor now, so its stack shrank from the handler-era 8 KB.
  xTaskCreatePinnedToCore(taskWeb,     "Web",     4096, NULL, 2, &hTaskWeb,   0);
  xTaskCreatePinnedToCore(taskNetwork, "Network", 6144, NULL, 1, &hTaskNet,   1);
  xTaskCreatePinnedToCore(taskDisplay, "Display", 4096, NULL, 2, &hTaskDisp,  1);

  printf("[Boot] Ready\n");
}

void loop() {
  // Stamp this boot's running time into the reset ring (v3.13.1): after a crash, the
  // NEXT boot reports how long this one lived. Cheap: one RTC-mem write per pass.
  sfResetUpMin[0] = (uint32_t)((millis() - sfBootMs) / 60000UL);
  // Heap heartbeat to the SD log every 10 min (v3.13.2): a crashed board's log then
  // shows its health right up to the end, without anything on USB.
  { static uint32_t lastHb = 0;
    if (millis() - lastHb > 600000UL) {
      lastHb = millis();
      sdLog("hb up=%lum heap=%u minheap=%u", (unsigned long)(millis() / 60000UL),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    } }
  static unsigned long lastWdgCheck = 0;
  unsigned long now = millis();
  // Panic-recovery: once we have run healthy for a minute, this boot plainly was not a crash
  // loop, so clear the RTC crash counter -- only RAPID consecutive crashes (each before 60s)
  // accumulate toward the FATFS reformat in setup().
  static bool panicCounterCleared = false;
  if (!panicCounterCleared && now > 60000UL) { sfPanicBoots = 0; panicCounterCleared = true; }
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
    // monitor ring and the TX queue live there. The virtual modules do
    // NOT -- they are pinned to internal RAM, see vmInit().
    // Heap + min-ever heap + largest free block
    // (fragmentation: a big gap between freeHeap and maxAlloc is a common
    // pre-crash signature). Per-task stack high-water marks catch the
    // canary-overflow class before it fires. rx/tx/drop counters surface
    // frame traffic.
    unsigned freeHeap = (unsigned)ESP.getFreeHeap();
    unsigned minHeap  = (unsigned)ESP.getMinFreeHeap();
    unsigned maxBlk   = ESP.getMaxAllocHeap();
    unsigned sFrm = hTaskFrames   ? uxTaskGetStackHighWaterMark(hTaskFrames)   : 0;
    unsigned sWeb = hTaskWeb   ? uxTaskGetStackHighWaterMark(hTaskWeb)   : 0;
    unsigned sNet = hTaskNet   ? uxTaskGetStackHighWaterMark(hTaskNet)   : 0;
    TaskHandle_t hHttpd = xTaskGetHandle("httpd");   // esp_http_server's worker
    unsigned sHtp = hHttpd     ? uxTaskGetStackHighWaterMark(hHttpd)     : 0;
    unsigned sRtc = hTaskRTC   ? uxTaskGetStackHighWaterMark(hTaskRTC)   : 0;
    unsigned sDsp = hTaskDisp  ? uxTaskGetStackHighWaterMark(hTaskDisp)  : 0;
    static unsigned minBlkEver = 0xFFFFFFFFu;
    if (maxBlk < minBlkEver) minBlkEver = maxBlk;
    printf("[WDG] up=%lus heap=%u min=%u maxblk=%u minblk=%u "
           "stk(frames/web/net/httpd/rtc/disp)=%u/%u/%u/%u/%u/%u "
           "tx=%lu psram=%u panel=%d "
           "wifi=%d ap=%d rssi=%d mods=%d\n",
           now/1000, freeHeap, minHeap, maxBlk, minBlkEver,
           sFrm, sWeb, sNet, sHtp, sRtc, sDsp,
           txCount,
           (unsigned)ESP.getFreePsram(), (int)gPanel.ready,
           (int)(WiFi.status()==WL_CONNECTED),
           (int)gApActive,
           (WiFi.status()==WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
           vmCount);

    // Boot grace period: skip stall detection for the first 60 s. The first boot
    // after flashing formats the FATFS partition (a long blocking flash
    // operation), and WiFi bring-up can briefly skew task scheduling.
    if (now >= 60000UL) {
      // A heartbeat in the future (wdg > now) can only come from transient boot
      // skew -- treat it as healthy rather than letting the unsigned subtraction
      // underflow into a 49-day "stall".
      bool okFrm  = (wdgFramesMs == 0 || wdgFramesMs > now || now - wdgFramesMs < 30000UL);
      bool okWeb  = (wdgWebMs   == 0 || wdgWebMs   > now || now - wdgWebMs  < 120000UL);
      bool okNet  = (wdgNetMs   == 0 || wdgNetMs   > now || now - wdgNetMs  < 30000UL);
      bool okDisp = (wdgDispMs  == 0 || wdgDispMs  > now || now - wdgDispMs < 30000UL);
      if (!okFrm || !okWeb || !okNet || !okDisp) {
        printf("[WDG] STALL: Frames=%d Web=%d Net=%d Disp=%d (heap=%u) -- rebooting\n",
               okFrm, okWeb, okNet, okDisp, (unsigned)ESP.getFreeHeap());
        sdLog("WDG STALL Frames=%d Web=%d Net=%d Disp=%d heap=%u -- rebooting",
              okFrm, okWeb, okNet, okDisp, (unsigned)ESP.getFreeHeap());
        delay(200);
        ESP.restart();
      }
    }
    // Emergency reboot if heap falls critically low -- EXCEPT during a web OTA.
    // A fast sender fills the TCP receive window (~95 KB on this build's lwIP
    // config) faster than flash writes drain it, which transiently dives below
    // this floor on a strong link; the OTA handler's own throttle keeps it
    // survivable, the transient ends with the upload, and rebooting mid-flash
    // is the one genuinely destructive response. Every observed "mystery OTA
    // reboot" on the bench was this check firing. Outside an upload the floor
    // keeps its original meaning: a leak has won, reboot before malloc chaos.
    if ((unsigned)ESP.getFreeHeap() < 20000 && !gOtaInProgress) {
      printf("[WDG] CRITICAL: heap=%u -- rebooting\n", (unsigned)ESP.getFreeHeap());
      sdLog("WDG heap floor: heap=%u -- rebooting", (unsigned)ESP.getFreeHeap());
      delay(200);
      ESP.restart();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}
