# Matrix Portal Gateway

A split-flap display with no split flaps.

This is the [Split-Flap Gateway](../../SplitFlapGateway/3.1) v3.1 firmware, ported to an
**Adafruit MatrixPortal ESP32-S3** driving a HUB75 RGB LED matrix. The RS-485 transceiver is
gone. In its place is a software bus and a wall of *virtual* split-flap modules, each one
emulating the real module firmware's wire protocol byte for byte and rendering itself as a
flapping character cell on the panel.

Everything above the bus is unchanged: the web UI, the REST API, MQTT and Home Assistant
discovery, OTA, the sticky module registry, the bus monitor. The
[companion app](../../SplitFlapGatewayCompanion) drives it without modification and cannot
tell the difference.

```
     ┌──────────────────────────────────────────────┐
     │  web UI · REST API · MQTT · OTA · registry    │   unchanged from Gateway 3.1
     ├──────────────────────────────────────────────┤
     │  rs485Send()  framing · sanitization · Quiet  │   unchanged
     ├──────────────────────────────────────────────┤
     │  vbus         deliver frames · queue replies  │   new
     ├──────────────────────────────────────────────┤
     │  vmodule      45 virtual split-flap modules   │   new
     ├──────────────────────────────────────────────┤
     │  display      HUB75 flap renderer             │   new
     ├──────────────────────────────────────────────┤
     │  panel        LCD_CAM + GDMA HUB75 driver     │   new  (no external library)
     └──────────────────────────────────────────────┘
```

---

## Table of Contents

