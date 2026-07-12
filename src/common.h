#ifndef MPGW_COMMON_H
#define MPGW_COMMON_H

/*
 * Matrix Portal Gateway
 * Firmware for the Adafruit MatrixPortal ESP32-S3 driving a HUB75 RGB LED matrix.
 *
 * A port of the Split-Flap Gateway (v3.1) that keeps the entire gateway -- web UI,
 * REST API, MQTT/Home Assistant, OTA, module registry, bus monitor -- and replaces
 * the RS-485 transceiver with a software bus and a panel full of *virtual*
 * split-flap modules. Nothing is wired to a real reel: every module the gateway
 * discovers, calibrates and drives is emulated in firmware and drawn as a flap
 * cell on the LED matrix.
 *
 * The emulation is at the PROTOCOL level, not the API level. Commands are framed,
 * sanitized and "transmitted" exactly as before; the virtual modules parse those
 * bytes and reply with real protocol frames, staggered on a simulated half-duplex
 * bus. The gateway cannot tell the difference, so the companion app and any MQTT
 * client keep working unchanged.
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
 *   Bus format:   m<ADDR><CMD>[data]\n
 *   Address:      decimal (zero-padded 2-digit v6 style, or variable-length v7+)
 *                 broadcast: m** (v6) or m* (v7+), optional range m*v<lo>-<hi>
 *                 by serial: mX...
 *   Key commands: m<id>-<char>    display character
 *                 m<id>+<n>       display by flap index
 *                 m<id>h          home
 *                 m<id>c          calibrate     -> m<id>:<steps>\n
 *                 m<id>v          get version   -> m<id>v:<ver>:<id>:<sn>\n
 *                 m<id>d          dump EEPROM   -> m<id>d:<ho>:<ts>:<map>\n
 *                 m<id>A          all fields    -> ...:<map>:<flapCount>:<flapChars>\n
 *                 m<id>N<n>:<s>   configure the flap set
 *                 mX<cmd><sn>     address one module by serial number
 *
 * Board: Adafruit MatrixPortal ESP32-S3 (8MB flash, 2MB *quad* PSRAM)
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

/* ============================================================================
 *  BOARD / BUILD CONFIGURATION
 *  ----------------------------------------------------------------------------
 *  Everything needed to retarget this firmware to a different board, panel, or
 *  default setup lives in this single block.
 *
 *  Default config is for the Adafruit MatrixPortal ESP32-S3 (product 5778).
 * ==========================================================================*/

/* ---- HUB75 matrix pins ----
   Straight from Adafruit's own MatrixPortal S3 pinout. The address list carries all
   five lines; the panel height decides how many are actually clocked, so a
   1/16-scan 32-row panel simply never drives E (GPIO 21).

   The board's PSRAM is QUAD, not octal. That is what leaves GPIO 35/36/37 free
   for D/B/B2 -- an octal-PSRAM S3 module consumes them. It is also why the panel
   framebuffer cannot be pushed to PSRAM (too slow); it must live in internal
   SRAM. See ARCHITECTURE.md. */
#define HUB75_R1     42
#define HUB75_G1     41
#define HUB75_B1     40
#define HUB75_R2     38
#define HUB75_G2     39
#define HUB75_B2     37
#define HUB75_ADDR_A 45
#define HUB75_ADDR_B 36
#define HUB75_ADDR_C 48
#define HUB75_ADDR_D 35
#define HUB75_ADDR_E 21          // 64-row panels only; a board jumper picks the
                                 // HUB75 connector pin (8 by default, or 16)
#define HUB75_CLK    2
#define HUB75_LAT    47
#define HUB75_OE     14


/* ---- Other MatrixPortal S3 hardware ---- */
#define PIN_NEOPIX      4        // single status NeoPixel
#define PIN_BTN_UP      6
#define PIN_BTN_DOWN    7
#define I2C_SDA_PIN     16       // STEMMA QT (also the onboard LIS3DH at 0x19)
#define I2C_SCL_PIN     17

/* ---- Time ----
   The MatrixPortal S3 has NO battery-backed RTC chip (unlike the Waveshare board
   the Split-Flap Gateway runs on, which carries a PCF85063). Wall-clock time is
   kept in the ESP32's internal RTC, seeded from NTP, and is therefore INVALID
   from power-on until the first successful sync. Everything downstream already
   copes: rtcEpochNow() returns 0 while the clock is unset, which is exactly the
   "RTC not valid yet" path the registry pruner and frame timestamps handle. */
#define RTC_YEAR_OFFSET   2000

