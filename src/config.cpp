#include "gateway.h"


// config.cpp -- runtime configuration persisted in NVS (Preferences).
// Defaults live in cfgSetDefaults(); loadConfig()/saveConfig() move the struct
// to and from the "splitflap" NVS namespace. Called from setup() and the
// /api/config/* handlers.
void cfgSetDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.wifiSSID, DEFAULT_WIFI_SSID, sizeof(cfg.wifiSSID));
  strlcpy(cfg.wifiPass, DEFAULT_WIFI_PASS, sizeof(cfg.wifiPass));
  strlcpy(cfg.posixTZ, "UTC0", sizeof(cfg.posixTZ));
  strlcpy(cfg.ntpServer, DEFAULT_NTP_SERVER, sizeof(cfg.ntpServer));
  cfg.gridRows = DEFAULT_GRID_ROWS;
  cfg.gridCols = DEFAULT_GRID_COLS;
  cfg.panelW        = DEFAULT_PANEL_W;
  cfg.panelH        = DEFAULT_PANEL_H;
  cfg.panelBitDepth = DEFAULT_BIT_DEPTH;
  cfg.panelBGR      = DEFAULT_PANEL_BGR;
  cfg.panelBright   = DEFAULT_BRIGHTNESS;
  cfg.fbPsram       = false;
  cfg.flapMs        = DEFAULT_FLAP_MS;
  cfg.flapMax       = DEFAULT_FLAP_MAX;
  cfg.soundEnabled  = true;
  cfg.soundVolume   = 70;
  cfg.tempOffsetC10 = 0;
  cfg.transType     = 0;
  cfg.transMs       = 400;
  cfg.dimEnabled    = false;
  strlcpy(cfg.dimStart, "21:00", sizeof(cfg.dimStart));
  strlcpy(cfg.dimEnd,   "07:00", sizeof(cfg.dimEnd));
  cfg.dimLevel      = 40;
  for (int i = 0; i < 4; i++) {
    strlcpy(cfg.almTime[i], "07:00", sizeof(cfg.almTime[i]));
    cfg.almDays[i] = 0x7F; cfg.almEnabled[i] = false;
  }
  cfg.tapEnabled  = false;
  cfg.backupEnabled = true;
  cfg.hostname[0] = 0;          // blank -> derived from the MAC
  cfg.serialDebug = false;
  gSerialDebug    = false;
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
  cfg.fbPsram       =           prefs.getBool ("fbPsram",false);
  cfg.flapMs        = (uint16_t)prefs.getInt  ("flapMs", DEFAULT_FLAP_MS);
  cfg.flapMax       =           prefs.getUChar("flapMax",DEFAULT_FLAP_MAX);
  cfg.soundEnabled  =           prefs.getBool ("sndEn",  true);
  cfg.soundVolume   =           prefs.getUChar("sndVol", 70);
  cfg.tempOffsetC10 = (int16_t)prefs.getShort("tOff",   0);
  cfg.transType     =           prefs.getUChar("trType", 0);
  cfg.dimEnabled    =           prefs.getBool ("dimEn",  false);
  strlcpy(cfg.dimStart,         prefs.getString("dimStart", "21:00").c_str(), sizeof(cfg.dimStart));
  strlcpy(cfg.dimEnd,           prefs.getString("dimEnd",   "07:00").c_str(), sizeof(cfg.dimEnd));
  cfg.dimLevel      =           prefs.getUChar("dimLvl", 40);
  if (cfg.dimLevel < 1) cfg.dimLevel = 40;
  cfg.tapEnabled  = prefs.getBool("tapEn",  false);
  cfg.backupEnabled = prefs.getBool("bakEn", true);
  for (int i = 0; i < 4; i++) {
    char k[8];
    snprintf(k, sizeof(k), "almT%d", i);
    strlcpy(cfg.almTime[i], prefs.getString(k, "07:00").c_str(), sizeof(cfg.almTime[i]));
    snprintf(k, sizeof(k), "almD%d", i); cfg.almDays[i]    = prefs.getUChar(k, 0x7F);
    snprintf(k, sizeof(k), "almE%d", i); cfg.almEnabled[i] = prefs.getBool(k, false);
  }
  cfg.transMs       = (uint16_t)prefs.getShort("trMs",   400);
  if (cfg.panelBitDepth < 1 || cfg.panelBitDepth > 6) cfg.panelBitDepth = DEFAULT_BIT_DEPTH;
  if (cfg.panelBright < 1) cfg.panelBright = DEFAULT_BRIGHTNESS;
  if (cfg.flapMs < 2 || cfg.flapMs > 500) cfg.flapMs = DEFAULT_FLAP_MS;
  if (cfg.flapMax < 1 || cfg.flapMax > FLAP_ANIM_MAX) cfg.flapMax = DEFAULT_FLAP_MAX;
  if (cfg.soundVolume > 100) cfg.soundVolume = 70;
  if (cfg.tempOffsetC10 < -300 || cfg.tempOffsetC10 > 300) cfg.tempOffsetC10 = 0;
  if (cfg.transType > 3) cfg.transType = 0;
  if (cfg.transMs < 100 || cfg.transMs > 2000) cfg.transMs = 400;
  prefs.getString("host", cfg.hostname, sizeof(cfg.hostname));
  if (cfg.hostname[0] && !cfgValidHostname(cfg.hostname)) cfg.hostname[0] = 0;
  cfg.serialDebug = prefs.getBool("dbgSerial", false);
  gSerialDebug    = cfg.serialDebug;
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
  prefs.putString("tz",     cfg.posixTZ);
  prefs.putBool  ("dbgSerial", cfg.serialDebug);
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
  prefs.putBool  ("fbPsram",  cfg.fbPsram);
  prefs.putInt   ("flapMs",   cfg.flapMs);
  prefs.putUChar ("flapMax",  cfg.flapMax);
  prefs.putBool  ("sndEn",    cfg.soundEnabled);
  prefs.putUChar ("sndVol",   cfg.soundVolume);
  prefs.putShort ("tOff",     cfg.tempOffsetC10);
  prefs.putUChar ("trType",   cfg.transType);
  prefs.putBool  ("dimEn",    cfg.dimEnabled);
  prefs.putString("dimStart", cfg.dimStart);
  prefs.putString("dimEnd",   cfg.dimEnd);
  prefs.putUChar ("dimLvl",   cfg.dimLevel);
  prefs.putBool  ("tapEn",    cfg.tapEnabled);
  prefs.putBool  ("bakEn",    cfg.backupEnabled);
  for (int i = 0; i < 4; i++) {
    char k[8];
    snprintf(k, sizeof(k), "almT%d", i); prefs.putString(k, cfg.almTime[i]);
    snprintf(k, sizeof(k), "almD%d", i); prefs.putUChar (k, cfg.almDays[i]);
    snprintf(k, sizeof(k), "almE%d", i); prefs.putBool  (k, cfg.almEnabled[i]);
  }
  prefs.putShort ("trMs",     cfg.transMs);
  prefs.putString("host",     cfg.hostname);
  prefs.end();
}