- [What it does](#what-it-does)
- [The reel](#the-reel)
  - [Why `m38-r` shows red and not the letter r](#why-m38-r-shows-red-and-not-the-letter-r)
- [What is and is not emulated](#what-is-and-is-not-emulated)
- [The panel](#the-panel)
- [The flip](#the-flip)
- [Configuration](#configuration)
- [Serial numbers](#serial-numbers)
- [Compatibility](#compatibility)
- [Repository contents](#repository-contents)
- [Building](#building)
- [Known limitations](#known-limitations)

---

## What it does

On boot the firmware creates one virtual split-flap module per cell of the module wall
(15 × 3 = **45 modules** by default), with IDs `0`…`44` fixed by wall position. The gateway discovers
them exactly as it would discover real hardware — it broadcasts `m*v`, they answer with their
firmware version and serial number, and they populate the registry.

From then on, everything works. Send `m5-A` and module 5's reel flips forward until it lands
on `A`. Send text from the Display tab and it cascades across the wall. Reconfigure a
module's flap count and character set. The bus monitor shows every frame in both directions.

The difference is that nothing is moving. The modules are software, and the panel is where
they live.

---

## The reel

Each virtual module carries **64 flaps** — the same count as a real module. The default reel
is a **German** layout:

| Index | Contents |
|---|---|
| `0` | blank (the home position) |
| `1`–`26` | `A`–`Z` |
| `27`–`30` | `Ä Ö Ü ß` |
| `31`–`40` | `0`–`9` |
| `41`–`56` | `! @ # & € - = ; q : ' . , / ? *` |
| `57`–`63` | the seven colour flaps: `r o y g b p w` |

```
 ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜß0123456789!@#&€-=;q:'.,/?*roygbpw
```

The reel is a *character set*, not a hardware constant: it is set per module by the `N`
command (`POST /api/flap/flapconfig`) and only defaults to the layout above. It swaps five of
the classic reel's symbols (`$ ( ) + %`) for the five German needs (`Ä Ö Ü ß` and `€`); the
rest matches the reel every existing controller assumes. **This is not byte-identical to the
[companion](../../SplitFlapGatewayCompanion)'s built-in `DEFAULT_FLAP_CHARS`** — text sent by
character still lands correctly (see below), but a controller that sends one of the five
dropped symbols will get a blank, and the umlauts and `€` are reachable from *this* gateway's
own UI/API, which decode UTF-8 to Windows-1252.

The `"` flap lives at index `49` and is addressed as `q` — the classic reel has no lowercase,
so the firmware's char map has always borrowed that byte for the double quote. The renderer
draws it back as `"` so the wall shows a quote, not a `q`.

### Why `m5-r` shows red and not the letter r

The protocol addresses the colour flaps by the lowercase letters `r o y g b p w`. The reel
also carries the *uppercase* letters, so something has to disambiguate.

Two things do. First, the module firmware's `flapIndexOf()` is a **first-match scan**, and the
only lowercase bytes on the reel are `q` and the seven colour codes — so `r` can only be the
red flap at index 57 (uppercase `R` is a different byte, at index 18). Second, `sfSendChar()`
**uppercases** every ASCII letter before sending — *except* the seven colour codes:

```
m5-r      → flap 57   the red colour flap  (colour codes are never folded)
m5-R      → flap 18   the letter R
m5-a      → flap 1    folded to 'A'        (the reel has no lowercase letters)
m5-A      → flap 1    the letter A
m5+0      → flap 0    blank
```

So `hello` renders `HELLO`, and `m5-r` still means red — every existing colour command works
unchanged. Accented lowercase folds too (`ä` → `Ä`), so text with real umlauts displays.

---

## What is and is not emulated

**The protocol is emulated where it is used.** The firmware speaks the subset of the module
v31 command set the gateway still needs: display (`-`, `+`), homing (`h`), the flap-set
config (`N`), and the queries (`v`, `A`) — plus the `mX…` by-serial forms `mXA` and `mXN`.
Broadcasts, the two-star v6 form, and the ranged `m*v0-49` / `m*A0-49` batch queries all work.

**The mechanism is not emulated at all.** There is no stepper, no Hall sensor and no EEPROM.
Nothing can be out of tune, so nothing needs tuning: the calibration, diagnostics, provisioning
and backup commands have all been removed rather than faked. `h` simply shows flap 0, and the
`A` reply reports the nominal `homeOffset=2832` / `totalSteps=4096` with an empty flap map.

**The bus is not emulated either.** A real RS-485 bus at 9600 baud is half-duplex and slow; a
broadcast `m*v` across 45 modules takes four seconds of staggered reply slots. Here replies
come back promptly, in module-ID order, spaced just enough to stay legible in the monitor. The
`baud`, `dataBits`, `parity` and `stopBits` settings survive in the UI and the API so a config
round-trips between the two firmwares, but nothing reads them.

Collisions therefore do not happen. Two modules given the same ID will both obey a command, but
they answer one after the other rather than on top of each other, so the gateway's duplicate-ID
heuristic — which keys off garbled serial numbers — never fires.

---

## The panel

The module grid *is* the wall. Cell size falls out of it:

```
cellW = panelW / gridCols          cellH = panelH / gridRows
```

and the renderer then picks the roomiest bundled font that fits with a one-pixel seam. Four
faces are bundled — `6x13`, `6x10`, `6x9` and `5x8` — all carrying the full 216-glyph CP1252
set with real diacritics. (`5x7` and `4x6` are deliberately absent: at those sizes the source
face has no room for accents and draws `À` identically to `A`, which on this reel is a
correctness bug, not an aesthetic one. `tools/genfont.py` rejects any face that does this.)

Leftover pixels become an even margin, so the wall is centred. A grid whose cells could not
hold even the 5×8 face is quietly reduced, and the boot log says so.

The default is a **15 × 3 wall on a 128 × 32 chain** — two 64×32 panels in series, or one
native 128×32. Fifteen columns need 120 px, so this wall does not fit a 64-wide panel.

| Panel | px per module | Font | Verdict |
|---|---|---|---|
| 64 × 32 | 4 × 10 | — | 15 columns don't fit; auto-reduced to 10 |
| 64 × 64 | 4 × 21 | — | too narrow for 15 columns |
| **128 × 32** | **8 × 10** | **6×9** | the default. Tight, every glyph legible |
| **128 × 64** (2 × 64×64) | **8 × 21** | **6×13** | roomy, with real bezels |

A 64-row panel needs the E address line, which is GPIO 21 on this board. A solder jumper on the
MatrixPortal selects which HUB75 connector pin that reaches (pin 8 by default, or pin 16); match
it to your panel.

---

## The flip

Changing the displayed flap cascades forward through the reel one flap at a time. This is a
*rendering effect*, not a simulation.

`flapMax` caps one change at **64 flips** (a whole reel's worth); a longer jump starts its walk
`flapMax` flaps short of the destination. At the default 60 ms per flap, the longest cascade
takes about 3.8 seconds. Set `flapMax` to `1` for an instant cut.

Mid-flip, a cell shows the top half of the incoming flap above the bottom half of the outgoing
one, split by a bright seam. That is what a split-flap does, and it is why the animation reads
correctly even in an 8×10 cell.

---

## Configuration

Everything is on the Settings page and in `POST /api/config/settings`.

| Setting | Default | Applies |
|---|---|---|
| `gridRows` × `gridCols` | 3 × 15 | **on reboot** — this creates and destroys modules |
| `panelW` × `panelH` | 128 × 32 | **on reboot** — the panel driver takes geometry at init |
| `panelBitDepth` | 4 | **on reboot** — 1…6; RAM and EMI scale with it |
| `panelBright` | 160 | next frame |
| `flapMs` | 60 | next flap |
| `flapMax` | 64 | next change |

`GET /api/config` also reports `product`, `fwVersion` and `maxFlaps`. Its `version` field is the
**gateway API level** (`3.1.0`), not this firmware's version — see
[Compatibility](#compatibility).

> ### Test WiFi credentials are compiled in
> `DEFAULT_WIFI_SSID` / `DEFAULT_WIFI_PASS` in `src/common.h` seed the config on a board whose
> NVS has never had a network saved, so a freshly flashed unit joins the bench network without
> the SoftAP setup page. They are a development convenience, **not** a secret store — anyone
> with the firmware image can read the password. Blank them before publishing the firmware or
> the repository. Once any network is saved from the UI, NVS wins and they are never consulted
> again.

---

## Serial numbers

Each module gets a deterministic, obviously-fake 20-character serial:

```
FA5E  <the board's 6 MAC bytes>  <module index>  <crc8>
└──┘
"fabricated"
```

Stable across reboots (the registry keys modules by serial), unique per board, and never
mistakable for a real ATtiny SIGROW read.

---

## Compatibility

The companion app talks to the gateway over exactly seven HTTP endpoints and models *nothing*
about a module — not the flap count, not the character set, not serial numbers. The
reel and the `FA5E…` serials are invisible to it. Those seven are
`GET /api/config`, `GET /api/status`, `POST /api/rs485/send`, `POST /api/rs485/batch`,
`POST /api/companion`, and `GET`/`PUT /api/companion/settings`.

Two things do matter, and both are handled:

1. **`GET /api/config` must report `version >= 3.1`.** The companion parses `MAJOR.MINOR` out of
   it and gates its gateway-stored settings on `>= (3,1)`. This firmware implements that surface
   exactly, so it answers `3.1.0` and puts its own version in `fwVersion`.
2. **`POST /api/rs485/send` and `/api/rs485/batch` must forward frame bytes verbatim.** The
   companion sends `m00-A\n` style frames as `windows-1252`, one byte per glyph. They are not
   transcoded.

The gateway's own MQTT surface, Home Assistant discovery and the `/api/companion/settings` gzip
blob store are carried over untouched.

---

## Repository contents

```
src/common.h        board config, panel defaults, buffer sizes, shared types
src/globals.cpp     single definition site for every shared global
src/config.*        runtime configuration (GwConfig) persisted in NVS
src/rtc.*           wall-clock time: the ESP32's internal RTC + NTP
src/charset.*       UTF-8 <-> Windows-1252 flap-byte transcoding
src/font1252.*      GENERATED bitmap glyphs for the 216 printable CP1252 flaps
src/rs485.*         bus framing, sanitization, TX choke point, monitor ring
src/vbus.*          the emulated bus: frame delivery + the reply queue
src/vmodule.*       the virtual split-flap modules: protocol, reel, persistence
src/display.*       flap-wall geometry and the flap renderer (calls panel.*)
src/panel.*         the low-level HUB75 driver: ESP32-S3 LCD_CAM + GDMA, no library
src/modules.*       the gateway's module REGISTRY and reply parser   (unchanged)
src/mqtt.*          MQTT client, publish queue, HA discovery         (unchanged)
src/web.*           HTTP server: dashboard (web_ui.h) + REST API
src/ota.*           firmware update: ArduinoOTA + browser upload     (unchanged)
src/tasks.*         the FreeRTOS task loops
src/main.cpp        setup() boot sequence + loop() watchdog supervisor

tools/genfont.py    regenerates src/font1252.cpp from the vendored BDFs
tools/bdf/          public-domain X11 "misc-fixed" fonts (6x13, 6x10, 6x9, 5x8)
tools/reel_test.cpp native regression test for the reel and the 'A' reply format

platformio.ini      build/upload configuration
ARCHITECTURE.md     why the non-obvious decisions were made
openapi.yaml        REST API reference
```

Note the two "module" layers, which the RS-485 gateway also has: `modules.*` is the gateway's
**registry** of whatever answers on the bus, unchanged from upstream. `vmodule.*` is what
answers. They talk only through protocol frames — which is exactly why the port works.

---

## Building

```sh
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # 115200 baud, native USB CDC
```

Regenerate the fonts (only needed if you swap a BDF):

```sh
python3 tools/genfont.py
```

Run the reel regression test on the host:

```sh
c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp -o /tmp/reel_test && /tmp/reel_test
```

---

## Known limitations

- **WiFi and HUB75 on this board share the internal-SRAM bus.** The panel's GDMA streams the
  framebuffer continuously out of the same internal SRAM the WiFi MAC uses, and at a high pixel
  clock that starves the radio — association fails and TCP connections drop even at close range.
  The pixel clock is therefore held at 5 MHz (`LCD_CLK_HZ` in `src/panel.cpp`), which is ample
  for a flicker-free refresh; do **not** raise it. If you still see WiFi trouble, drop
  `panelBitDepth`. WiFi modem sleep is also disabled (`src/main.cpp`) for the same reason.
- **The framebuffer cannot go in PSRAM.** This board's 2 MB is *quad* SPI, far too slow to feed
  a panel, so `panel.cpp` allocates the double-buffered framebuffer and the DMA descriptor chain
  from internal DMA-capable RAM (`MALLOC_CAP_DMA`). That bounds panel size and colour depth
  together, and `panelBegin()` refuses a geometry that would starve WiFi of that pool. The
  virtual-module array is pinned to internal RAM too — `taskDisplay` walks it 100×/s on the core
  the refresh runs on, and quad PSRAM there causes a shimmer. (The monitor ring, MQTT queue,
  module registry and the scheduled-TX ring *are* in PSRAM; nothing in the refresh path touches
  them.)
- **No battery-backed RTC.** Wall-clock time is invalid from power-on until the first NTP sync.
  Every caller already handles that state; frame timestamps show `HH:MM:SS` uptime until then.
- **No collision emulation**, so the duplicate-ID heuristic never fires.

---

## Licence

CC BY-NC-SA 4.0, as the upstream Split-Flap Gateway. Split-flap module hardware and the initial
protocol by [Adam G Makes](https://www.youtube.com/@AdamGMakes). The bundled bitmap fonts are
the X11 `misc-fixed` faces: *"Public domain font. Share and enjoy."*
