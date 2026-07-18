// rtc.h -- wall-clock time: the ESP32's internal RTC + NTP.
//
// The API is unchanged from the physical Split-Flap Gateway. The Waveshare
// board carries the same battery-backed PCF85063: it seeds the system clock at
// boot (valid time seconds after power-on, no network needed) and is written
// back on every NTP sync. With no backup cell the old behaviour returns --
// time is invalid until the first sync, which every caller already handles.

#ifndef MPGW_RTC_H
#define MPGW_RTC_H

#include "common.h"
#include <time.h>

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  bool     valid;
};

// ---- owned globals (defined in globals.cpp) ----
extern volatile RtcTime rtcNow;
extern char gPosixTZ[64];

void rtcHwInit();
void rtcRead();
bool rtcNTPSync();
void rtcFormatTime(char* out, size_t outLen);
unsigned long rtcEpochNow();
bool rtcLocalNow(struct tm* out);   // broken-down local time; false if clock unset or TZ lock busy
void cfgApplyTZ();                   // (re)apply gPosixTZ to the process env, serialised on timeMutex

#endif // MPGW_RTC_H
