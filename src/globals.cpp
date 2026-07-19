#include "gateway.h"


// globals.cpp -- single definition site for every shared global.
// Each variable here is declared 'extern' in common.h (or a subsystem header)
// and defined exactly once below, so there is one authoritative home for all
// cross-file state (config, queues, mutexes, task handles, ...).

// Debug flag read by the DBG() macro. Mirrors cfg.serialDebug, kept in sync in
// loadConfig() and handleApiConfigSettings().
volatile bool gSerialDebug = false;
// Quiet Time: when true the gateway still accepts and acknowledges every command,
// but does NOT transmit normal display-motion frames to the modules (show character,
// show index, and home), so the flaps stay still during quiet hours. What each
// module was asked to show meanwhile is remembered -- as a FLAP INDEX, in
// VModule::pendFlap -- so the wall can resync exactly when Quiet Time turns off.
//
// The wall is BLANKED as Quiet Time is entered -- sfSetQuietTime() homes every reel
// on the rising edge, and it must do so BEFORE raising this flag, because the
// suppression above would otherwise swallow the very home frame that blanks the
// wall. What each module was showing is snapshotted first, so the falling-edge
// resync puts the wall back. The reels are virtual here, but the frame is real: it
// goes out as m*h through frameSend -> vmDispatch, so the panel visibly flips
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
// Set for the duration of a web OTA upload. While true the display/frame tasks
// stand down so the upload has the heap and CPU it needs. Set by the OTA
// handler; read across tasks.
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
volatile unsigned long wdgFramesMs     = 0;
volatile unsigned long wdgNetMs     = 0;
volatile unsigned long wdgWebMs     = 0;
volatile unsigned long wdgDispMs    = 0;
// Task handles -- used for uxTaskGetStackHighWaterMark on the Status page so
// stack pressure is visible BEFORE it becomes a canary crash.
TaskHandle_t hTaskRTC = NULL, hTaskFrames = NULL,
                    hTaskWeb = NULL, hTaskNet = NULL, hTaskDisp = NULL;
// Scheduled outbound frame ring (paces /api/frames/batch off taskWeb -- see frames.h).
// PSRAM: written by taskWeb, drained by taskFrames, never from an ISR or DMA.
TxQItem*              txQueue       = NULL;
volatile int          txQHead       = 0;
volatile int          txQTail       = 0;
SemaphoreHandle_t     txQMutex      = NULL;
StaticSemaphore_t     txQMutexBuf;
// Serializes the module-touching section of frameSend. There is no UART to garble any
// more, but the section is still shared mutable RAM: a static scratch buffer, the
// txCount counter and vmDispatch's mutation of the module array. The httpd task
// (REST, core 0) and taskFrames (scheduled batch frames, core 0) both call
// frameSend, so it is genuinely concurrent. Lock order is ALWAYS txMutex -> vmMutex (frameSend takes vmMutex around
// vmDispatch): never call frameSend while holding vmMutex, or it re-takes it and
// self-deadlocks. The rule used to name the module registry's lock. The registry
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
   Frame counter
---------------------------------------------------------- */
volatile unsigned long txCount = 0;
bool sfFsReady = false;   // set true once FFat is mounted

/* ----------------------------------------------------------
   The emulated modules
   ----------------------------------------------------------
   THE wall. Not a model of it and not a cache of it: vmInit() creates every module from
   rows x cols, none can appear or vanish, and vmods[i].curIndex IS the flap on show. Every
   read path -- /api/display/state, /api/status, the SSE stream, the quiet-time
   snapshot -- reads it directly. There used to be a second copy of this (the module
   registry: one byte per cell), and a byte cannot name a flap on a 237-flap reel.

   Lives in INTERNAL RAM, not PSRAM: taskDisplay walks the whole array a hundred times a
   second (vmTick) on the core the panel refresh runs on, and this board's quad-SPI PSRAM
   stalls that walk long enough to wander the OE window -- an idle wall that shimmers.
   vmInit() pins it with MALLOC_CAP_INTERNAL for exactly that reason. The monitor ring and
   ARE in PSRAM (gwPsramAlloc); none is walked on a 100 Hz path.
---------------------------------------------------------- */
VModule*              vmods     = NULL;
int                   vmCount   = 0;
SemaphoreHandle_t     vmMutex   = NULL;
StaticSemaphore_t     vmMutexBuf;
unsigned long staDownSince = 0;   // millis() the station last dropped (0 = up/never)
