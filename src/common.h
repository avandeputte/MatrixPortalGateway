#ifndef MPGW_COMMON_H
#define MPGW_COMMON_H

/*
 * Matrix Portal Gateway
 * Firmware for the Waveshare ESP32-S3-RGB-Matrix driver board driving a HUB75
 * RGB LED matrix. (Originally built on the Adafruit MatrixPortal S3; that
 * board's final version, v1.25.0, lives on the `matrixportal` git branch.)
 *
 * A port of the Split-Flap Gateway (v3.1) that keeps the entire gateway -- web UI,
 * REST API, MQTT/Home Assistant, OTA, command monitor -- and replaces
 * the physical gateway's serial transceiver with an in-process frame link and
 * a panel full of *virtual* split-flap modules. Nothing is wired to a real reel: every module the gateway
 * drives is emulated in firmware and drawn as a flap cell on the LED matrix.
 *
 * The emulation is at the PROTOCOL level, not the API level. Commands are framed,
 * sanitized and "transmitted" exactly as before; the virtual modules parse those
 * bytes and reply with real protocol frames, staggered the way the physical
 * modules' replies would be. The gateway cannot tell the difference, so the
 * companion app and any MQTT client keep working unchanged.
 *
 * Copyright (c) 2026 Alex Van de Putte
 *
 * Licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 * International License (CC BY-NC-SA 4.0):
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 *
 * Split-flap module hardware and the initial protocol by Adam G Makes
 * (YouTube: https://www.youtube.com/@AdamGMakes).
 *
 * Emulated protocol (module firmware v31, backward-compatible to v6):
 *   Frame format: m<ADDR><CMD>[data]\n
 *   Address:      decimal (zero-padded 2-digit v6 style, or variable-length v7+)
 *                 broadcast: m** (v6) or m* (v7+), optional range m*v<lo>-<hi>
 *                 by serial: mX...
 *   Commands the virtual modules ACT ON (vmDispatch):
 *                 m<id>-<char>    display character
 *                 m<id>+<n>       display by flap index
 *                 m<id>h          home
 *   Nothing replies. The physical protocol's query commands ('v' version,
 *   'A' all-fields) and by-serial addressing were removed in v1.24 -- no
 *   client ever sent them, and the wall self-describes through
 *   /api/capabilities and /api/display/state. The calibration/dump grammar
 *   is likewise unmodelled. All such frames pass the sanitizer untrimmed and
 *   the virtual modules silently ignore them, like any unknown command.
 *
 * Board: Waveshare ESP32-S3-RGB-Matrix (32MB octal flash @1.8V, 16MB octal
 *        PSRAM, PCF85063 battery RTC, TF slot, QMI8658 IMU, ES8311 audio --
 *        of which this firmware uses the RTC; the rest is future headroom)
 * Libraries: none for the panel -- see panel.cpp, a direct LCD_CAM + GDMA driver.
 *            PubSubClient, ArduinoJson.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FFat.h>
#include <ESPmDNS.h>
#include <esp_mac.h>

/* ---- This board's unique id -------------------------------------------------------
   Derived from the NIC-specific half of the MAC (bytes 3..5) -- the only part that
   actually differs between two boards.

   NOT ESP.getEfuseMac() masked, which is the trap this exists to close.
   esp_efuse_mac_get_default() writes the six MAC bytes in NETWORK order, and Arduino's
   EspClass reads that same buffer straight back as a LITTLE-endian uint64 -- so the LOW
   bytes of the value it returns are the OUI (48:27:e2), which is identical on every
   Espressif chip ever made. Masking the low 24 or 32 bits therefore hands EVERY board
   the same "unique" id.

   That is not theoretical. Two of these gateways on one LAN both derived the hostname
   splitflap-gw-e22748 and both connected to MQTT as splitflap-20E22748 -- and a broker
   evicts the client already holding a duplicate id, so the pair knocked each other
   offline in a loop, forever.                                                          */
static inline uint32_t boardId24() {          // 6 hex digits -- hostname suffix
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  return ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | (uint32_t)m[5];
}
static inline uint32_t boardId32() {          // 8 hex digits -- MQTT client id, HA node id
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16)
       | ((uint32_t)m[4] << 8)  | (uint32_t)m[5];
}