/* ---- Firmware identity ---- */
#define FW_VERSION           "1.0.1"   // this product's version (UI + boot log)
// The gateway REST/MQTT surface this firmware implements, reported as "version"
// by GET /api/config. The companion app gates its features on reading >= 3.1
// there, and this firmware is API-compatible with Split-Flap Gateway 3.1, so it
// must answer 3.1.0 -- not FW_VERSION. GET /api/config also returns "product"
// and "fwVersion" so a client can tell the two apart.
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
#define DEFAULT_BAUD         9600UL           // emulated bus baud (paces replies)
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
#define DEFAULT_BRIGHTNESS   160    // 1..255, scales every colour before output
// The faint seam drawn around each module -- the split-flap gap. This is NOT the module
// layout (that is GRID_COLS/ROWS above); it is the decoration between cells. Its dimness
// is the colour times the brightness, NOT the panel brightness -- the panel dims the seam
// and the glyphs together, so the seam has to be faint on its own to read as a hairline.
#define DEFAULT_GRID_COLOR   0xFFFFFFUL // 0xRRGGBB; white reads as neutral grey once dimmed
#define DEFAULT_GRID_BRIGHT  32     // 0..255 seam intensity; 0 = no grid at all
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
   board. panelBegin() refuses any geometry that breaches either bound.

   BUDGET is the ceiling on the panel itself; RESERVE is what must still be free
   afterwards for WiFi/lwIP to come up. Both are internal-DMA-capable bytes.
   The 128x32 depth-4 default costs ~38.6 KB, so there is room to grow. */
#define PANEL_RAM_BUDGET     (120u * 1024u)   // most the panel may ever claim
#define PANEL_RAM_RESERVE    (100u * 1024u)   // must remain free for WiFi + lwIP

#define PANEL_MAX_W          256
#define PANEL_MAX_H          64

/* ---- Flip animation ----
   Changing the displayed flap cascades forward through the reel, which is what
   makes the panel read as a split-flap. It is a rendering effect, not a mechanism.

   FLAP_ANIM_MAX bounds one change to 64 flips -- a whole 64-flap reel's worth, and
   64 * DEFAULT_FLAP_MS ~= 3.8 s for the longest cascade. A longer jump starts its
   walk flapMax flaps short of the destination. Set flapMax to 1 for an instant cut. */
#define DEFAULT_FLAP_MAX     64     // flips drawn for one character change
#define FLAP_ANIM_MAX        64     // hard ceiling on flapMax

/* ---- Bus timing ----
   Carried over from the RS-485 gateway. The emulated bus is not metered at
   cfg.rs485Baud -- replies come back promptly (see vbus.*), staggered only by
   VBUS_SLOT_MS so a broadcast stays legible in the monitor. What remains is the
   bus-quiet guard: rs485Send still waits for the bus to fall quiet before it
   transmits, which keeps command and reply frames from interleaving and keeps the
   gateway's collision-avoidance code path identical to the one upstream runs. */
#define TX_BUS_GUARD_MS      12
#define TX_BUS_WAIT_CAP_MS   400

/* ---- Flap set sizing ----
   64 flaps: the real module's reel, exactly. The count is part of the PROTOCOL -- the
   'A' reply reports flapCount, and the controller (splitflap-os, server/app.py) rejects
   any flapCount outside 1..64, so a larger virtual reel would make every module-config
   sync fail silently and the controller fall back to its own map.

   The COUNT is protocol; the CONTENT is ours. VM_DEFAULT_REEL in vmodule.cpp is a GERMAN
   reel: relative to the classic reel it spends five symbol flaps ($ ( ) + %) on the five
   Germany needs (A/O/U-umlaut, eszett, euro). Flap 0 is blank (the home position); the
   seven colour flaps sit at the END (57..63) -- VM_COLOUR_BASE is defined from that, so
   they must stay last. It is safe because the only lowercase on the reel are 'q' and the
   colour codes, so vmFlapIndexOf's first-match scan resolves 'r' to red, not 'R'. */
#define SF_MAX_FLAPS         64    // the real reel: 1 blank + 56 glyphs + 7 colours
#define SF_COLOUR_FLAPS      7     // r o y g b p w, reel indices 57..63
#define SF_MAX_TEXT          256   // longest text sfSendText will lay across modules

/* ---- Buffer / queue sizes ----
   The virtual modules are perfectly calibrated, so they carry no flap-position
   map and every dump reports an empty one. With the 64-flap reel the longest frame
   is the 'A' reply, whose 64-byte flapChars tail dominates at ~117 bytes total,
   followed by the 'mXW' restore and a broadcast 'm*N64:<chars>'. TX_MAX_BYTES (512)
   is kept generous for raw frame sends -- comfortably more than any reply needs.

   MSG_MAX_BYTES is 320 rather than the RS-485 gateway's 256 so a full 'A' reply
   fits in one monitor-ring entry untruncated -- it is the frame you most want to
   read. It also sizes mqttPublishMsg's stack buffer (MSG_MAX_BYTES*3 + 80, since
   a flap byte can expand to a 3-byte UTF-8 glyph), which MQTT_BUF_SIZE must be
   able to hold. */