// ---- settings export/import (v3.16) ---------------------------------------

static bool okHHMM(const char* s) {
  int h = -1, m = -1;
  return s && sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

void cfgExportJson(JsonDocument& doc) {
  doc["product"]   = PRODUCT_NAME;
  doc["fwVersion"] = FW_VERSION;
  doc["exported"]  = (uint32_t)time(nullptr);
  doc["wifiSSID"]  = cfg.wifiSSID;          // no wifiPass -- see config.h
  doc["posixTZ"]   = cfg.posixTZ;
  doc["ntpServer"] = cfg.ntpServer;
  doc["serialDebug"] = cfg.serialDebug;
  doc["hostname"]  = cfg.hostname;
  doc["gridRows"]  = cfg.gridRows;
  doc["gridCols"]  = cfg.gridCols;
  doc["bootAnim"]  = cfg.bootAnim;
  doc["quietSchedEnabled"] = cfg.quietSchedEnabled;
  doc["quietStart"] = cfg.quietStart;
  doc["quietEnd"]   = cfg.quietEnd;
  doc["quietDays"]  = cfg.quietDays;
  doc["quietTzOffsetMin"] = cfg.quietTzOffsetMin;
  doc["panelW"]        = cfg.panelW;
  doc["panelH"]        = cfg.panelH;
  doc["panelBitDepth"] = cfg.panelBitDepth;
  doc["panelBGR"]      = cfg.panelBGR;
  doc["panelBright"]   = cfg.panelBright;
  doc["fbPsram"]       = cfg.fbPsram;
  doc["flapMs"]        = cfg.flapMs;
  doc["flapMax"]       = cfg.flapMax;
  doc["soundEnabled"]  = cfg.soundEnabled;
  doc["soundVolume"]   = cfg.soundVolume;
  doc["tempOffsetC10"] = cfg.tempOffsetC10;
  doc["transType"]     = cfg.transType;
  doc["transMs"]       = cfg.transMs;
  doc["dimEnabled"] = cfg.dimEnabled;
  doc["dimStart"]   = cfg.dimStart;
  doc["dimEnd"]     = cfg.dimEnd;
  doc["dimLevel"]   = cfg.dimLevel;
  JsonArray al = doc["alarms"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    JsonObject a = al.add<JsonObject>();
    a["time"] = cfg.almTime[i]; a["days"] = cfg.almDays[i]; a["enabled"] = cfg.almEnabled[i];
  }
  doc["tapEnabled"]    = cfg.tapEnabled;
  doc["backupEnabled"] = cfg.backupEnabled;
}

// Every key optional; strings length-clamped by strlcpy, numerics by the same
// rules loadConfig() enforces. Malformed values are skipped, not fatal: the rest
// of the file still applies (and `applied` tells the caller how much did).
bool cfgImportJson(const JsonDocument& doc, int& applied, bool& rebootNeeded) {
  applied = 0;
  const uint16_t oW = cfg.panelW, oH = cfg.panelH;
  const uint8_t  oD = cfg.panelBitDepth, oR = cfg.gridRows, oC = cfg.gridCols;
  const bool     oP = cfg.fbPsram;
  char oHost[HOSTNAME_MAX]; strlcpy(oHost, cfg.hostname, sizeof(oHost));

  #define IMP_STR(key, field) if (doc[key].is<const char*>()) { strlcpy(field, doc[key].as<const char*>(), sizeof(field)); applied++; }
  #define IMP_BOOL(key, field) if (doc[key].is<bool>()) { field = doc[key].as<bool>(); applied++; }
  #define IMP_NUM(key, field, lo, hi) if (doc[key].is<int>()) { long v = doc[key].as<long>(); if (v >= (lo) && v <= (hi)) { field = v; applied++; } }
  #define IMP_HHMM(key, field) if (doc[key].is<const char*>() && okHHMM(doc[key].as<const char*>())) { strlcpy(field, doc[key].as<const char*>(), sizeof(field)); applied++; }

  IMP_STR ("wifiSSID",  cfg.wifiSSID);
  IMP_STR ("posixTZ",   cfg.posixTZ);
  IMP_STR ("ntpServer", cfg.ntpServer);
  IMP_BOOL("serialDebug", cfg.serialDebug);
  if (doc["hostname"].is<const char*>()) {
    const char* h = doc["hostname"].as<const char*>();
    if (!h[0] || cfgValidHostname(h)) { strlcpy(cfg.hostname, h, sizeof(cfg.hostname)); applied++; }
  }
  IMP_NUM ("gridRows", cfg.gridRows, 1, 64);
  IMP_NUM ("gridCols", cfg.gridCols, 1, 64);
  IMP_STR ("bootAnim", cfg.bootAnim);
  IMP_BOOL("quietSchedEnabled", cfg.quietSchedEnabled);
  IMP_HHMM("quietStart", cfg.quietStart);
  IMP_HHMM("quietEnd",   cfg.quietEnd);
  IMP_NUM ("quietDays", cfg.quietDays, 0, 0x7F);
  IMP_NUM ("quietTzOffsetMin", cfg.quietTzOffsetMin, -14*60, 14*60);
  IMP_NUM ("panelW", cfg.panelW, 32, 256);
  IMP_NUM ("panelH", cfg.panelH, 16, 64);
  IMP_NUM ("panelBitDepth", cfg.panelBitDepth, 1, 6);
  IMP_BOOL("panelBGR", cfg.panelBGR);
  IMP_NUM ("panelBright", cfg.panelBright, 1, 255);
  IMP_BOOL("fbPsram", cfg.fbPsram);
  IMP_NUM ("flapMs", cfg.flapMs, 2, 500);
  IMP_NUM ("flapMax", cfg.flapMax, 1, FLAP_ANIM_MAX);
  IMP_BOOL("soundEnabled", cfg.soundEnabled);
  IMP_NUM ("soundVolume", cfg.soundVolume, 0, 100);
  IMP_NUM ("tempOffsetC10", cfg.tempOffsetC10, -300, 300);
  IMP_NUM ("transType", cfg.transType, 0, 3);
  IMP_NUM ("transMs", cfg.transMs, 100, 2000);
  IMP_BOOL("dimEnabled", cfg.dimEnabled);
  IMP_HHMM("dimStart", cfg.dimStart);
  IMP_HHMM("dimEnd",   cfg.dimEnd);
  IMP_NUM ("dimLevel", cfg.dimLevel, 1, 255);
  if (doc["alarms"].is<JsonArrayConst>()) {
    int i = 0;
    for (JsonVariantConst a : doc["alarms"].as<JsonArrayConst>()) {
      if (i >= 4) break;
      if (a["time"].is<const char*>() && okHHMM(a["time"].as<const char*>()))
        strlcpy(cfg.almTime[i], a["time"].as<const char*>(), sizeof(cfg.almTime[i]));
      cfg.almDays[i]    = (uint8_t)((a["days"] | (int)cfg.almDays[i]) & 0x7F);
      cfg.almEnabled[i] = a["enabled"] | cfg.almEnabled[i];
      i++;
    }
    if (i) applied++;
  }
  IMP_BOOL("tapEnabled",  cfg.tapEnabled);
  IMP_BOOL("backupEnabled", cfg.backupEnabled);

  #undef IMP_STR
  #undef IMP_BOOL
  #undef IMP_NUM
  #undef IMP_HHMM

  if (!applied) return false;
  gSerialDebug = cfg.serialDebug;
  strlcpy(gPosixTZ, cfg.posixTZ, sizeof(gPosixTZ));
  cfgApplyTZ();
  saveConfig();
  rebootNeeded = (oW != cfg.panelW || oH != cfg.panelH || oD != cfg.panelBitDepth ||
                  oP != cfg.fbPsram || oR != cfg.gridRows || oC != cfg.gridCols ||
                  strcmp(oHost, cfg.hostname) != 0);
  return true;
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

// Cached: mDNS and the SoftAP latch this once at init, so changing
// cfg.hostname takes effect on the next boot. Deriving from the eFuse MAC keeps it
// stable across reflashes and unique across boards -- the same 32-bit value the (removed) MQTT
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