/* ============================================================================
 *  BOARD / BUILD CONFIGURATION
 *  ----------------------------------------------------------------------------
 *  Everything needed to retarget this firmware to a different board, panel, or
 *  default setup lives in this single block.
 *
 *  Default config is for the Waveshare ESP32-S3-RGB-Matrix driver board.
 * ==========================================================================*/

/* ---- HUB75 matrix pins ----
   Waveshare's own map for this board (shipped in their patched
   ESP32-HUB75-MatrixPanel-DMA example: esp32s3-default-pins.hpp + the .ino's
   gpio.e override). The octal PSRAM consumes GPIO 33-37, so nothing here may
   touch that range -- and none of these collide with the board's I2C (47/48),
   I2S audio (12/21/38/39/43), SD (1/14/17/44) or BOOT button (0).

   The address list carries all five lines; the panel height decides how many
   are actually clocked, so a 1/16-scan 32-row panel simply never drives E.

   All 13 signals route through the GPIO matrix into one LCD_CAM data word
   (panel.cpp), so the map is arbitrary as far as the driver is concerned. */
#define HUB75_R1     4
#define HUB75_G1     5
#define HUB75_B1     6
#define HUB75_R2     7
#define HUB75_G2     15
#define HUB75_B2     16
#define HUB75_ADDR_A 18
#define HUB75_ADDR_B 8
#define HUB75_ADDR_C 3
#define HUB75_ADDR_D 42
#define HUB75_ADDR_E 9           // 64-row panels only
#define HUB75_CLK    41
#define HUB75_LAT    40
#define HUB75_OE     2


/* ---- Other Waveshare ESP32-S3-RGB-Matrix hardware ---- */
#define I2C_SDA_PIN     47       // shared bus: PCF85063 RTC, QMI8658 IMU, SHTC3
#define I2C_SCL_PIN     48

/* ---- Time ----
   This board carries a battery-backed PCF85063 RTC (addr 0x51) on the I2C bus.
   The SYSTEM clock remains the single source of truth downstream -- everything
   reads it exactly as before -- but the chip now (a) SEEDS the system clock at
   boot when it holds a plausible time, so the wall clock is valid seconds after
   power-on with no network, and (b) is written back on every NTP sync. With no
   backup cell fitted the chip loses time on power-off and boot falls back to
   the old wait-for-NTP path; rtcEpochNow() returns 0 until then, which every
   consumer already handles. */
#define PCF85063_ADDR     0x51
#define PCF85063_CTRL1    0x00
#define PCF85063_SEC_REG  0x04
#define RTC_YEAR_OFFSET   2000   // PCF85063 reg 6 is 0-99 = 2000-2099

/* ---- Firmware identity ---- */
#define FW_VERSION           "2.2.2"   // this product's version (UI + boot log)
// The gateway REST/MQTT surface this firmware implements, reported as "version"
// by GET /api/config. The companion app gates its features on reading >= 3.1
// there, and this firmware is API-compatible with Split-Flap Gateway 3.1, so it
// must answer 3.1.0 -- not FW_VERSION. GET /api/config also returns "product"
// and "fwVersion" so a client can tell the two apart.
//
// v1.1 adds GET /lang/<code> (the UI's translation dictionaries), but that is a
// DASHBOARD concern, not part of the contract the companion negotiates -- so this
// stays 3.1.0. Raising it would advertise split-flap 3.5 endpoints (calibration,
// provisioning) that this product does not have and cannot have: its modules are
// drawn, not driven.
#define API_VERSION          "3.1.0"
#define PRODUCT_NAME         "Matrix Portal Gateway"

/* ---- Network / service defaults (overridable at runtime via Settings) ---- */
/* WiFi credentials are intentionally BLANK. A freshly flashed board therefore comes
   up on its SoftAP setup page ("Split-Flap-GW") so you enter the network once from the
   browser; after that NVS holds it and these are never consulted again.

   For a personal bench build you may temporarily hard-code an SSID/password here to
   skip the setup page -- but they get compiled into the firmware image as plain
   strings (readable with `strings firmware.bin`), so NEVER commit real credentials or
   ship a binary built with them. Blank them again before publishing. */
#define DEFAULT_WIFI_SSID    ""
#define DEFAULT_WIFI_PASS    ""

