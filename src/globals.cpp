#include "gateway.h"


// globals.cpp -- single definition site for every shared global.
// Each variable here is declared 'extern' in common.h (or a subsystem header)
// and defined exactly once below, so there is one authoritative home for all
// cross-file state (config, registry, queues, mutexes, task handles, ...).
// Single definition site for every shared global declared extern in common.h.

// Debug flag read by the DBG() macro. Mirrors cfg.serialDebug, kept in sync in
// loadConfig() and handleApiConfigSettings().
volatile bool gSerialDebug = false;
// Maintenance mode: when true, external commands arriving via MQTT are ignored
// and not relayed to the RS-485 bus. The web UI / REST API (the gateway itself)
// continue to work normally. Always OFF at boot -- never persisted -- so a
// reboot is a guaranteed return to normal operation.
volatile bool gMaintenanceMode = false;
// Quiet Time: when true the gateway still accepts and acknowledges every command,
// but does NOT transmit normal display-motion frames to the bus (show character,
// show index, and home), so the flaps stay still during quiet hours. Display
// tracking is left unchanged
// (it reflects the physically-shown flap). The latest requested display per
// module is remembered so the reels can resync when Quiet Time turns off.
// Runtime-only -- OFF at boot, never persisted -- like maintenance mode.
volatile bool gQuietTime = false;
// Last flap byte transmitted to each module id (= grid cell). Drives the display
// wall so it mirrors every cell the gateway sent to, provisioned or not. Written
// in sfTrackFromFrame (the single frame choke point); 0 = nothing sent yet.
char gWallChars[256] = {0};
// v3.0: last status the companion app reported, and when (millis). Runtime-only.
char gCompanionStatus[80] = "";
volatile unsigned long gCompanionSeenMs = 0;
volatile bool          gCompanionUrlDirty   = false;   // URL changed, not yet in flash
volatile unsigned long gCompanionUrlDirtyMs = 0;       // millis() of the LAST change
// Set when display tracking changes (in rs485Send); the network task publishes
// the HA display-state topic (rate-limited) so HA reflects what's shown without
// spamming. rs485Send sets it; the network task (tasks.cpp) reads and clears it.
volatile bool gDisplayDirty = false;
// Set for the duration of a web OTA upload. While true the network task skips
// MQTT status/display/discovery publishes so the upload has the heap and CPU it
// needs (these reduce the contiguous heap the WiFi/TCP stack relies on, a known
// cause of mid-upload connection drops). Set by the OTA handlers; read by the
// network task.
volatile bool gOtaInProgress = false;
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
volatile unsigned long wdgRS485Ms   = 0;
volatile unsigned long gLastRxMs    = 0;  // millis() of last byte received on the bus
int                    mqttFailCount = 0;  // consecutive MQTT connect failures
volatile unsigned long wdgNetMs     = 0;
volatile unsigned long wdgWebMs     = 0;
volatile unsigned long wdgDispMs    = 0;
// Task handles -- used for uxTaskGetStackHighWaterMark on the Status page so
// stack pressure is visible BEFORE it becomes a canary crash.
TaskHandle_t hTaskRTC = NULL, hTaskRS485 = NULL, hTaskOTA = NULL,
                    hTaskWeb = NULL, hTaskNet = NULL, hTaskDisp = NULL;
// MQTT outbound queue -- RS485/web tasks enqueue; network task publishes

