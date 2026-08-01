#include "gateway.h"
#include "timer.h"
#include "panel.h"
#include "effects.h"   // aaTextDraw: the big Orbitron digits
#include "sound.h"
#include "rtc.h"

// timer.cpp -- see timer.h.

// ---- countdown state (written from httpd, read on taskDisplay/taskRTC) ----
static volatile uint32_t timerEndMs   = 0;      // 0 = no timer
static volatile uint32_t timerDoneMs  = 0;      // when it hit zero (end screen holds ~10 s)
static volatile bool     alarmRinging = false;
static volatile uint32_t alarmSinceMs = 0;
static char              alarmLabel[6] = "";    // the "HH:MM" that fired, for the screen

bool timerActive()   { return timerEndMs != 0 || timerDoneMs != 0; }
bool alarmFiring()   { return alarmRinging; }
uint32_t timerRemaining() {
  const uint32_t end = timerEndMs;
  if (!end) return 0;
  const uint32_t now = millis();
  return (int32_t)(end - now) > 0 ? (end - now) / 1000u : 0;
}

bool timerStart(uint32_t seconds) {
  if (seconds < 1 || seconds > 24u * 3600u) return false;
  timerDoneMs = 0;
  timerEndMs  = millis() + seconds * 1000u;
  // A canvas takeover parks taskDisplay before the timer renderer runs -- evict it so
  // the countdown actually shows. Safe from any task (see dispReturnToWall).
  dispReturnToWall();
  return true;
}

void timerCancel() {
  timerEndMs = 0; timerDoneMs = 0;
  soundStop();
  dispMarkDirty();                       // repaint whatever mode resumes underneath
}

bool timerAlarmGestureDismiss() {
  if (alarmRinging) { alarmDismiss(); return true; }
  if (timerEndMs || timerDoneMs) { timerCancel(); return true; }
  return false;
}

void alarmDismiss() {
  alarmRinging = false;
  soundStop();
  dispMarkDirty();
}

// The chime: three rising notes, played force=true so Quiet Time cannot mute an alert
// the user explicitly scheduled. The speaker master enable still applies (soundPlay
// checks it) -- a deliberately muted speaker leaves the alert visual-only.
static void alertChime() {
  static const uint16_t f[] = { 880, 1175, 1568 };
  static const uint16_t m[] = { 120, 120, 320 };
  soundPlay(f, m, 3, (uint8_t)(60 * cfg.soundVolume / 100), true);
}

/* ---- alarms (taskRTC, ~1/s) -------------------------------------------------------
   Same local-time mechanics as the quiet/dim schedules: user-local "HH:MM" plus the
   shared browser tz offset. Each slot fires once per matching minute (lastFired
   guards re-triggering within it). A ring self-stops after 90 s undismissed. */
void alarmTick() {
  if (alarmRinging) {
    if (millis() - alarmSinceMs > 90000u) alarmDismiss();   // nobody home: give up
    else if ((millis() - alarmSinceMs) % 2000u < 60u) alertChime();   // re-ring ~2 s
    return;
  }
  const uint32_t utc = rtcEpochNow();
  if (!utc) return;
  time_t local = (time_t)utc + (time_t)cfg.quietTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&local, &lt);
  static int lastFiredMin = -1;                  // minute-of-day already fired
  const int curMin = lt.tm_hour * 60 + lt.tm_min;
  if (curMin == lastFiredMin) return;
  for (int i = 0; i < ALARM_SLOTS; i++) {
    if (!cfg.almEnabled[i]) continue;
    if (!((cfg.almDays[i] >> lt.tm_wday) & 1)) continue;
    int h = -1, m = -1;
    sscanf(cfg.almTime[i], "%d:%d", &h, &m);
    if (h * 60 + m != curMin) continue;
    lastFiredMin = curMin;
    strlcpy(alarmLabel, cfg.almTime[i], sizeof(alarmLabel));
    alarmSinceMs = millis();
    alarmRinging = true;
    dispReturnToWall();                          // evict canvas/effects: the alert must show
    alertChime();
    return;
  }
  lastFiredMin = -1;                             // new minute, nothing due: re-arm
}

/* ---- rendering (taskDisplay) ------------------------------------------------------ */
// Vertically centre on the DIGITS' real cap height: aaTextDraw offsets its y by the
// face ascent, which overstates the ink box -- naive size/2 maths drew the countdown
// low enough to clip (found on the wall, v3.14.0 bring-up).
static void centreTextV(int centreY, int size, const char* s, uint8_t r, uint8_t g, uint8_t b) {
  int asc = 0, cap = 0;
  aaTextMetrics(size, &asc, &cap);
  const int yTop = centreY - cap / 2;            // where the ink should start
  aaTextDraw(gPanel.panelW / 2, yTop + cap - asc, size, s, 1, r, g, b);
}

bool timerAlarmActive() { return alarmRinging || timerEndMs != 0 || timerDoneMs != 0; }

bool timerAlarmRender() {
  const int W = gPanel.panelW, H = gPanel.panelH;
  // Re-evict a canvas takeover every frame: the companion repaints within seconds of
  // dispReturnToWall, and the alert must stay on top for its whole life.
  if (gCanvasMode) dispReturnToWall();

  if (alarmRinging) {
    const bool flash = (millis() / 500) & 1;     // 1 Hz flash
    panelClear();
    if (flash) panelFillRect(0, 0, W, 2, 255, 40, 40), panelFillRect(0, H - 2, W, 2, 255, 40, 40);
    const int big = H >= 64 ? 34 : 24;
    centreTextV(H >= 64 ? H / 2 - 8 : H / 2 - 5, big, alarmLabel, 255, flash ? 60 : 200, 40);
    if (H >= 48) centreTextV(H - 9, 10, "ALARM", 255, 220, 220);
    panelShow();
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
  }

  if (timerDoneMs) {                             // TIME! screen holds ~10 s, chiming
    if (millis() - timerDoneMs > 10000u) { timerDoneMs = 0; dispMarkDirty(); return false; }
    const bool flash = (millis() / 400) & 1;
    panelClear();
    const int big = H >= 64 ? 34 : 24;
    centreTextV(H / 2, big, "00:00", flash ? 255 : 120, flash ? 200 : 60, 0);
    panelShow();
    if ((millis() - timerDoneMs) % 2500u < 60u) alertChime();
    vTaskDelay(pdMS_TO_TICKS(80));
    return true;
  }

  if (!timerEndMs) return false;
  const uint32_t now = millis();
  if ((int32_t)(timerEndMs - now) <= 0) {        // just hit zero
    timerEndMs = 0;
    timerDoneMs = now;
    alertChime();
    return true;
  }
  const uint32_t left = (timerEndMs - now + 999) / 1000u;   // ceil: shows 00:01 through the last second
  char txt[12];
  if (left >= 3600) snprintf(txt, sizeof(txt), "%lu:%02lu:%02lu",
                             (unsigned long)(left / 3600), (unsigned long)((left / 60) % 60),
                             (unsigned long)(left % 60));
  else              snprintf(txt, sizeof(txt), "%02lu:%02lu",
                             (unsigned long)(left / 60), (unsigned long)(left % 60));
  panelClear();
  const bool urgent = left <= 10;
  const int big = (H >= 64 ? 34 : 24);
  centreTextV(H / 2, big, txt,
              255, urgent ? ((millis() / 300) & 1 ? 60 : 200) : 244, urgent ? 40 : 224);
  panelShow();
  vTaskDelay(pdMS_TO_TICKS(150));
  return true;
}
