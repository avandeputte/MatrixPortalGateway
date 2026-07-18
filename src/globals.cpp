#include "gateway.h"


// globals.cpp -- single definition site for every shared global.
// Each variable here is declared 'extern' in common.h (or a subsystem header)
// and defined exactly once below, so there is one authoritative home for all
// cross-file state (config, queues, mutexes, task handles, ...).

// Debug flag read by the DBG() macro. Mirrors cfg.serialDebug, kept in sync in
// loadConfig() and handleApiConfigSettings().
volatile bool gSerialDebug = false;
// Quiet Time: when true the gateway still accepts and acknowledges every command,
// but does NOT transmit normal display-motion frames to the bus (show character,
// show index, and home), so the flaps stay still during quiet hours. What each
// module was asked to show meanwhile is remembered -- as a FLAP INDEX, in
// VModule::pendFlap -- so the wall can resync exactly when Quiet Time turns off.
//
// The wall is BLANKED as Quiet Time is entered -- sfSetQuietTime() homes every reel
// on the rising edge, and it must do so BEFORE raising this flag, because the
// suppression above would otherwise swallow the very home frame that blanks the
// wall. What each module was showing is snapshotted first, so the falling-edge
// resync puts the wall back. The reels are virtual here, but the frame is real: it
// goes out as m*h through busSend -> vbus -> vmodule, so the panel visibly flips
// down to blank.
//
// Runtime-only -- OFF at boot, never persisted -- so a reboot is a guaranteed
// return to normal operation.
volatile bool gQuietTime = false;
// Last status the companion app reported, and when (millis). Runtime-only.
char gCompanionStatus[80] = "";
// The tab list the companion advertised, kept as ready-made JSON so the 4-second
// dashboard poll can splice it into the response without re-serialising it. Empty
// = this companion never advertised any, and the dashboard uses its built-in list.
char gCompanionTabs[COMPANION_TABS_MAX] = "";
volatile unsigned long gCompanionSeenMs = 0;
volatile bool          gCompanionUrlDirty   = false;   // URL changed, not yet in flash
volatile unsigned long gCompanionUrlDirtyMs = 0;       // millis() of the LAST change
// Set when the wall changes (in busSend); the network task publishes
// the HA display-state topic (rate-limited) so HA reflects what's shown without
// spamming. busSend sets it; the network task (tasks.cpp) reads and clears it.
volatile bool gDisplayDirty = false;
// Set for the duration of a web OTA upload. While true the network task skips
// MQTT status/display/discovery publishes so the upload has the heap and CPU it
// needs (these reduce the contiguous heap the WiFi/TCP stack relies on, a known
// cause of mid-upload connection drops). Set by the OTA handlers; read by the
// network task.
volatile bool gOtaInProgress = false;
volatile bool gCanvasMode    = false;
// taskDisplay sets this true once it has seen gCanvasMode (or gOtaInProgress) and parked,
// having finished any in-flight repaint. The canvas take-over waits for it, so the reel
// renderer's closing swap can never land the wall back over the first canvas frame.
volatile bool gDispParked    = false;
// Set once a verified image is flashed and the HTTP 200 has been queued. taskWeb
// reboots when it sees this, AFTER handleClient() has flushed and closed the socket.
// Restarting from inside the handler tears the connection down mid-response, which
// the browser reports as a connection error on an upload that actually succeeded.
volatile bool gOtaRebootPending = false;
// Fallback SoftAP: the AP is only brought up when the station is NOT connected
// to a configured network (so the config page stays reachable). Once the station
// connects, the AP is dropped and the gateway runs STA-only. Tracks whether the
// AP is currently up so we only switch WiFi modes on actual state transitions.
bool gApActive = false;
volatile RtcTime rtcNow = {2000,1,1,0,0,0,false};
// POSIX TZ string used by rtcFormatTime; kept in sync with cfg.posixTZ.
char gPosixTZ[64] = "UTC0";
GwConfig cfg;
// Mutex protecting setenv/tzset/localtime (not thread-safe in newlib)
SemaphoreHandle_t     timeMutex     = NULL;
StaticSemaphore_t     timeMutexBuf;
// Watchdog timestamps -- each task writes millis() here every iteration
volatile unsigned long wdgBusMs     = 0;
volatile unsigned long gLastRxMs    = 0;  // millis() of last byte received on the bus
int                    mqttFailCount = 0;  // consecutive MQTT connect failures
volatile unsigned long wdgNetMs     = 0;
volatile unsigned long wdgWebMs     = 0;
volatile unsigned long wdgDispMs    = 0;
// Task handles -- used for uxTaskGetStackHighWaterMark on the Status page so
// stack pressure is visible BEFORE it becomes a canary crash.
TaskHandle_t hTaskRTC = NULL, hTaskBus = NULL, hTaskOTA = NULL,
                    hTaskWeb = NULL, hTaskNet = NULL, hTaskDisp = NULL;
// MQTT outbound queue -- bus/web tasks enqueue; network task publishes