// Hostname: mDNS name, ArduinoOTA name, DHCP name and the fallback-AP SSID all come
// from cfgHostname(). The default carries the low 24 bits of the eFuse MAC, because
// several gateways on one LAN cannot all answer to "splitflap-gw.local". Override it
// from Settings; blank means "derive it".
#define HOSTNAME_MAX         32
#define HOSTNAME_PREFIX      "splitflap-gw"
#define DEFAULT_AP_PASS      "12345678"       // SoftAP password (>= 8 chars)
#define DEFAULT_MQTT_PORT    1883
#define DEFAULT_MQTT_PREFIX  "splitflap"
#define DEFAULT_NTP_SERVER   "pool.ntp.org"
#define NTP_TIMEOUT_MS       8000UL

/* ---- Panel defaults ----
   Runtime-configurable (Settings -> Panel; applied on reboot, since the driver
   takes its geometry at construction). The grid is the emulated wall: one virtual
   split-flap module per cell, IDs assigned row-major from 0.

   The default is a 15 x 3 wall on a 128x32 chain, which lands on an 8x10 pixel
   cell -- the smallest that still fits a fully-accented CP1252 glyph (6x9 plus a
   one-pixel gutter). 15 columns need 120 px, so a 64-wide panel cannot host this
   wall; chain two 64x32 panels (or use a native 128x32). For a roomy version,
   chain two 64x64 panels: an 8x21 cell picks up the 6x13 face. */
#define DEFAULT_PANEL_W      128    // total chain width in px
#define DEFAULT_PANEL_H      32     // panel height in px (32 or 64)
#define DEFAULT_GRID_COLS    15     // virtual modules across
#define DEFAULT_GRID_ROWS    3      // virtual modules down
#define DEFAULT_BIT_DEPTH    4      // bitplanes, 1..8 (RAM and refresh rate scale with it)
#define DEFAULT_PANEL_BGR    false  // true if the panel is wired BGR (see panelSetColourOrder)
#define DEFAULT_BRIGHTNESS   160    // 1..255, scales every colour before output
// A real module flips a handful of flaps per second, not fifty. This also sets the
// repaint rate: one flap is two half-steps, so the panel redraws at 2000/flapMs Hz
// while a reel is turning (DISP_MIN_FRAME_MS in display.cpp caps it regardless).
#define DEFAULT_FLAP_MS      60     // ms per flap step -- the reel's speed
// Draw a geometry test pattern for four seconds at boot, before any flap content. It is
// on by default: it costs four seconds and it tells you which layer is wrong the moment
// the wall looks scrambled, without a rebuild.
//   * border broken, doubled, or only half-height  -> scan / address lines (addrBits)
//   * diagonal duplicated or stair-stepped         -> row mapping, wrong panel height
//   * gap or swapped halves at x = panelW/2        -> the chain, not the firmware
//   * corner colours not R/G/B/W clockwise from TL -> RGB pin order
//   * pattern perfect but flaps garbled            -> content, not the panel
#define PANEL_BOOT_TEST      1

// Diagnostic: build with -DPANEL_DISABLE=1 to bring the gateway up with the panel
// never started -- no framebuffer, no GDMA, no panel current. Isolates the radio
// from the panel. See the block comment in dispInit().
#ifndef PANEL_DISABLE
#define PANEL_DISABLE        0
#endif

/* How much internal SRAM the panel may take, and how much it must leave behind.
   dispInit() runs before WiFi.begin() so Protomatter^W panel.cpp gets first claim on
   the internal DMA pool -- but WiFi and lwIP allocate their RX/TX buffers from that
   same pool moments later, and nothing else in the firmware defends it. The settings
   page happily accepts 256x64 at depth 6, which is 242 KB of double-buffered
   framebuffer plus descriptors; that leaves the network stack on fumes, and the
   failure does NOT look like a panel fault. It looks like TCP connects failing
   (MQTT rc=-2), a web UI that stalls, and loop()'s 20 KB heap floor rebooting the
   board. panelBegin() first clamps the bit depth down until the framebuffer fits
   (v1.17.1) and refuses outright only if even depth 1 breaches a bound.

   BUDGET is the ceiling on the panel itself; RESERVE is what must still be free
   afterwards for WiFi/lwIP to come up. Both are internal-DMA-capable bytes.
   The 128x32 depth-4 default costs ~38.6 KB, so there is room to grow. */
