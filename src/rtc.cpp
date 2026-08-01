#include "gateway.h"

// rtc.cpp -- wall-clock time: the system clock, seeded by the PCF85063 and
// disciplined by NTP.
//
// The Waveshare board carries the same battery-backed PCF85063 the physical
// Split-Flap Gateway reads. The SYSTEM clock stays the single source of truth
// -- rtcRead() snapshots gmtime() once a second exactly as before -- and the
// chip plays two supporting roles:
//   * At boot, if it holds a plausible time (oscillator running, year >= 2020),
//     it SEEDS the system clock, so the wall clock is valid seconds after
//     power-on with no network at all.
//   * On every successful NTP sync the fresh UTC time is WRITTEN BACK, so the
//     chip (and its backup cell) carry the corrected time across power cycles.
// With no cell fitted the chip loses time on power-off and boot falls back to
// the old wait-for-NTP path: rtcNow.valid stays false and rtcEpochNow() returns
// 0 until the first sync, which every consumer already handles.
//
// Local time is derived at format time from the configured POSIX TZ string. TZ is
// set ONCE (loadConfig, and again only when the timezone changes) because
// repeated setenv() leaks heap on ESP32 newlib -- the bug that caused the physical
// gateway's long-standing ~132 bytes/30 s drain. timeMutex serialises the
// formatting calls: newlib's time functions and the TZ environment are
// process-wide and not thread-safe.

// The system clock starts at the epoch. Treat anything before 2020-01-01 as "not
// yet synced" rather than reporting 1970 timestamps as fact.
#define RTC_VALID_AFTER  1577836800UL   // 2020-01-01T00:00:00Z

#include "sdcard.h"   // sdLog: chip-discipline diagnostics (v3.15)
#include <esp_sntp.h>  // sntp_get_sync_status: REAL sync completion, not a plausible clock

static time_t rtcToEpochUTC(uint16_t yr, uint8_t mo, uint8_t dy, uint8_t hr, uint8_t mn, uint8_t sc);

/* ---- PCF85063 access (I2C, addr 0x51; registers in common.h) ---- */
static uint8_t rtcDecToBcd(int v)     { return (uint8_t)((v / 10 * 16) + (v % 10)); }
static int     rtcBcdToDec(uint8_t v) { return (v / 16 * 10) + (v % 16); }

static bool rtcChipWrite(uint8_t reg, const uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
  return Wire.endTransmission(true) == 0;
}
static bool rtcChipRead(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)PCF85063_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Write a UTC epoch into the chip's seven time registers.
static void rtcChipWriteUnix(time_t t) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  uint8_t buf[7] = {
    rtcDecToBcd(tmv.tm_sec),          // also clears the OS (oscillator-stop) flag
    rtcDecToBcd(tmv.tm_min),
    rtcDecToBcd(tmv.tm_hour),
    rtcDecToBcd(tmv.tm_mday),
    rtcDecToBcd(tmv.tm_wday),
    rtcDecToBcd(tmv.tm_mon + 1),
    rtcDecToBcd(tmv.tm_year - 100)    // reg 6 is 0-99 = 2000-2099
  };
  rtcChipWrite(PCF85063_SEC_REG, buf, 7);
}

// Read the chip's seven time registers as a UTC epoch; 0 = unreadable/not trusted.
static time_t rtcChipReadUnix() {
  uint8_t buf[7] = {0};
  if (!rtcChipRead(PCF85063_SEC_REG, buf, 7) || (buf[0] & 0x80)) return 0;
  struct tm tmv = {};
  tmv.tm_sec  = rtcBcdToDec(buf[0] & 0x7F);
  tmv.tm_min  = rtcBcdToDec(buf[1] & 0x7F);
  tmv.tm_hour = rtcBcdToDec(buf[2] & 0x3F);
  tmv.tm_mday = rtcBcdToDec(buf[3] & 0x3F);
  tmv.tm_mon  = rtcBcdToDec(buf[5] & 0x1F) - 1;
  tmv.tm_year = rtcBcdToDec(buf[6]) + RTC_YEAR_OFFSET - 1900;
  // Pure-arithmetic UTC conversion -- NOT mktime, which applies the PROCESS timezone:
  // once cfgApplyTZ has set EST5EDT, mktime here read the chip 5 h off and the verify
  // cried wolf ("OFF by 18000s", 2026-08-01). rtcToEpochUTC is TZ-independent.
  return (time_t)rtcToEpochUTC((uint16_t)(tmv.tm_year + 1900), (uint8_t)(tmv.tm_mon + 1),
                               (uint8_t)tmv.tm_mday, (uint8_t)tmv.tm_hour,
                               (uint8_t)tmv.tm_min,  (uint8_t)tmv.tm_sec);
}

