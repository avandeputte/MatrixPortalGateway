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
//   rs485.*     bus framing, sanitization, TX choke point, monitor ring
//   vbus.*      the emulated bus underneath rs485Send: delivery + reply queue
//   vmodule.*   the virtual split-flap modules: protocol, reel, persistence
//   display.*   HUB75 panel geometry and the flap renderer
//   panel.*     the HUB75 driver itself (LCD_CAM + GDMA)
//   modules.*   split-flap module REGISTRY, protocol commands, reply parser,
//               FATFS persistence  (the gateway's view of the modules)
//   mqtt.*      MQTT client, outbound publish queue, Home Assistant discovery
//   web.*       HTTP server: dashboard page (web_ui.h) + REST API handlers
//   ota.*       firmware update: ArduinoOTA + browser upload
//   tasks.*     the FreeRTOS task loops (Bus / RTC / Web / Network / Display)
//   main.cpp    setup() boot sequence + loop() watchdog supervisor
//
// Note the two "module" layers, which the RS-485 gateway also has: modules.* is
// the gateway's REGISTRY of whatever answers on the bus, and it is unchanged from
// upstream. vmodule.* is what answers. They talk only through protocol frames.

#ifndef MPGW_GATEWAY_H
#define MPGW_GATEWAY_H

#include "common.h"
#include "config.h"
#include "charset.h"
#include "font1252.h"
#include "rtc.h"
#include "rs485.h"
#include "vbus.h"
#include "vmodule.h"
#include "display.h"
#include "modules.h"
#include "mqtt.h"
#include "ota.h"
#include "web.h"
#include "tasks.h"

#endif // MPGW_GATEWAY_H
