// gateway.h -- umbrella header included by every .cpp in the project.
//
// Pulls in the shared kernel (common.h) and each subsystem's public API. A
// source file includes this one header and sees everything it may legally call.
//
// Source map:
//   common.h    libraries, board config macros, shared structs, global externs
//   globals.cpp single definition site for every shared/extern global
//   config.*    runtime configuration (GwConfig) persisted in NVS
//   rtc.*       wall-clock time: the ESP32's internal RTC + NTP
//   charset.*   UTF-8 <-> Windows-1252 flap-byte transcoding
//   font1252.*  GENERATED bitmap glyphs for the 216 printable CP1252 flaps
//   frames.*    frame sanitization, TX choke point, command log
//   vmodule.*   the virtual split-flap modules: protocol, reel, persistence
//   display.*   HUB75 panel geometry and the flap renderer
//   panel.*     the HUB75 driver itself (LCD_CAM + GDMA)
//   modules.*   high-level protocol send helpers (text/char/home) + FATFS mount
//   web.*       HTTP server: dashboard page (web_ui.h) + REST API handlers
//   ota.*       firmware update: raw-body browser/curl upload + mDNS
//   tasks.*     the FreeRTOS task loops (Frames / RTC / Web / Network / Display)
//   main.cpp    setup() boot sequence + loop() watchdog supervisor
//
// Note the two "module" layers: modules.* is the gateway side (it builds and
// sends protocol frames), vmodule.* is what answers them. They talk only through
// protocol frames -- the same seam the physical gateway has with real modules.

#ifndef MPGW_GATEWAY_H
#define MPGW_GATEWAY_H

#include "common.h"
#include "config.h"
#include "charset.h"
#include "font1252.h"
#include "rtc.h"
#include "frames.h"
#include "vmodule.h"
#include "display.h"
#include "modules.h"
#include "ota.h"
#include "web.h"
#include "tasks.h"

#endif // MPGW_GATEWAY_H
