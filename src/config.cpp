#include "gateway.h"


// config.cpp -- runtime configuration persisted in NVS (Preferences).
// Defaults live in cfgSetDefaults(); loadConfig()/saveConfig() move the struct
// to and from the "splitflap" NVS namespace. Called from setup() and the
// /api/config/* handlers.
void cfgSetDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.wifiSSID, DEFAULT_WIFI_SSID, sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, DEFAULT_WIFI_PASS, sizeof(cfg.wifiPass));
  cfg.mqttPort      = DEFAULT_MQTT_PORT;
  strlcpy(cfg.mqttPrefix, DEFAULT_MQTT_PREFIX, sizeof(cfg.mqttPrefix));
  strlcpy(cfg.posixTZ, "UTC0", sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
  cfg.gridRows = DEFAULT_GRID_ROWS;
  cfg.gridCols = DEFAULT_GRID_COLS;
  cfg.panelW        = DEFAULT_PANEL_W;
  cfg.panelH        = DEFAULT_PANEL_H;
  cfg.panelBitDepth = DEFAULT_BIT_DEPTH;
  cfg.panelBGR      = DEFAULT_PANEL_BGR;
  cfg.panelBright   = DEFAULT_BRIGHTNESS;
  cfg.flapMs        = DEFAULT_FLAP_MS;
  cfg.flapMax       = DEFAULT_FLAP_MAX;
  cfg.hostname[0] = 0;          // blank -> derived from the MAC
  cfg.serialDebug = false;
  gSerialDebug    = false;
  cfg.haEnabled   = false;
  strlcpy(cfg.otaPassword, "", sizeof(cfg.otaPassword));
  strlcpy(gPosixTZ,    "UTC0", sizeof(gPosixTZ));
  // v3.0 defaults
  strlcpy(cfg.companionUrl, "", sizeof(cfg.companionUrl));
  strlcpy(cfg.bootAnim, "", sizeof(cfg.bootAnim));
  cfg.quietSchedEnabled = false;
  strlcpy(cfg.quietStart, "22:00", sizeof(cfg.quietStart));
  strlcpy(cfg.quietEnd,   "07:00", sizeof(cfg.quietEnd));
  cfg.quietDays = 0x7F;  // all days
  cfg.quietTzOffsetMin = 0;   // captured from the browser on Save Schedule
}
void loadConfig() {
  prefs.begin("splitflap", true);
  // The compile-time credentials are the default ONLY for a key that has never
  // been written. Preferences returns a stored empty string as an empty string,
  // so once anything is saved from the Settings page -- including a deliberately
  // blank SSID -- NVS wins and the baked-in network is never reinstated.
  strlcpy(cfg.wifiSSID,   prefs.getString("wSSID",  DEFAULT_WIFI_SSID).c_str(), sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass,   prefs.getString("wPASS",  DEFAULT_WIFI_PASS).c_str(), sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost,   prefs.getString("mqHost", "").c_str(), sizeof(cfg.mqttHost));
  cfg.mqttPort          = prefs.getInt   ("mqPort",  DEFAULT_MQTT_PORT);
  strlcpy(cfg.mqttUser,   prefs.getString("mqUser", "").c_str(), sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,   prefs.getString("mqPass", "").c_str(), sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, prefs.getString("mqPfx",  DEFAULT_MQTT_PREFIX).c_str(), sizeof(cfg.mqttPrefix));
  strlcpy(cfg.posixTZ, prefs.getString("tz", "UTC0").c_str(), sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, prefs.getString("ntp", DEFAULT_NTP_SERVER).c_str(), sizeof(cfg.ntpServer));
  cfg.gridRows = (uint8_t)prefs.getInt("gRows", DEFAULT_GRID_ROWS);
  cfg.gridCols = (uint8_t)prefs.getInt("gCols", DEFAULT_GRID_COLS);
  if (cfg.gridRows < 1) cfg.gridRows = 1;
  if (cfg.gridCols < 1) cfg.gridCols = 1;
  cfg.panelW        = (uint16_t)prefs.getInt  ("pW",     DEFAULT_PANEL_W);
  cfg.panelH        = (uint16_t)prefs.getInt  ("pH",     DEFAULT_PANEL_H);
  cfg.panelBitDepth =           prefs.getUChar("pDepth", DEFAULT_BIT_DEPTH);
  cfg.panelBGR      =           prefs.getBool ("pBGR",   DEFAULT_PANEL_BGR);
  cfg.panelBright   =           prefs.getUChar("pBright",DEFAULT_BRIGHTNESS);
  cfg.flapMs        = (uint16_t)prefs.getInt  ("flapMs", DEFAULT_FLAP_MS);
  cfg.flapMax       =           prefs.getUChar("flapMax",DEFAULT_FLAP_MAX);
  if (cfg.panelBitDepth < 1 || cfg.panelBitDepth > 6) cfg.panelBitDepth = DEFAULT_BIT_DEPTH;
  if (cfg.panelBright < 1) cfg.panelBright = DEFAULT_BRIGHTNESS;
  if (cfg.flapMs < 2 || cfg.flapMs > 500) cfg.flapMs = DEFAULT_FLAP_MS;
  if (cfg.flapMax < 1 || cfg.flapMax > FLAP_ANIM_MAX) cfg.flapMax = DEFAULT_FLAP_MAX;
  prefs.getString("host", cfg.hostname, sizeof(cfg.hostname));
  if (cfg.hostname[0] && !cfgValidHostname(cfg.hostname)) cfg.hostname[0] = 0;
  cfg.serialDebug = prefs.getBool("dbgSerial", false);
  gSerialDebug    = cfg.serialDebug;
  cfg.haEnabled   = prefs.getBool("haEnabled", false);
  strlcpy(cfg.otaPassword, prefs.getString("otaPass", "").c_str(), sizeof(cfg.otaPassword));
  // v3.0
  strlcpy(cfg.companionUrl, prefs.getString("compUrl", "").c_str(), sizeof(cfg.companionUrl));
  strlcpy(cfg.bootAnim, prefs.getString("bAnim", "").c_str(), sizeof(cfg.bootAnim));
  cfg.quietSchedEnabled = prefs.getBool("qsEn", false);
  strlcpy(cfg.quietStart, prefs.getString("qsStart", "22:00").c_str(), sizeof(cfg.quietStart));
  strlcpy(cfg.quietEnd,   prefs.getString("qsEnd",   "07:00").c_str(), sizeof(cfg.quietEnd));
  cfg.quietDays = prefs.getUChar("qsDays", 0x7F);
  cfg.quietTzOffsetMin = prefs.getShort("qsTzOff", 0);
  strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
  cfgApplyTZ();
  prefs.end();
}