// Outbound MQTT publish queue (~25 KB). Lives in PSRAM -- it's drained by the
// network task and written under mqttQMutex, never from an ISR or DMA, so the
// slightly slower PSRAM is fine and it frees ~25 KB of internal RAM. Allocated
// in psramAllocInit(); falls back to internal RAM if PSRAM is unavailable.
MqttQItem*            mqttQueue     = NULL;
volatile int          mqttQHead     = 0;
volatile int          mqttQTail     = 0;
SemaphoreHandle_t     mqttQMutex    = NULL;
StaticSemaphore_t     mqttQMutexBuf;
// Scheduled outbound frame ring (paces /api/bus/batch off taskWeb -- see bus.h).
// PSRAM: written by taskWeb, drained by taskBus, never from an ISR or DMA.
TxQItem*              txQueue       = NULL;
volatile int          txQHead       = 0;
volatile int          txQTail       = 0;
SemaphoreHandle_t     txQMutex      = NULL;
StaticSemaphore_t     txQMutexBuf;
// Serializes the module-touching section of busSend. There is no UART to garble any
// more, but the section is still shared mutable RAM: a static scratch buffer, the
// txCount counter, vbusDeliver's mutation of the module array, and the MQTT mirror.
// taskWeb (REST, core 0), taskNetwork (MQTT, core 1) and taskBus (scheduled batch
// frames, core 0) all call busSend, so it is genuinely concurrent and genuinely
// cross-core. Lock order is ALWAYS txMutex -> vmMutex (busSend -> vbusDeliver ->
// vmDispatch): never call busSend while holding vmMutex, or vbusDeliver re-takes it
// and self-deadlocks. The rule used to name the module registry's lock. The registry
// is gone; the rule is not -- it now guards the thing that was the truth all along.
SemaphoreHandle_t txMutex = NULL;
StaticSemaphore_t txMutexBuf;
bool          ntpSynced   = false; // true once NTP sync succeeds (in taskNetwork)
/* ----------------------------------------------------------
   Persistent configuration stored in NVS
---------------------------------------------------------- */
Preferences prefs;
// The command log (64 x ~168 bytes ~= 11 KB) lives in PSRAM, not internal
// RAM. It's a diagnostic log on no hot path, so the slightly slower PSRAM is
// fine, and freeing that internal DRAM gives the WiFi/TCP stack the
// headroom it needs during a large web-OTA upload (which otherwise exhausts the
// heap as receive buffers pile up). Allocated in setup() via psramAllocInit();
// falls back to internal RAM if PSRAM is unavailable. Never freed.
GwLogEntry*       logRing       = NULL;
volatile int      logHead       = 0;
volatile int      logPollCursor = 0;
StaticSemaphore_t msgMutexBuf;
SemaphoreHandle_t msgMutex = NULL;
/* ----------------------------------------------------------
   Bus low-level
---------------------------------------------------------- */
volatile unsigned long rxCount = 0;
volatile unsigned long txCount = 0;
volatile unsigned long vbusDropped   = 0;   // module replies lost to a full queue
bool sfFsReady = false;   // set true once FFat is mounted

/* ----------------------------------------------------------
   The emulated modules
   ----------------------------------------------------------
   THE wall. Not a model of it and not a cache of it: vmInit() creates every module from
   rows x cols, none can appear or vanish, and vmods[i].curIndex IS the flap on show. Every
   read path -- /api/display/state, /api/status, the MQTT display sensor, the quiet-time
   snapshot -- reads it directly. There used to be a second copy of this (the module
   registry: one byte per cell), and a byte cannot name a flap on a 237-flap reel.

   Lives in INTERNAL RAM, not PSRAM: taskDisplay walks the whole array a hundred times a
   second (vmTick) on the core the panel refresh runs on, and this board's quad-SPI PSRAM
   stalls that walk long enough to wander the OE window -- an idle wall that shimmers.
   vmInit() pins it with MALLOC_CAP_INTERNAL for exactly that reason. The monitor ring and
   the MQTT queue ARE in PSRAM (gwPsramAlloc); neither is walked on a 100 Hz path.
---------------------------------------------------------- */
VModule*              vmods     = NULL;
int                   vmCount   = 0;
SemaphoreHandle_t     vmMutex   = NULL;
StaticSemaphore_t     vmMutexBuf;
WiFiClient   mqttWifiClient;        // persistent client for PubSubClient
PubSubClient mqtt(mqttWifiClient);  // mqttInit() configures timeouts on this

unsigned long lastStatusMs = 0;
unsigned long          lastDispPubMs   = 0;
unsigned long mqttRetryMs  = 0;
// Grows on repeated MQTT connect failures (30s -> 300s) so an unreachable broker is
// not retried every 30 s forever; reset to 30 s on a successful connect.
unsigned long mqttRetryDelayMs = 30000UL;
HealWebServer server(80);
unsigned long staDownSince = 0;   // millis() the station last dropped (0 = up/never)