#define PANEL_RAM_BUDGET     (120u * 1024u)   // most the panel may ever claim
#define PANEL_RAM_RESERVE    (100u * 1024u)   // must remain free for WiFi + lwIP

// Circuit breaker for the large canvas ops (animation + QOI upload, framebuffer readback). If free
// internal heap is already below this when one arrives, refuse it with 507 rather than pile its
// transient (PSRAM alloc + panel takeover, or a ~48 KB response stream) onto a stressed heap and
// risk loop()'s 20 KB reboot floor. 2x the floor: a genuinely-low signal, not a normal dip. A big
// panel (256x64) sits near this under companion load; a small one never does.
#define CANVAS_MIN_UPLOAD_HEAP  (40u * 1024u)

#define PANEL_MAX_W          256   // panel height is validated by the enumerated 16/32/64 set

/* ---- Flip animation ----
   Changing the displayed flap cascades forward through the reel, which is what
   makes the panel read as a split-flap. It is a rendering effect, not a mechanism.

   FLAP_ANIM_MAX bounds one change to 64 flips (64 * DEFAULT_FLAP_MS ~= 3.8 s for
   the longest cascade -- a physical 64-flap reel's full revolution, kept as the
   cap even though this reel has 237 flaps). A longer jump starts its walk flapMax
   flaps short of the destination. Set flapMax to 1 for an instant cut. */
#define DEFAULT_FLAP_MAX     64     // flips drawn for one character change
#define FLAP_ANIM_MAX        64     // hard ceiling on flapMax


/* ---- Flap set sizing ----
   A physical reel has 64 leaves because it is a physical object. These modules are DRAWN,
   so there is nothing to ration: the reel carries 237 flaps -- every printable CP1252
   glyph, the seven colour flaps, every lowercase letter, and the pictographs. It is BUILT
   AT BOOT (reelBuild in reel.h) rather than typed out, and it is not stored per module and
   not configurable -- a reel that can already draw everything has nothing left to
   reconfigure, so the physical gateway's 'N' command and flap-set editor are gone.

   The SECTION ORDER is the contract (see reel.h for the full story): the 156 uppercase
   glyph flaps and the 7 colour flaps (indices 0..162) come first, byte-compatible with
   the classic reel; the lowercase and pictograph flaps live PAST them, reachable only by
   index. On the one-byte legacy path lowercase still folds to uppercase -- it must,
   because the bytes r o y g b p w mean colours there -- while the index-addressed API
   (/api/display/cells, /api/flap/index) reaches every flap without folding.

   Flap 0 is blank (the home position).

   ONE COMPATIBILITY NOTE, deliberately accepted: the 'A' reply reports flapCount as
   SF_LEGACY_FLAPS (163), and splitflap-os (server/app.py) rejects any count outside
   1..64, so it will refuse this reel and fall back to its own map. That costs nothing
   here -- it addresses flaps BY CHARACTER ('-'), never by index, so all of its text
   still displays exactly as before. The gateway's own UI/REST/MQTT are unaffected. */
#include "reel.h"          // SF_MAX_FLAPS / SF_COLOUR_FLAPS / SF_CHAR_FLAPS, and the reel
                           // itself -- Arduino-free, so tools/reel_test.cpp compiles the
                           // very same code rather than a copy of it.
#define SF_MAX_TEXT          256   // longest text sfSendText will lay across modules

/* ---- Buffer / queue sizes ----
   TX_MAX_BYTES (512) is kept generous for raw frame sends. MSG_MAX_BYTES caps
   what the MQTT wire mirror records per frame; it also sizes mqttPublishMsg's
   stack buffer (MSG_MAX_BYTES*3 + 80, since a flap byte can expand to a 3-byte
   UTF-8 glyph), which MQTT_BUF_SIZE must be able to hold. */
#define MSG_RING_SIZE        64    // command log: number of entries retained
#define MSG_MAX_BYTES        320   // MQTT wire mirror: bytes stored per frame
#define TX_MAX_BYTES         512   // max bytes frameSend will transmit in one frame
#define MQTT_BUF_SIZE        1280  // MQTT packet buffer + queue slot: >= the worst-case
                                   // rx/tx monitor JSON (320 wire bytes -> 3-byte UTF-8
                                   // glyphs = ~960B + prefix) and any dump payload