/* ---- chip discipline, on the I2C-owner task (v3.15) -------------------------------
   The NTP write-back used to run rtcChipWriteUnix() directly on taskNetwork -- I2C
   from the WRONG task on a bus with no lock (taskRTC polls the SHTC3 and the IMU
   concurrently), so corrections could collide and garble. Now rtcNTPSync only sets a
   flag; taskRTC performs the write, VERIFIES it by reading back, and afterwards runs
   one rate check -- chip elapsed vs system elapsed over ~2 minutes -- so the SD log
   shows exactly how the crystal is behaving after the CAP_SEL fix. */
static volatile bool chipWbPending = false;
void rtcRequestChipWriteback() { chipWbPending = true; }

// sdLog does FatFs I/O -- far too stack-hungry for taskRTC (it blew the stack canary
// and boot-looped the board, 2026-08-01). The service formats its line into this
// mailbox; loop() -- a task with headroom -- writes it to the card.
static char          chipLogLine[96];
static volatile bool chipLogPending = false;
static void chipLog(const char* fmt, long a, long b) {
  snprintf(chipLogLine, sizeof(chipLogLine), fmt, a, b);
  chipLogPending = true;
}
bool rtcChipLogTake(char* out, size_t cap) {
  if (!chipLogPending) return false;
  strlcpy(out, chipLogLine, cap);
  chipLogPending = false;
  return true;
}

void rtcChipService() {
  static time_t rateChip0 = 0, rateSys0 = 0;
  static bool   rateLogged = false;
  if (chipWbPending) {
    chipWbPending = false;
    const time_t now = time(NULL);
    rtcChipWriteUnix(now);
    const time_t back = rtcChipReadUnix();
    const long   err  = back ? (long)(back - time(NULL)) : LONG_MAX;
    if (err == LONG_MAX)      chipLog("RTC writeback FAILED (chip unreadable)", 0, 0);
    else if (labs(err) > 2)   chipLog("RTC writeback VERIFY OFF by %lds", err, 0);
    else                      chipLog("RTC writeback ok (chip within %lds)", err, 0);
    rateChip0 = back; rateSys0 = time(NULL); rateLogged = false;
    return;
  }
  if (rateChip0 && !rateLogged && time(NULL) - rateSys0 >= 120) {
    const time_t chip = rtcChipReadUnix();
    if (chip) {
      const long chipEl = (long)(chip - rateChip0), sysEl = (long)(time(NULL) - rateSys0);
      chipLog("RTC rate check: chip advanced %lds over %lds real (healthy = equal)",
              chipEl, sysEl);
    }
    rateLogged = true;
  }
}

