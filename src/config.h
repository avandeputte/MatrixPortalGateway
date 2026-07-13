// config.h -- runtime configuration: the GwConfig struct and load/save API.

#ifndef MPGW_CONFIG_H
#define MPGW_CONFIG_H

#include "common.h"

// Runtime configuration; the single instance is the global `cfg`. Defaults are
// set in cfgSetDefaults(); loadConfig()/saveConfig() persist it to the
// "splitflap" NVS namespace (config.cpp). The namespace and key names are
// unchanged from the RS-485 gateway, so a settings backup moves between them.
struct GwConfig {
  char          wifiSSID[64];
  char          wifiPass[64];
  char          mqttHost[64];
  int           mqttPort;
  char          mqttUser[64];
  char          mqttPass[64];
  char          mqttPrefix[32];
  // The emulated bus has no transceiver and no wire, so these four are inert.
  // They are kept because the Settings page, the REST surface and the companion
  // app all still carry them, and so a config from a real Split-Flap Gateway
  // round-trips unchanged. Nothing reads them.
  unsigned long rs485Baud;
  uint8_t       rs485DataBits;
  uint8_t       rs485Parity;
  uint8_t       rs485StopBits;
  char          posixTZ[64];   // POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0"
  char          ntpServer[64]; // NTP server hostname (default pool.ntp.org)
  bool          serialDebug;   // enable verbose serial output
  bool          haEnabled;     // publish Home Assistant MQTT discovery + entity state
  char          otaPassword[32]; // OTA update password (blank = no auth)
  char          hostname[HOSTNAME_MAX]; // blank = derive from the MAC; see cfgHostname()
  // The module grid. On the RS-485 gateway this only laid out the web UI's
  // display wall; here it is also the PHYSICAL wall -- one virtual split-flap
  // module per cell, IDs row-major from 0 -- so changing it changes how many
  // modules exist. Applied on reboot.
  uint8_t       gridRows;      // wall rows (>=1)
  uint8_t       gridCols;      // wall columns (>=1)
  // ---- v3.0 ----
  char          companionUrl[128]; // registered companion-app URL (blank = none)
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
  uint16_t      panelW;        // total chain width in px (64 / 128)
  uint16_t      panelH;        // panel height in px (16 / 32 / 64)
  uint8_t       panelBitDepth; // bitplanes, 1..8 (RAM and refresh rate scale with it)
  bool          panelBGR;      // panel wired BGR, not RGB: swap red and blue on output
  uint8_t       panelBright;   // 1..255, multiplied into every colour before it reaches the panel
  uint16_t      flapMs;        // ms per flap step -- the reel's speed
  uint8_t       flapMax;       // flips drawn for one change, 1..FLAP_ANIM_MAX
  uint32_t      gridColor;     // 0xRRGGBB seam colour drawn around each module cell
  uint8_t       gridBright;    // 0..255 seam intensity; 0 = no grid. Faintness lives here,
                               //   not in panelBright -- see DEFAULT_GRID_BRIGHT. Live.
};

// ---- owned globals (defined in globals.cpp) ----
extern GwConfig cfg;
extern Preferences prefs;

// The effective hostname: cfg.hostname if set, else HOSTNAME_PREFIX-<mac24>. Stable for
// the life of the boot -- mDNS, ArduinoOTA and the AP all latch it at init, so a change
// needs a reboot.
const char* cfgHostname();
// True if `h` is a legal DNS label: 1..31 chars of [a-z0-9-], starting and ending
// alphanumeric. Callers should lowercase first.
bool cfgValidHostname(const char* h);

void cfgSetDefaults();
void loadConfig();
void saveConfig();

#endif // MPGW_CONFIG_H
