#include "gateway.h"
#include "sdcard.h"
#include "SD_MMC.h"

// sdcard.cpp -- see sdcard.h. SD_MMC 1-bit mode on the Waveshare-documented pins.

#define SD_PIN_CLK  1
#define SD_PIN_CMD  44
#define SD_PIN_D0   17

static bool        gSdReady = false;
static const char* gSdType  = "none";

static const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    default:        return "unknown";
  }
}

void sdInit() {
  // setPins must precede begin; both can fail benignly (no card, bad card, wiring).
  if (!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0)) {
    printf("[SD] setPins failed -- microSD disabled\n");
    return;
  }
  // "/sdcard" mount, 1-bit mode (true), default freq. A missing card returns false here.
  if (!SD_MMC.begin("/sdcard", true)) {
    printf("[SD] no card / mount failed -- microSD disabled\n");
    return;
  }
  const uint8_t ct = SD_MMC.cardType();
  if (ct == CARD_NONE) {
    printf("[SD] card slot empty -- microSD disabled\n");
    SD_MMC.end();
    return;
  }
  gSdType  = cardTypeName(ct);
  gSdReady = true;
  printf("[SD] %s mounted: %llu MB total, %llu MB used\n", gSdType,
         SD_MMC.cardSize() / (1024ULL * 1024ULL),
         SD_MMC.usedBytes() / (1024ULL * 1024ULL));
}

bool sdReady() { return gSdReady; }

/* ---- on-card event log (v3.13.2) ---------------------------------------------------
   Append-only /logs/gateway.log, mutex-serialized (callers live on several tasks),
   rotated to gateway.old at 512 KB. Deliberately open-append-close per line: the rate
   is a few lines a minute, and never holding a handle means a crash can lose at most
   the line being written. Fetch via the SD browser or /api/sd/get -- the point is
   troubleshooting with NOTHING attached to USB. Timestamped with wall-clock when NTP
   has synced, else uptime. */
#define SD_LOG_PATH  "/logs/gateway.log"
#define SD_LOG_OLD   "/logs/gateway.old"
#define SD_LOG_ROTATE (512u * 1024u)
static SemaphoreHandle_t sdLogMux = nullptr;

void sdLog(const char* fmt, ...) {
  if (!gSdReady) return;
  if (!sdLogMux) { sdLogMux = xSemaphoreCreateMutex(); if (!sdLogMux) return; }
  char line[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  char stamp[40];
  const uint32_t ep = rtcEpochNow();
  if (ep) {
    time_t t = (time_t)ep; struct tm lt; localtime_r(&t, &lt);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
  } else snprintf(stamp, sizeof(stamp), "up+%lus", (unsigned long)(millis() / 1000));
  if (xSemaphoreTake(sdLogMux, pdMS_TO_TICKS(250)) != pdTRUE) return;   // never block a task on the log
  SD_MMC.mkdir("/logs");                              // idempotent
  File f = SD_MMC.open(SD_LOG_PATH, FILE_APPEND);
  if (f) {
    if (f.size() > SD_LOG_ROTATE) {                   // rotate: current -> .old (replace)
      f.close();
      SD_MMC.remove(SD_LOG_OLD);
      SD_MMC.rename(SD_LOG_PATH, SD_LOG_OLD);
      f = SD_MMC.open(SD_LOG_PATH, FILE_APPEND);
    }
    if (f) { f.printf("%s %s\n", stamp, line); f.close(); }
  }
  xSemaphoreGive(sdLogMux);
}

bool sdInfo(uint64_t& sizeMB, uint64_t& usedMB, const char*& type) {
  if (!gSdReady) return false;
  sizeMB = SD_MMC.cardSize()  / (1024ULL * 1024ULL);
  usedMB = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
  type   = gSdType;
  return true;
}

static bool sdRemoveTreeDepth(const char* path, int depth);
bool sdRemoveTree(const char* path) { return sdRemoveTreeDepth(path, 0); }

static bool sdRemoveTreeDepth(const char* path, int depth) {
  if (!gSdReady || !path || !path[0]) return false;
  if (depth > 16) return false;      // stack guard: one C frame per level on the httpd task
  String p(path);
  { File probe = SD_MMC.open(p);                 // file, or a directory to recurse into?
    if (!probe) return false;
    const bool isDir = probe.isDirectory();
    probe.close();
    if (!isDir) return SD_MMC.remove(p);
  }
  // Directory: delete the FIRST remaining child each pass, re-opening the dir between passes,
  // so only one handle is live at a time. Each pass makes progress (the dot-entry skip below
  // is defensive only -- ESP-IDF's FATFS VFS never emits them), so this terminates.
  for (int guard = 0; guard < 200000; guard++) {
    wdgWebMs = millis();             // a big tree wipe must not trip the web-stall watchdog
    File dir = SD_MMC.open(p);
    if (!dir) return false;
    File e = dir.openNextFile();
    if (!e) { dir.close(); break; }               // empty now
    String name = e.name();
    const bool eDir = e.isDirectory();
    e.close();
    dir.close();
    // name() may be a basename or a full path depending on the core; normalise, skip . / ..
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name == "." || name == "..") continue;    // defensive: would be a non-progress pass
    String child = p.endsWith("/") ? p + name : p + "/" + name;
    const bool ok = eDir ? sdRemoveTreeDepth(child.c_str(), depth + 1) : SD_MMC.remove(child);
    if (!ok) return false;                         // couldn't clear a child: give up (no infinite loop)
  }
  return SD_MMC.rmdir(p);
}