void saveConfig() {
  prefs.begin("splitflap", false);
  prefs.putString("wSSID",  cfg.wifiSSID);
  prefs.putString("ntp",    cfg.ntpServer);
  prefs.putInt   ("gRows",  cfg.gridRows);
  prefs.putInt   ("gCols",  cfg.gridCols);
  prefs.putString("wPASS",  cfg.wifiPass);
  prefs.putString("mqHost", cfg.mqttHost);
  prefs.putInt   ("mqPort", cfg.mqttPort);
  prefs.putString("mqUser", cfg.mqttUser);
  prefs.putString("mqPass", cfg.mqttPass);
  prefs.putString("mqPfx",  cfg.mqttPrefix);
  prefs.putString("tz",     cfg.posixTZ);
  prefs.putBool  ("dbgSerial", cfg.serialDebug);
  prefs.putBool  ("haEnabled", cfg.haEnabled);
  prefs.putString("otaPass",   cfg.otaPassword);
  // v3.0
  prefs.putString("compUrl",   cfg.companionUrl);
  prefs.putString("bAnim",     cfg.bootAnim);
  prefs.putBool  ("qsEn",      cfg.quietSchedEnabled);
  prefs.putString("qsStart",   cfg.quietStart);
  prefs.putString("qsEnd",     cfg.quietEnd);
  prefs.putUChar ("qsDays",    cfg.quietDays);
  prefs.putShort ("qsTzOff",   cfg.quietTzOffsetMin);
  // panel
  prefs.putInt   ("pW",       cfg.panelW);
  prefs.putInt   ("pH",       cfg.panelH);
  prefs.putUChar ("pDepth",   cfg.panelBitDepth);
  prefs.putBool  ("pBGR",     cfg.panelBGR);
  prefs.putUChar ("pBright",  cfg.panelBright);
  prefs.putInt   ("flapMs",   cfg.flapMs);
  prefs.putUChar ("flapMax",  cfg.flapMax);
  prefs.putString("host",     cfg.hostname);
  prefs.end();
}

// ---- identity -------------------------------------------------------------

bool cfgValidHostname(const char* h) {
  if (!h) return false;
  size_t n = strlen(h);
  if (n < 1 || n >= HOSTNAME_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && c != '-') return false;
    if (c == '-' && (i == 0 || i == n - 1)) return false;   // no leading/trailing hyphen
  }
  return true;
}

// Cached: mDNS, ArduinoOTA and the SoftAP all latch this once at init, so changing
// cfg.hostname takes effect on the next boot. Deriving from the eFuse MAC keeps it
// stable across reflashes and unique across boards -- the same 32-bit value the MQTT
// client id and the Home Assistant node id already use.
const char* cfgHostname() {
  static char host[HOSTNAME_MAX];
  if (host[0]) return host;
  if (cfg.hostname[0] && cfgValidHostname(cfg.hostname)) {
    strlcpy(host, cfg.hostname, sizeof(host));
  } else {
    snprintf(host, sizeof(host), HOSTNAME_PREFIX "-%06lx", (unsigned long)boardId24());
  }
  return host;
}
