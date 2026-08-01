// config.h -- runtime configuration: the GwConfig struct and load/save API.

#ifndef MPGW_CONFIG_H
#define MPGW_CONFIG_H

#include "common.h"

// Runtime configuration; the single instance is the global `cfg`. Defaults are
// set in cfgSetDefaults(); loadConfig()/saveConfig() persist it to the
// "splitflap" NVS namespace (config.cpp). Key names are stable so upgrades keep
// settings; they carry no cross-product portability promise.
struct GwConfig {
  char          wifiSSID[64];
  char          wifiPass[64];
  char          posixTZ[64];   // POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0"
  char          ntpServer[64]; // NTP server hostname (default pool.ntp.org)
  bool          serialDebug;   // enable verbose serial output
  char          hostname[HOSTNAME_MAX]; // blank = derive from the MAC; see cfgHostname()
  // The module grid. On the physical gateway this only laid out the web UI's
  // display wall; here it is also the PHYSICAL wall -- one virtual split-flap
  // module per cell, IDs row-major from 0 -- so changing it changes how many
  // modules exist. Applied on reboot.
  uint8_t       gridRows;      // wall rows (>=1)
  uint8_t       gridCols;      // wall columns (>=1)
  // ---- v3.0 ----
  char          companionUrl[128]; // registered companion-app URL (blank = none)
  char          bootAnim[25];      // animation library name autoplayed at boot (blank = none)
  bool          quietSchedEnabled; // auto-enable Quiet Time on a daily schedule
  char          quietStart[6];     // quiet window start "HH:MM" (the user's LOCAL time)
  char          quietEnd[6];       // quiet window end   "HH:MM" (the user's LOCAL time)
  uint8_t       quietDays;         // active-day bitmask, bit0=Sun .. bit6=Sat (local)
  int16_t       quietTzOffsetMin;  // minutes EAST of UTC for the schedule (browser-supplied);
                                   // local = UTC + this. Independent of the gateway posixTZ so the
                                   // user just enters their own local time. See quietScheduleTick.
  // ---- HUB75 panel (Matrix Portal Gateway) ----
  // The driver takes its geometry and bit depth at construction, so the first three
  // only take effect on reboot. panelBright, flapMs and flapMax are live.
  uint16_t      panelW;        // total chain width in px (32..256, multiple of 32)
  uint16_t      panelH;        // panel height in px (16 / 32 / 64)
  uint8_t       panelBitDepth; // bitplanes, 1..6 (RAM and refresh rate scale with it)
  bool          panelBGR;      // panel wired BGR, not RGB: swap red and blue on output
  uint8_t       panelBright;   // 1..255, multiplied into every colour before it reaches the panel
  bool          fbPsram;       // v3.11 (experimental): framebuffer in octal PSRAM, not internal
                               // SRAM -- lifts the internal-RAM depth cap so 256x64 can run depth 4+
  uint16_t      flapMs;        // ms per flap step -- the reel's speed
  uint8_t       flapMax;       // flips drawn for one change, 1..FLAP_ANIM_MAX
  bool          soundEnabled;  // master speaker enable (v3.6); false = silent
  uint8_t       soundVolume;   // master volume 0..100 -- scales every /api/sound call
  int16_t       tempOffsetC10; // SHTC3 temperature calibration, TENTHS of degC (v3.7);
                               // added to the raw reading to correct board self-heating
  uint8_t       transType;     // canvas full-frame transition (v3.7.2): 0 none 1 crossfade 2 wipe 3 slide
  uint16_t      transMs;       // transition duration, 100..2000 ms
  // Brightness schedule (v3.13): auto-dim the panel in a daily window (e.g. evenings).
  // Times are the user's LOCAL "HH:MM" and share quietTzOffsetMin with the quiet
  // schedule (one browser, one offset). Quiet Time takes precedence (it blanks).
  bool          dimEnabled;
  char          dimStart[6];   // window start "HH:MM"
  char          dimEnd[6];     // window end   "HH:MM" (may wrap past midnight)
  uint8_t       dimLevel;      // 1..255 panel brightness while the window is active
  // Daily alarms (v3.14): up to 4 slots, user-local "HH:MM" sharing quietTzOffsetMin.
  // Alarms override Quiet Time by design.
  char          almTime[4][6];
  uint8_t       almDays[4];    // day bitmask, bit0=Sun .. bit6=Sat
  bool          almEnabled[4];
};

// ---- owned globals (defined in globals.cpp) ----
extern GwConfig cfg;
extern Preferences prefs;

// The effective hostname: cfg.hostname if set, else HOSTNAME_PREFIX-<mac24>. Stable for
// the life of the boot -- mDNS and the AP latch it at init, so a change
// needs a reboot.
const char* cfgHostname();
// True if `h` is a legal DNS label: 1..31 chars of [a-z0-9-], starting and ending
// alphanumeric. Callers should lowercase first.
bool cfgValidHostname(const char* h);

void cfgSetDefaults();
void loadConfig();
void saveConfig();

#endif // MPGW_CONFIG_H
