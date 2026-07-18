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

void rtcHwInit() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  uint8_t ctrl = 0x01;                             // 24h mode, oscillator on
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
    // The system TZ is UTC at boot (cfgApplyTZ runs later), so mktime is timegm.
    time_t t = mktime(&tmv);
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
  struct tm info;
  unsigned long start = millis();
  while (!getLocalTime(&info, 200)) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      DBG("[NTP] timed out\n");
      return false;
    }
  }
  rtcRead();
  // Discipline the battery RTC with the fresh NTP time, so the corrected clock
  // survives the next power cycle.
  rtcChipWriteUnix(time(NULL));
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
