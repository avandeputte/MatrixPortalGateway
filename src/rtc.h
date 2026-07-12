// rtc.h -- wall-clock time: the ESP32's internal RTC + NTP.
//
// The API is unchanged from the RS-485 gateway, which reads a battery-backed
// PCF85063. The MatrixPortal S3 has no such chip, so the same calls are served by
// the ESP32's own RTC, seeded from NTP once WiFi is up. The visible difference is
// that time is INVALID from power-on until the first sync -- rtcNow.valid stays
// false and rtcEpochNow() returns 0 -- which is exactly the state every caller
// already handles (an RTC with a flat cell behaves the same way).

#ifndef MPGW_RTC_H
#define MPGW_RTC_H

#include "common.h"

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

#endif // MPGW_RTC_H
