#include "common.h"
#include "backup.h"
#include "config.h"
#include "sdcard.h"
#include "rtc.h"
#include <FFat.h>
#include <SD_MMC.h>
#include <vector>

// backup.cpp -- see backup.h. Everything here runs on the loop() task except
// backupRequest() (any task; it only sets a flag) and backupRestoreIfNeeded()
// (setup(), single-threaded). FF_FS_REENTRANT serializes the underlying FatFs
// access against web handlers touching either volume mid-pass.

static const char* MIRROR_ROOT = "/backup/fatfs";
static const int   MAX_DEPTH   = 8;
static const size_t COPY_BUF   = 16384;

static BackupStatus st = {};
static volatile bool reqFlag = false;

void backupRequest() { reqFlag = true; }
const BackupStatus& backupStatus() { return st; }

// One directory level, snapshotted so at most one dir handle is ever open
// (FATFS's open-file table is tiny -- same rule as sdRemoveTree).
struct Ent { String name; bool dir; uint64_t size; time_t mtime; };

static bool listDir(fs::FS& fs, const char* path, std::vector<Ent>& out) {
  File d = fs.open(path);
  if (!d || !d.isDirectory()) return false;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    Ent e; e.name = f.name(); e.dir = f.isDirectory();
    e.size = e.dir ? 0 : f.size(); e.mtime = e.dir ? 0 : f.getLastWrite();
    out.push_back(e);
    f.close();
  }
  d.close();
  return true;
}

