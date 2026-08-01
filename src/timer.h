// timer.h -- kitchen timer + daily alarms (v3.14).
//
// One countdown timer (volatile) and up to four daily alarms (persisted in NVS via
// GwConfig). While a timer is counting or an alarm is firing, taskDisplay renders a
// full-screen view (big anti-aliased digits) that takes priority over every other
// display mode -- it is an ALERT, so wall pushes and effects wait underneath and
// resume when it releases.
//
// Sound: the timer chime and the alarm ring deliberately OVERRIDE Quiet Time (a 07:00
// alarm inside a 23:00-07:00 quiet window must still ring -- that is what alarms are
// for). The speaker master enable is still respected: with the speaker off the alert
// is visual only. Alarms evaluate on taskRTC using the same browser-local timezone
// offset as the quiet and brightness schedules.

#pragma once
#include <stdint.h>

#define ALARM_SLOTS 4

// ---- countdown timer ----
bool timerStart(uint32_t seconds);      // starts/replaces the countdown (1 s .. 24 h)
void timerCancel();                     // stop + release the panel
bool timerActive();                     // counting down, or holding the TIME! screen
uint32_t timerRemaining();              // seconds left (0 while the end screen holds)

// ---- alarms ----
void alarmTick();                       // taskRTC, ~1/s: fire due alarms
void alarmDismiss();                    // stop a ringing alarm (API / any timer op)
bool alarmFiring();

// ---- rendering (taskDisplay only) ----
bool timerAlarmActive();   // an alert wants the panel (checked before the canvas park)
// True if the timer/alarm currently owns the panel; renders one frame when it does.
bool timerAlarmRender();