#define MQTT_Q_SIZE          32    // outbound MQTT publish queue depth

/* ---- Housekeeping cadences ---- */
#define STATUS_INTERVAL_MS      60000UL   // MQTT status publish cadence (1/min)
// Longer than a companion heartbeat (~30 s) ON PURPOSE. Each change RESTARTS this
// clock, so two companions flipping the URL between them never hold still long enough
// to be written -- which is the point. A single, real change persists after two quiet
// minutes.
#define COMPANION_SAVE_DEBOUNCE_MS 120000UL   // companion URL: persist once it settles

/* ---- Persisted files (FFat) ----
   Only the companion settings blob below. The virtual modules persist NOTHING:
   every field is deterministic from the configured grid (id = wall slot, serial
   derived from the MAC, reel homed at boot), so the physical gateway's
   /modules.dat registry and this product's former /vmods.dat are both gone --
   vmInit() deletes a leftover file from older firmware. */

/* ---- Companion settings blob (FFat) ---- */
#define COMPANION_FILE       "/compset.gz"
#define COMPANION_TMP        "/compset.tmp"
#define COMPANION_MAX_BYTES  (64UL * 1024UL)

/* ---- Companion tab advertisement (v1.4) --------------------------------------------
   The two apps tell each other which tabs they have, so each nav can deep-link the
   other's screens instead of hard-coding a list that goes stale. The companion POSTs
   its `tabs` to /api/companion; the response always carries this firmware's `gwTabs`.
   Either side may say nothing (an older peer), and the other then falls back to its
   own built-in list. */
#define COMPANION_TABS_MAX     384
#define COMPANION_TABS_MAX_N   10    // max tabs accepted from a companion
#define COMPANION_TAB_ID_MAX   24    // max chars of one tab's id (the URL hash)
#define COMPANION_TAB_LBL_MAX  24    // max chars of one tab's label

/* ==========================================================================*/
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ============================================================================
 *  SHARED INFRASTRUCTURE  (cross-cutting; defined once in globals.cpp)
 * ==========================================================================*/
extern volatile bool gSerialDebug;
extern volatile bool gQuietTime;
extern char gCompanionStatus[80];
extern char gCompanionTabs[COMPANION_TABS_MAX];   // the companion's tabs, already serialised JSON
extern volatile unsigned long gCompanionSeenMs;
// The companion URL is persisted on a DEBOUNCE, not on every change. Two companions
// pointed at the same gateway will each re-register their own URL on their heartbeat,
// so cfg.companionUrl flips back and forth -- and saving on every change turned that
// into an NVS write every ~30 s, forever. Observed in the wild. The URL is applied to
// RAM immediately (the UI and the companion tabs are live at once); only the flash
// write waits for the value to hold still. A contested URL therefore never reaches
// flash at all, which is the right answer: nothing durable should be written for a
// value two clients are still arguing over. A companion re-registers within a
// heartbeat of any reboot, so nothing is lost by not persisting it.
extern volatile bool          gCompanionUrlDirty;
extern volatile unsigned long gCompanionUrlDirtyMs;
extern volatile bool gDisplayDirty;
extern volatile bool gOtaInProgress;
// Raw-canvas mode: HTTP owns the panel and the reel renderer stands down (see /api/canvas).
extern volatile bool gCanvasMode;
// Acknowledgement from taskDisplay that it has parked for canvas/OTA; the take-over waits on it.
extern volatile bool gDispParked;
extern volatile bool gOtaRebootPending;
extern bool gApActive;
extern SemaphoreHandle_t timeMutex;
extern StaticSemaphore_t timeMutexBuf;
extern volatile unsigned long wdgFramesMs;
extern volatile unsigned long wdgNetMs;
extern volatile unsigned long wdgWebMs;
extern volatile unsigned long wdgDispMs;
extern TaskHandle_t hTaskRTC, hTaskFrames, hTaskOTA, hTaskWeb, hTaskNet, hTaskDisp;
extern bool ntpSynced;
extern unsigned long staDownSince;

#endif // MPGW_COMMON_H