void rtcHwInit() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // Control_1 = 0x00: 24 h mode, oscillator running, CAP_SEL = 7 pF. The old value
  // 0x01 set CAP_SEL = 12.5 pF ("24h mode, oscillator on" was a mislabel) -- wrong
  // load for this board's crystal, which then oscillated on an overtone and counted
  // ~2x real time (+1 day of error per day; found via the SD log's boot stamps).
  uint8_t ctrl = 0x00;
  if (!rtcChipWrite(PCF85063_CTRL1, &ctrl, 1)) {
    printf("[RTC] PCF85063 not answering -- waiting for NTP\n");
    return;
  }
  // Seed the system clock from the chip if it survived the power cycle. Bit 7
  // of the seconds register is the oscillator-stop flag: set means the time is
  // not to be trusted (fresh board, or no backup cell).
  uint8_t buf[7] = {0};
  if (rtcChipRead(PCF85063_SEC_REG, buf, 7) && !(buf[0] & 0x80)) {
    struct tm tmv = {};
    tmv.tm_sec  = rtcBcdToDec(buf[0] & 0x7F);
    tmv.tm_min  = rtcBcdToDec(buf[1] & 0x7F);
    tmv.tm_hour = rtcBcdToDec(buf[2] & 0x3F);
    tmv.tm_mday = rtcBcdToDec(buf[3] & 0x3F);
    tmv.tm_mon  = rtcBcdToDec(buf[5] & 0x1F) - 1;
    tmv.tm_year = rtcBcdToDec(buf[6]) + RTC_YEAR_OFFSET - 1900;
    // TZ-independent conversion: loadConfig() applies the configured zone BEFORE this
    // runs (the old "TZ is UTC at boot, mktime is timegm" claim was false), which
    // skewed every pre-NTP timestamp by the zone offset.
    time_t t = (time_t)rtcToEpochUTC((uint16_t)(tmv.tm_year + 1900), (uint8_t)(tmv.tm_mon + 1),
                                     (uint8_t)tmv.tm_mday, (uint8_t)tmv.tm_hour,
                                     (uint8_t)tmv.tm_min,  (uint8_t)tmv.tm_sec);
    // Plausibility is a WINDOW, not a floor. A factory-fresh chip can hold
    // garbage with the oscillator-stop flag clear -- this board's first boot
    // read 2056 -- and a floor-only check happily seeds the future. Trust
    // nothing before this firmware was built or more than ~5 years after:
    // outside that, wait for NTP (which also rewrites the chip).
    struct tm bt = {};
    strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &bt);
    const time_t built = mktime(&bt);
    if (t >= built && t <= built + 5L * 365 * 86400) {
      struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
      settimeofday(&tv, NULL);
      printf("[RTC] PCF85063 seeded the clock: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
      return;
    }
  }
  printf("[RTC] PCF85063 present but time not trusted -- waiting for NTP\n");
}

