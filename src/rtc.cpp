#include "gateway.h"

// rtc.cpp -- wall-clock time from the ESP32's internal RTC, synced by NTP.
//
// The RS-485 gateway reads a battery-backed PCF85063 over I2C. This board has no
// RTC chip, so the same API is served from the system clock: rtcNTPSync() calls
// configTime() (always with a zero offset, so the system clock is UTC and mktime
// acts as timegm), and rtcRead() snapshots gmtime() into rtcNow once a second.
//
// Two consequences, both already handled by every caller:
//   * Time is invalid from power-on until the first NTP sync. rtcNow.valid stays
//     false and rtcEpochNow() returns 0, which the module pruner treats as "no
//     valid clock yet" and the frame timestamps render as HH:MM:SS uptime.
//   * A reboot without network loses the wall clock. The persisted module
//     registry copes: entries carry an epoch and are only pruned once the clock
//     is trustworthy again.
//
// Local time is derived at format time from the configured POSIX TZ string. TZ is
// set ONCE (loadConfig, and again only when the timezone changes) because
// repeated setenv() leaks heap on ESP32 newlib -- the bug that caused the RS-485
// gateway's long-standing ~132 bytes/30 s drain. timeMutex serialises the
// formatting calls: newlib's time functions and the TZ environment are
// process-wide and not thread-safe.

// The system clock starts at the epoch. Treat anything before 2020-01-01 as "not
// yet synced" rather than reporting 1970 timestamps as fact.
#define RTC_VALID_AFTER  1577836800UL   // 2020-01-01T00:00:00Z

void rtcHwInit() {
  // Nothing to bring up: no RTC chip, and the ESP32's RTC is already running.
  // I2C is started anyway so a STEMMA QT device (or the onboard LIS3DH at 0x19)
  // can be added later without touching the boot sequence.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  DBG("[RTC] internal RTC; waiting for NTP\n");
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
  // Compute the UTC epoch by hand (days-since-1970). This avoids timegm(), which
  // ESP32 newlib does not expose, and mktime()+setenv(), which leaks.
  static const int cumDays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = yr;
  long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cumDays[(mo - 1) % 12];
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (leap && mo > 2) days += 1;
  days += (dy - 1);
  time_t utcEpoch = (time_t)days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc;
  // localtime_r applies the TZ environment set once at boot.
  struct tm lt;
  localtime_r(&utcEpoch, &lt);
  snprintf(out, outLen, "%04d-%02d-%02d %02d:%02d:%02d",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec);
  xSemaphoreGive(timeMutex);
}

// Current UTC epoch, or 0 if the clock has never been set. Used for module
// last-seen tracking that must survive reboots (millis() resets).
unsigned long rtcEpochNow() {
  if (!rtcNow.valid) return 0;
  uint16_t yr = rtcNow.year;   uint8_t mo = rtcNow.month;
  uint8_t  dy = rtcNow.day;    uint8_t hr = rtcNow.hour;
  uint8_t  mn = rtcNow.minute; uint8_t sc = rtcNow.second;
  static const int cumDays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long y = yr;
  long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cumDays[(mo - 1) % 12];
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (leap && mo > 2) days += 1;
  days += (dy - 1);
  return (unsigned long)((long)days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc);
}
