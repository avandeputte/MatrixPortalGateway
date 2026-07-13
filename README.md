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

## New in 1.4

- **The registry now reconciles against the wall at boot.** Discovery only ran when the
  registry was *completely empty*, so growing the wall (15×3 → 15×5) left it holding the 45
  modules it had loaded from FATFS and never asked the 30 new ones to announce themselves.
  The Modules tab read "45 known modules" forever, the new rows rendered as empty cells, and
  any client driving the wall from `/api/flap/modules` kept addressing only the first 45 — so
  the panel really did keep showing a 15×3 picture on a 15×5 wall. Pressing **Identify All**
  was the only way out, and you had to know to do it. It now rebuilds whenever the registry
  disagrees with the wall, in either direction.

- **Companion tab advertisement — the whole feature was missing.** `/api/companion` accepted
  the companion's `tabs` and *silently discarded them*, and never advertised the gateway's own
  `gwTabs`. So neither app could link to the other's screens. Both directions now work. The
  gateway advertises **Modules, Display, Monitor, Settings, Status** — deliberately *not*
  Provision or Calibration, which the split-flap gateway has and this product cannot.

---

## New in 1.3

- **A 15 × 5 geometry preset for 128 × 64 panels — 75 modules, filling the wall.** The firmware
  already supported it (`VM_MAX_MODULES` is 128, and the emulated bus's reply queue is sized for
  a full 128-module wall); it simply was not offered. Every 128 × 64 preset was three rows tall,
  which leaves two thirds of the panel as bezel. Pick it in Settings → *Geometry preset*.

  A 64-row chain halves the refresh rate (~157 Hz → ~78 Hz) and doubles the framebuffer
  (38 KB → 77 KB of internal DMA RAM). Both are within budget. See [The panel](#the-panel).

---

## New in 1.2.1

- **Every board now has its own identity.** The auto hostname, the MQTT client id and the
  Home Assistant device id were all derived from `ESP.getEfuseMac()`'s *low* bytes — which are
  the Espressif OUI, identical on every ESP32 ever made. Two gateways on one LAN both called
  themselves `splitflap-gw-e22748` and both connected to MQTT as `splitflap-20E22748`, and a
  broker evicts the client already holding a duplicate id, so the pair knocked each other
  offline in a loop. They now derive from the MAC bytes that actually differ.

  > **If you use the Home Assistant integration, the device id changes** (`sfgw_20E22748` →
  > `sfgw_E2205AC8`). Home Assistant will discover a new device. Delete the old one.

---

## New in 1.2

- **Quiet Time now blanks the wall.** Turning it on homes every reel to its blank flap — the
  same operation as **Home All** — so the panel goes empty for the night instead of freezing
  mid-message. Turning it off restores what was there. The reels are virtual, but the frame is
  real: the blank goes out as a broadcast `m*h` through the emulated bus, so the flaps visibly
  flip down on the panel exactly as physical modules would.

---

## New in 1.1

- **The dashboard speaks 13 languages**, following your browser the same way light/dark already
  does. One firmware image ships all of them. See [Language](#language).
- **Fixed two invisible controls in the light theme**, both shipped in 1.0.1. The Home Assistant
  re-skin re-pointed `--acc` to 12% black but left `button{color:#fff}` inherited, so every
  secondary button (Identify All, Refresh, Clear, Download Log, Test Connection) drew white on
  near-white — a contrast ratio of **1.3:1**. Separately, the bus monitor's text was themed to
  follow `--txt` while its panel stayed hardcoded dark, giving near-black on near-black at
  **1.2:1**. Both now clear WCAG AA.
- **The re-skin is finished.** The monitor, the status tiles and the module cards read from the
  theme instead of the old dark palette, in both light and dark.

---

## Table of Contents

- [What it does](#what-it-does)
- [The reel](#the-reel)
  - [Why `m38-r` shows red and not the letter r](#why-m38-r-shows-red-and-not-the-letter-r)
- [What is and is not emulated](#what-is-and-is-not-emulated)
- [The panel](#the-panel)
- [The flip](#the-flip)
- [Configuration](#configuration)
- [Language](#language)
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

| Panel | Grid | px per module | Font | Verdict |
|---|---|---|---|---|
| 64 × 32 | 15 × 3 | 4 × 10 | — | 15 columns don't fit; auto-reduced to 10 |
| 64 × 64 | 15 × 3 | 4 × 21 | — | too narrow for 15 columns |
| **128 × 32** | **15 × 3** | **8 × 10** | **6×9** | the default. Tight, every glyph legible |
| 128 × 64 | 16 × 3 | 8 × 21 | 6×13 | roomy, with real bezels — but three rows leave the wall looking sparse |
| **128 × 64** | **15 × 5** | **8 × 12** | **6×10** | **75 modules — fills the panel. The one to pick for a 64-row chain.** |

Settings → *Geometry preset* offers each of these; picking one fills the Panel and Module Wall
cards and saves them. Power-cycle to apply (geometry is read once at boot).

**A 64-row panel halves the refresh rate**, because the same pixel clock now has twice the rows
to scan: ~157 Hz at 128 × 32 becomes **~78 Hz at 128 × 64**, and the framebuffer grows from 38 KB
to 77 KB of internal DMA RAM. Both are within budget — `panelBegin()` refuses anything that would
starve WiFi of internal SRAM (see [Known limitations](#known-limitations)) — but if you see
flicker, drop `panelBitDepth` to 3, which buys back refresh at the cost of colour depth.

The ceiling on the emulated wall is **`VM_MAX_MODULES` = 128** (`src/vmodule.h`), and the bus's
reply queue is sized to match, so a 15 × 5 = 75-module wall has room to spare. A grid that
exceeds the ceiling is quietly reduced, and the boot log says so.

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
| `gridRows` × `gridCols` | 3 × 15 | **on reboot** — this creates and destroys modules (up to `VM_MAX_MODULES` = 128) |
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

## Language

The dashboard speaks 13 languages. **One firmware image ships all of them** — switching
language never means reflashing, and English costs nothing extra because it *is* the text in
the page.

| | |
|---|---|
| English | `en` · `en-GB` · `en-AU` |
| Western Europe | `fr` · `de` · `es` · `it` · `pt` · `pt-BR` · `nl` |
| Nordics | `da` · `sv` · `nb` · `fi` |

The gateway stores **no language setting at all** — no config field, no NVS write, no API. The
choice is resolved in the browser, highest priority first:

1. **`?lang=fr` in the URL.** This lets the companion app request a language for an embedded
   view *without* touching the user's own preference, so it is deliberately not saved.
2. **The Settings → Language override**, kept in `localStorage` (per-device, like a bookmark).
3. **`navigator.languages`** — "Auto", the default. It follows the browser exactly the way the
   light/dark theme already does, so a French browser gets a French dashboard with no setup.
4. **English.**

A region falls back to its base (`fr-CA` → `fr`), and an unsupported language simply stays
English. `en-US` resolves to the **base** English, never to `en-GB` — the base is US spelling,
and `en-GB`/`en-AU` are thin diffs over it (`color` → `colour`).

Each language is one gzipped JSON dictionary in flash, fetched from `GET /lang/<code>` only if
it is the one being used. A key with no translation falls back to English on its own, so a
partial dictionary is a supported state rather than a broken page.

### Why these languages

The dashboard is UTF-8 and could render anything. The **flap modules cannot**: their glyphs are
Windows-1252 (see [The reel](#the-reel)). Translating the UI into a language whose alphabet the
wall itself can never show would be a promise the hardware cannot keep — so the language list is
scoped to what Windows-1252 covers.

### Changing the text

`ui/index.html` is the source of truth, and `src/web_ui.h` is **generated** from it:

```sh
python3 tools/i18n_extract.py --wrap   # English text  -> ui/strings/en.json (+ wrap composed messages)
python3 tools/i18n_context.py          # where each string appears -> ui/strings/CONTEXT.md
python3 tools/i18n_check.py            # validate every dictionary
python3 tools/build_ui.py              # ui/ -> src/web_ui.h        (--check in CI)
node    tools/i18n_test.js             # language-resolution regression test
```

Never edit `src/web_ui.h` by hand — the next `build_ui.py` overwrites it.

Strings are keyed by their **English text**, so the markup needs no `data-i18n` tagging: a DOM
walk plus a `MutationObserver` translates the static page *and* whatever the JS builds later
(module cards, the flap wall). Messages the JS *composes* (`"Error: " + e`) are wrapped in
`t()`, because a walk only ever sees the finished string. The bus monitor is deliberately
skipped — it shows protocol, not chrome.

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
src/web.*           HTTP server: dashboard (web_ui.h) + REST API + GET /lang/<code>
src/web_ui.h        GENERATED by tools/build_ui.py — do not edit
src/ota.*           firmware update: ArduinoOTA + browser upload     (unchanged)
src/tasks.*         the FreeRTOS task loops
src/main.cpp        setup() boot sequence + loop() watchdog supervisor

ui/index.html       the dashboard (HTML + CSS + JS) — the source of truth
ui/strings/en.json  the English string catalog, extracted from ui/index.html
ui/strings/*.json   one translation dictionary per language, keyed by the English text
ui/strings/CONTEXT.md  GENERATED — where each string appears, for translators

tools/build_ui.py   ui/ -> src/web_ui.h (page + gzipped dictionaries)
tools/i18n_extract.py  builds en.json; --wrap wraps composed JS messages in t()
tools/i18n_check.py    validates the dictionaries (stale keys, lost product name, encoding)
tools/i18n_context.py  builds CONTEXT.md
tools/i18n_test.js     regression test for language resolution and t()
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

After changing anything under `ui/`, regenerate the dashboard header — `pio run` compiles
`src/web_ui.h`, not `ui/index.html`, so skipping this silently builds the *old* page:

```sh
python3 tools/build_ui.py           # ui/ -> src/web_ui.h
python3 tools/build_ui.py --check   # or just assert it is up to date (CI)
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