// (Re)apply the configured POSIX zone (gPosixTZ) to the process environment. setenv/tzset and the
// localtime family are process-wide and not thread-safe in newlib, so every writer takes timeMutex
// -- the ONE place that does. Called from loadConfig (boot), a TZ change in settings, and after
// each NTP sync (configTime resets the zone to UTC). It waits up to a second rather than skipping:
// getting this wrong silently reverts every timestamp to UTC, and the lock is only ever held for
// the brief formatting calls. Before the scheduler exists (boot) there is no lock to take.
void cfgApplyTZ() {
  if (timeMutex && xSemaphoreTake(timeMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  setenv("TZ", gPosixTZ, 1);
  tzset();
  if (timeMutex) xSemaphoreGive(timeMutex);
}

// Days-since-1970 -> UTC epoch for a broken-down UTC time. Shared by rtcFormatTime, rtcEpochNow and
// rtcLocalNow; avoids timegm() (absent in ESP32 newlib) and mktime()+setenv() (which leaks heap).
static time_t rtcToEpochUTC(uint16_t yr, uint8_t mo, uint8_t dy, uint8_t hr, uint8_t mn, uint8_t sc) {
  static const int cumDays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = yr;
  long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cumDays[(mo - 1) % 12];
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (leap && mo > 2) days += 1;
  days += (dy - 1);
  return (time_t)days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc;
}

void rtcRead() {
  time_t now = time(NULL);
  if (now < (time_t)RTC_VALID_AFTER) { rtcNow.valid = false; return; }
  struct tm t;
  gmtime_r(&now, &t);                 // the system clock is UTC (configTime(0,0,..))
  rtcNow.second = t.tm_sec;
  rtcNow.minute = t.tm_min;
  rtcNow.hour   = t.tm_hour;
  rtcNow.day    = t.tm_mday;
  rtcNow.month  = t.tm_mon + 1;
  rtcNow.year   = t.tm_year + 1900;
  rtcNow.valid  = true;
}

// Always syncs in UTC. No tz offset is ever passed to configTime, which avoids
// the mktime/gmtime double-offset class of bug entirely.
bool rtcNTPSync() {
  const char* ntpSrv = cfg.ntpServer[0] ? cfg.ntpServer : DEFAULT_NTP_SERVER;
  DBG("[NTP] syncing (UTC) via %s...\n", ntpSrv);
  configTime(0, 0, ntpSrv);
  // Wait for a REAL SNTP completion -- NOT getLocalTime(), which returns true for any
  // plausible-looking clock. The RTC chip seeds a plausible clock at boot, so the old
  // loop "succeeded" instantly and the write-back ECHOED THE CHIP'S OWN (wrong) TIME
  // back into it -- the discipline was circular, and the chip's error self-perpetuated
  // across every boot (found 2026-08-01 chasing +36-day log stamps).
  unsigned long start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      DBG("[NTP] timed out\n");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  struct tm info;
  getLocalTime(&info, 10);           // now guaranteed fresh: format for the log below
  rtcRead();
  // Discipline the battery RTC with the fresh NTP time -- but not from THIS task:
  // the I2C bus has no lock and belongs to taskRTC. Queue it; rtcChipService()
  // writes and verifies there. (The old direct write from taskNetwork could collide
  // with sensor/IMU traffic.)
  rtcRequestChipWriteback();
  // configTime(0,0,..) resets the TZ env to UTC to keep the system clock in UTC -- but that also
  // clobbers the zone we set from cfg.posixTZ at boot, so every NTP sync silently reverted the
  // whole gateway (command-log timestamps, the clock effect, HA) to UTC. Restore the configured zone
  // so rtcFormatTime's localtime_r shows LOCAL time (cfgApplyTZ serialises setenv/tzset).
  cfgApplyTZ();
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &info);
  printf("[NTP] clock set: %s UTC\n", tbuf);
  return rtcNow.valid;
}

void rtcFormatTime(char* out, size_t outLen) {
  // Snapshot the volatile fields once.
  uint16_t yr = rtcNow.year;   uint8_t mo = rtcNow.month;
  uint8_t  dy = rtcNow.day;    uint8_t hr = rtcNow.hour;
  uint8_t  mn = rtcNow.minute; uint8_t sc = rtcNow.second;
  bool     vld = rtcNow.valid;

  // No clock yet, or the lock is contended: fall back to a TZ-free HH:MM:SS so
  // the caller still gets something, and never give a mutex we do not own.
  if (!vld || !timeMutex ||
      xSemaphoreTake(timeMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    snprintf(out, outLen, "%02u:%02u:%02u", hr, mn, sc);
    return;
  }
  time_t utcEpoch = rtcToEpochUTC(yr, mo, dy, hr, mn, sc);
  // localtime_r applies the TZ environment set once at boot.
  struct tm lt;
  localtime_r(&utcEpoch, &lt);
  snprintf(out, outLen, "%04d-%02d-%02d %02d:%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec);
  xSemaphoreGive(timeMutex);
}

// Current UTC epoch, or 0 if the clock has never been set. Consumers: the quiet
// schedule and the command log's browser-local timestamps.
unsigned long rtcEpochNow() {
  if (!rtcNow.valid) return 0;
  return (unsigned long)rtcToEpochUTC(rtcNow.year, rtcNow.month, rtcNow.day,
                                      rtcNow.hour, rtcNow.minute, rtcNow.second);
}

// Broken-down LOCAL time for the clock effect. Returns false (out untouched) if the clock is not
// yet valid or the TZ lock is contended -- the caller then holds its last good time. Cheaper than
// rtcFormatTime: one localtime_r, no strftime and no string parse back out.
bool rtcLocalNow(struct tm* out) {
  if (!rtcNow.valid) return false;
  time_t utc = rtcToEpochUTC(rtcNow.year, rtcNow.month, rtcNow.day,
                             rtcNow.hour, rtcNow.minute, rtcNow.second);
  if (!timeMutex || xSemaphoreTake(timeMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  localtime_r(&utc, out);
  xSemaphoreGive(timeMutex);
  return true;
}