static bool copyFile(fs::FS& srcFs, const char* srcPath, fs::FS& dstFs, const char* dstPath,
                     uint8_t* buf, uint64_t& bytesOut) {
  File s = srcFs.open(srcPath, FILE_READ);
  if (!s) return false;
  File t = dstFs.open(dstPath, FILE_WRITE);
  if (!t) { s.close(); return false; }
  bool ok = true;
  size_t sinceYield = 0;
  while (true) {
    int n = s.read(buf, COPY_BUF);
    if (n < 0) { ok = false; break; }
    if (n == 0) break;
    if (t.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
    bytesOut += (uint64_t)n;
    sinceYield += (size_t)n;
    if (sinceYield >= 65536) { sinceYield = 0; vTaskDelay(1); }  // keep IDLE fed
  }
  s.close(); t.close();
  if (!ok) dstFs.remove(dstPath);
  return ok;
}

// Mirror one FATFS directory into the card mirror: copy new/changed files, recurse
// into subdirs, then prune mirror entries with no source counterpart.
static bool mirrorDir(const char* srcPath, const char* dstPath, uint8_t* buf, int depth) {
  if (depth > MAX_DEPTH) return true;
  std::vector<Ent> src;
  if (!listDir(FFat, srcPath, src)) return false;
  if (!SD_MMC.exists(dstPath) && !SD_MMC.mkdir(dstPath)) return false;
  std::vector<Ent> dst;
  listDir(SD_MMC, dstPath, dst);

  for (auto& e : src) {
    String sp = String(srcPath) + "/" + e.name;
    String dp = String(dstPath) + "/" + e.name;
    if (sp.startsWith("//")) sp = sp.substring(1);   // FFat root is "/"
    if (e.dir) {
      if (!mirrorDir(sp.c_str(), dp.c_str(), buf, depth + 1)) st.lastOk = false;
      continue;
    }
    const Ent* m = nullptr;
    for (auto& x : dst) if (!x.dir && x.name == e.name) { m = &x; break; }
    // Recopy on size change, or when the source is newer (2 s slack for FAT
    // timestamp granularity). A fresh restore leaves FATFS copies newer than the
    // mirror, so the next pass re-syncs them once and then goes quiet.
    const bool fresh = m && m->size == e.size && e.mtime <= m->mtime + 2;
    if (fresh) { st.skipped++; continue; }
    if (copyFile(FFat, sp.c_str(), SD_MMC, dp.c_str(), buf, st.bytes)) st.copied++;
    else { st.lastOk = false; sdLog("backup: copy failed %s", sp.c_str()); }
    vTaskDelay(1);
  }
  for (auto& x : dst) {
    bool present = false;
    for (auto& e : src) if (e.name == x.name && e.dir == x.dir) { present = true; break; }
    if (present) continue;
    String dp = String(dstPath) + "/" + x.name;
    if (x.dir ? sdRemoveTree(dp.c_str()) : SD_MMC.remove(dp.c_str())) st.pruned++;
    else st.lastOk = false;
  }
  return true;
}

static void runPass() {
  st.running = true;
  st.copied = st.pruned = st.skipped = 0; st.bytes = 0; st.lastOk = true;
  const uint32_t t0 = millis();
  uint8_t* buf = (uint8_t*)heap_caps_malloc(COPY_BUF, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(COPY_BUF);
  if (!buf) { st.running = false; st.lastOk = false; return; }

  if (!SD_MMC.exists("/backup")) SD_MMC.mkdir("/backup");
  if (!mirrorDir("/", MIRROR_ROOT, buf, 0)) st.lastOk = false;

  // The full settings export rides along: NVS is not touched by a FATFS reformat,
  // but a dead board or an NVS erase is -- with this file the card alone can
  // rebuild the whole configuration (POST /api/config/import).
  {
    JsonDocument doc;
    cfgExportJson(doc);
    File f = SD_MMC.open("/backup/config.json", FILE_WRITE);
    if (f) { serializeJsonPretty(doc, f); f.close(); }
    else st.lastOk = false;
  }

  free(buf);
  st.lastMs = millis() - t0;
  st.lastEpoch = (uint32_t)rtcEpochNow();
  st.everRan = true;
  st.running = false;
  sdLog("backup: %s -- %lu copied, %lu pruned, %lu unchanged, %llu KB, %lu ms",
        st.lastOk ? "ok" : "ERRORS", (unsigned long)st.copied, (unsigned long)st.pruned,
        (unsigned long)st.skipped, (unsigned long long)(st.bytes / 1024), (unsigned long)st.lastMs);
}

void backupTick() {
  if (gOtaInProgress || !sdReady()) return;
  bool run = reqFlag;

  // Nightly pass at 03:30 local (same browser-supplied offset the quiet/dim
  // schedules use), once per day.
  static int lastDay = -1;
  const unsigned long ep = rtcEpochNow();
  if (!run && cfg.backupEnabled && ep > 1700000000UL) {
    time_t lt = (time_t)(ep + (long)cfg.quietTzOffsetMin * 60);
    struct tm tmv; gmtime_r(&lt, &tmv);
    if (tmv.tm_hour == 3 && tmv.tm_min >= 30 && tmv.tm_yday != lastDay) {
      lastDay = tmv.tm_yday;
      run = true;
    }
  }
  // First pass ~2 min after boot when the card has no mirror yet: protection
  // should not wait for tonight.
  if (!run && cfg.backupEnabled && !st.everRan && millis() > 120000UL &&
      !SD_MMC.exists(MIRROR_ROOT)) run = true;

  if (!run) return;
  reqFlag = false;
  runPass();
}

// Boot-time restore: card mirror -> freshly formatted FATFS. No pruning (the
// target is empty), no staleness checks (anything on the card beats nothing).
static int restoreDir(const char* srcPath, const char* dstPath, uint8_t* buf, int depth) {
  if (depth > MAX_DEPTH) return 0;
  std::vector<Ent> src;
  if (!listDir(SD_MMC, srcPath, src)) return 0;
  int n = 0;
  for (auto& e : src) {
    String sp = String(srcPath) + "/" + e.name;
    String dp = String(dstPath) + "/" + e.name;
    if (dp.startsWith("//")) dp = dp.substring(1);
    if (e.dir) {
      if (!FFat.exists(dp.c_str())) FFat.mkdir(dp.c_str());
      n += restoreDir(sp.c_str(), dp.c_str(), buf, depth + 1);
      continue;
    }
    uint64_t bytes = 0;
    if (copyFile(SD_MMC, sp.c_str(), FFat, dp.c_str(), buf, bytes)) n++;
    else printf("[BACKUP] restore failed: %s\n", dp.c_str());
  }
  return n;
}

int backupRestoreIfNeeded(bool fatfsWasFormatted) {
  if (!fatfsWasFormatted || !sdReady() || !SD_MMC.exists(MIRROR_ROOT)) return 0;
  printf("[BACKUP] FATFS was formatted this boot -- restoring mirror from the card\n");
  uint8_t* buf = (uint8_t*)heap_caps_malloc(COPY_BUF, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(COPY_BUF);
  if (!buf) return 0;
  const uint32_t t0 = millis();
  int n = restoreDir(MIRROR_ROOT, "/", buf, 0);
  free(buf);
  sdLog("backup: RESTORED %d files to FATFS after reformat (%lu ms)", n,
        (unsigned long)(millis() - t0));
  return n;
}