#define MSG_RING_SIZE        64    // monitor ring: number of frames retained
#define MSG_MAX_BYTES        320   // monitor ring: bytes stored per frame
#define TX_MAX_BYTES         512   // max bytes rs485Send will transmit in one frame
#define MQTT_BUF_SIZE        1280  // MQTT packet buffer + queue slot: >= the worst-case
                                   // rx/tx monitor JSON (320 wire bytes -> 3-byte UTF-8
                                   // glyphs = ~960B + prefix) and any dump payload
#define MQTT_Q_SIZE          32    // outbound MQTT publish queue depth

/* ---- Housekeeping cadences ---- */
#define STATUS_INTERVAL_MS      60000UL   // MQTT status publish cadence (1/min)
#define MODULE_STALE_SECS       86400UL   // 24h: prune modules not seen in this long
// Longer than a companion heartbeat (~30 s) ON PURPOSE. Each change RESTARTS this
// clock, so two companions flipping the URL between them never hold still long enough
// to be written -- which is the point. A single, real change persists after two quiet
// minutes.
#define COMPANION_SAVE_DEBOUNCE_MS 120000UL   // companion URL: persist once it settles
#define MODULE_SAVE_DEBOUNCE_MS 5000UL    // coalesce FATFS writes

/* ---- Module registry sizing ----
   Supports module IDs 0-254 (255 modules). id==255 is reserved as the
   empty-slot / unprovisioned sentinel. The emulated wall uses gridRows*gridCols
   of them (45 by default), but the registry keeps the full ceiling so a
   /modules.dat carried over from a real RS-485 bus still loads. */
#define MAX_MODULES         255   // module IDs 0-254

// Duplicate-ID heuristic: two modules at the same ID both answer a by-ID version
// or 'A' query and collide on the half-duplex bus. Retained because the emulated
// bus reproduces the collision -- a module can still be given a duplicate ID
// with the 'i' command.

/* ---- Persisted files (FFat) ---- */
#define MODULES_FILE     "/modules.dat"
// Bumped to SFG3 when `acked` left PersistedModule: an SFG2 file has a different
// record size, and the count*sizeof() read would silently misparse it. A rejected
// file costs nothing -- every module re-registers on the next m*v.
#define MODULES_MAGIC    0x53464733UL   // "SFG3"
// The virtual modules' own "EEPROM" -- id, serial, calibration, flap set. Written
// with the same temp-file-then-rename atomicity as the registry.
#define VMODULES_FILE    "/vmods.dat"
#define VMODULES_TMP     "/vmods.tmp"
// "VMO" + a layout generation. The saved record embeds flapChars[SF_MAX_FLAPS], so
// sizeof(VModule) is part of the file format: bump the low byte whenever the record
// layout changes (e.g. SF_MAX_FLAPS), or vmLoad() will read the old layout as garbage.
#define VMODULES_MAGIC   0x564D4F01UL   // "VMO" + layout generation 1

/* ---- Companion settings blob (FFat) ---- */
#define COMPANION_FILE       "/compset.gz"
#define COMPANION_TMP        "/compset.tmp"
#define COMPANION_MAX_BYTES  (64UL * 1024UL)

/* ==========================================================================*/
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ============================================================================
 *  SHARED INFRASTRUCTURE  (cross-cutting; defined once in globals.cpp)
 * ==========================================================================*/
extern volatile bool gSerialDebug;
extern volatile bool gMaintenanceMode;
extern volatile bool gQuietTime;
extern char gCompanionStatus[80];
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
extern volatile bool gOtaRebootPending;
extern char gWallChars[256];
extern bool gApActive;
extern SemaphoreHandle_t timeMutex;
extern StaticSemaphore_t timeMutexBuf;
extern volatile unsigned long wdgRS485Ms;
extern volatile unsigned long gLastRxMs;
extern volatile unsigned long wdgNetMs;
extern volatile unsigned long wdgWebMs;
extern volatile unsigned long wdgDispMs;
extern TaskHandle_t hTaskRTC, hTaskRS485, hTaskOTA, hTaskWeb, hTaskNet, hTaskDisp;
extern bool ntpSynced;
extern unsigned long staDownSince;

#endif // MPGW_COMMON_H