// Outbound MQTT publish queue (~25 KB). Lives in PSRAM -- it's drained by the
// network task and written under mqttQMutex, never from an ISR or DMA, so the
// slightly slower PSRAM is fine and it frees ~25 KB of internal RAM. Allocated
// in psramAllocInit(); falls back to internal RAM if PSRAM is unavailable.
MqttQItem*            mqttQueue     = NULL;
volatile int          mqttQHead     = 0;
volatile int          mqttQTail     = 0;
SemaphoreHandle_t     mqttQMutex    = NULL;
StaticSemaphore_t     mqttQMutexBuf;
// Scheduled outbound frame ring (paces /api/rs485/batch off taskWeb -- see rs485.h).
// PSRAM: written by taskWeb, drained by taskRS485, never from an ISR or DMA.
TxQItem*              txQueue       = NULL;
volatile int          txQHead       = 0;
volatile int          txQTail       = 0;
SemaphoreHandle_t     txQMutex      = NULL;
StaticSemaphore_t     txQMutexBuf;
// Module registry (~14 KB). Lives in PSRAM -- accessed only under sfMutex from
// normal tasks (never an ISR, never DMA, and not during flash writes), and the
// 9600-baud bus is far from a hot loop, so PSRAM latency is negligible while it
// frees ~14 KB of internal RAM. Allocated in psramAllocInit(); falls back to
// internal RAM if PSRAM is unavailable.
SFModule*            sfModules     = NULL;
SemaphoreHandle_t sfMutex = NULL;
StaticSemaphore_t sfMutexBuf;
// Serializes the bus-touching section of rs485Send. There is no UART to garble any
// more, but the section is still shared mutable RAM: a static scratch buffer, the
// txCount counter, vbusDeliver's mutation of the module array, and the monitor-ring
// push. taskWeb (REST, core 0), taskNetwork (MQTT, core 1) and taskRS485 (discovery,
// core 0) all call rs485Send, so it is genuinely concurrent and genuinely cross-core.
// Lock order is ALWAYS txMutex -> sfMutex (rs485Send -> sfTrackFromFrame): never call
// rs485Send while holding sfMutex (sfTrackFromFrame would re-take it and self-deadlock).
SemaphoreHandle_t txMutex = NULL;
StaticSemaphore_t txMutexBuf;
int      sfModuleCount = 0;
// Shared single-slot capture for the most recent combined 'A' reply -- the only
// multi-field response left. handleApiAll records the id it is waiting for, then
// polls for a match. One slot is enough: the web server is synchronous.
DumpCapture gDump;     // 'A' combined-reply capture
volatile bool          sfModulesDirty   = false;  // pending NVS save
volatile unsigned long sfModulesDirtyMs = 0;      // millis() when first dirtied
bool          ntpSynced   = false; // true once NTP sync succeeds (in taskNetwork)
/* ----------------------------------------------------------
   Persistent configuration stored in NVS
---------------------------------------------------------- */
Preferences prefs;
// The bus-monitor ring (64 x ~296 bytes ~= 19 KB) lives in PSRAM, not internal
// RAM. It's a diagnostic log on no hot path, so the slightly slower PSRAM is
// fine, and freeing ~19 KB of internal DRAM gives the WiFi/TCP stack the
// headroom it needs during a large web-OTA upload (which otherwise exhausts the
// heap as receive buffers pile up). Allocated in setup() via ringInit(); falls
// back to internal RAM if PSRAM is unavailable. Never freed.
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
   ~270 bytes each, so a 128-module ceiling is ~35 KB. Lives in INTERNAL RAM, not
   PSRAM: taskDisplay walks the whole array a hundred times a second (vmTick) on the
   core the panel refresh runs on, and this board's quad-SPI PSRAM stalls that walk
   long enough to wander the OE window -- an idle wall that shimmers. vmInit() pins
   it with MALLOC_CAP_INTERNAL for exactly that reason. The monitor ring, the MQTT
   queue and the module registry ARE in PSRAM (gwPsramAlloc); none of them is walked
   on a 100 Hz path.
---------------------------------------------------------- */
VModule*              vmods     = NULL;
int                   vmCount   = 0;
SemaphoreHandle_t     vmMutex   = NULL;
StaticSemaphore_t     vmMutexBuf;
volatile bool          vmDirty   = false;   // pending /vmods.dat save
volatile unsigned long vmDirtyMs = 0;       // millis() when first dirtied
WiFiClient   wifiClient;
WiFiClient   mqttWifiClient;        // persistent client for PubSubClient
PubSubClient mqtt(mqttWifiClient);  // mqttInit() configures timeouts on this

unsigned long lastStatusMs = 0;
unsigned long          lastDispPubMs   = 0;
unsigned long mqttRetryMs  = 0;
// Grows on repeated MQTT connect failures (30s -> 300s) so an unreachable broker is
// not retried every 30 s forever; reset to 30 s on a successful connect.
unsigned long mqttRetryDelayMs = 30000UL;
WebServer server(80);
unsigned long staDownSince = 0;   // millis() the station last dropped (0 = up/never)
