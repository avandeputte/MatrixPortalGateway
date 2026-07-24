# Matrix Portal Gateway

> ### 📖 [SplitFlap Wiki — the comprehensive documentation](https://github.com/avandeputte/SplitFlapGateway/wiki)
> Quick start · choosing a configuration · provisioning & calibration · the SplitFlap and
> Matrix Gateways · the companion and its apps · APIs and wire protocols — the whole
> ecosystem, documented in one place.


A split-flap display with no split flaps.

This is the [Split-Flap Gateway](../../SplitFlapGateway/3.1) v3.1 firmware, ported to a
**Waveshare ESP32-S3-RGB-Matrix driver board** driving a HUB75 RGB LED matrix. (Versions
up to v1.25.0 ran on the Adafruit MatrixPortal S3; that final MatrixPortal build lives on
the `matrixportal` git branch.) The physical gateway's
serial transceiver is gone. In its place is a wall of *virtual* split-flap modules, each one
receiving the same protocol frames a real module would, acting on the display commands
(`-`, `+`, `h`), and rendering itself as a flapping character cell on the panel.

Everything above the protocol seam is unchanged: the web UI, the REST API, OTA, the
command log. (MQTT and Home Assistant support existed through v2.2 and were removed in
v3.0 — unused here, and every byte of RAM counts on this board.) The
[companion app](../../SplitFlapGatewayCompanion) drives it without modification and cannot
tell the difference. (One thing above the seam is *gone* rather than unchanged — the sticky
module registry, which has no meaning on a wall that exists by construction.)

```
     ┌──────────────────────────────────────────────┐
     │  web UI · REST API · OTA · command log       │   from Gateway 3.1 (MQTT/HA removed in 3.0)
     ├──────────────────────────────────────────────┤
     │  frameSend()  framing · sanitization · Quiet  │   unchanged
     ├──────────────────────────────────────────────┤
     │  vmodule      45 virtual split-flap modules   │   new
     ├──────────────────────────────────────────────┤
     │  display      HUB75 flap renderer             │   new
     ├──────────────────────────────────────────────┤
     │  panel        LCD_CAM + GDMA HUB75 driver     │   new  (no external library)
     └──────────────────────────────────────────────┘
```

Per-release history — features, breaking changes, certification results — lives in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

---

## Table of Contents

- [What it does](#what-it-does)
- [Beyond the flaps: the pixel surface](#beyond-the-flaps-the-pixel-surface)
- [The reel](#the-reel)
  - [Why `m5-r` shows red and not the letter r](#why-m5-r-shows-red-and-not-the-letter-r)
- [What is and is not emulated](#what-is-and-is-not-emulated)
- [The panel](#the-panel)
- [The flip](#the-flip)
- [Configuration](#configuration)
- [Language](#language)
- [Compatibility](#compatibility)
- [Repository contents](#repository-contents)
- [Building](#building)
- [Known limitations](#known-limitations)

---

## What it does

On boot the firmware creates one virtual split-flap module per cell of the module wall
(15 × 3 = **45 modules** by default), with IDs `0`…`44` fixed by wall position. There is no
discovery, because there is nothing to discover: every module exists by construction, and the
array of them *is* the state of the wall. Nobody has to ask, and (since v1.24) nobody *can*
ask over the protocol — the wall self-describes through `/api/display/state`,
`/api/flap/modules` and `/api/capabilities` instead.

From then on, everything works. Send `m5-A` and module 5's reel flips forward until it lands
on `A`. Send text from the Display tab and it cascades across the wall. Every command the
gateway receives is recorded in the command log (`GET /api/log`).

The difference is that nothing is moving. The modules are software, and the panel is where
they live.

---

## Beyond the flaps: the pixel surface

Because the "wall" is really a framebuffer, the gateway exposes it directly alongside the
split-flap emulation — everything feature-detected through `GET /api/capabilities`, fully
specified in [openapi.yaml](openapi.yaml) and the
[wiki's Canvas page](https://github.com/avandeputte/SplitFlapGateway/wiki/Canvas):

- **Canvas mode** — push raw frames (`rgb888`/`rgb565`/QOI), partial rects, multi-rect
  deltas, or a JSON batch of draw ops (shapes, text in six sizes plus uploadable fonts,
  sprites); configure crossfade/wipe/slide **transitions** for full-frame presents.
- **The stream channel** (`PUT /api/canvas/stream`) — one long-lived connection carrying
  draw records back-to-back for animation-rate updates with no per-frame HTTP round trip.
- **The atlas library** — up to 16 named sprite sheets resident in PSRAM, optionally
  persisted to flash, blitted by index from draw ops or the stream.
- **On-device content** — a stored animation library (with GIF import) and boot
  animation, a scrolling ticker (exclusive or overlaid), and effects: plasma, fire,
  matrix rain, flip-o-rama, a clock, Game of Life.
- **Live events** (`GET /api/events`) — a Server-Sent Events stream carrying the display
  state the instant it changes plus a status heartbeat; the dashboard's live preview and
  its status pane ride it instead of polling.
- **Text is UTF-8 in, CP1252 flaps out** — ops/ticker text accepts the full Latin-1
  repertoire (`21° — Grüße`), transcoded on device.

The wall and the canvas hand the panel back and forth cleanly: canvas/effect/animation
modes park the flap renderer, and releasing them (`POST /api/canvas {"active":false}`)
returns whatever the reels last showed.

---

## The reel

Each virtual module carries **237 flaps**: 156 Windows-1252 glyphs, the seven colours, the
60 lowercase letters and the 14 pictographs.

A physical reel has 64 leaves because it is a physical object. These modules are *drawn*, so
there is nothing to ration — the reel simply carries **one flap for every character**, and any
Windows-1252 character you send has somewhere to land.

| Index | Contents | Reachable from |
|---|---|---|
| `0` | blank (the home position) | both |
| `0`–`155` | the CP1252 repertoire in code-point order, **minus the lowercase letters** | both |
| `156`–`162` | the seven colour flaps: `r o y g b p w` | both |
| `163`–`222` | the 60 **lowercase** letters | index only |
| `223`–`236` | 14 **pictographs**: ♥ ♦ ♣ ♠ ☺ ♪ ● ■ ⌂ ← ↑ → ↓ ☀ | index only |

The legacy sections come **first** and keep the indices they have always had, so growing the
reel can never move a flap an existing controller already addresses by number.

```
 !"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`{|}~€‚ƒ„…†‡ˆ‰Š‹ŒŽ‘’“”•–—˜™›Ÿ
¡¢£¤¥¦§¨©ª«¬®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞß÷roygbpw
```

The reel is **shared, complete and fixed**. It is not stored per module, and it is not
configurable: a drawn reel that can already render everything has nothing left to reconfigure,
so the `N` command and the flap-set editor are gone. (The split-flap gateway keeps both — its
reels are real, and what is printed on them is a fact about the hardware.)

It is **built at boot**, not typed out — from `isFlapByte()` and `cp1252IsLower()`, which
already own *which bytes exist* and *which bytes are lowercase*. Add a glyph to the font and
the reel grows a flap for it; it cannot drift from the font or from the folding rule. The
whole thing lives in `src/reel.h`, deliberately free of Arduino, so `tools/reel_test.cpp`
compiles the *same* code rather than a copy of it.

### Two ways in, and they are not the same

The legacy wire protocol carries **one byte** per character, and it has a problem it can never
solve: **the byte for lowercase `r` already means red.** The seven colour flaps are addressed
by `r o y g b p w` — that is the protocol, not a choice. So on that path lowercase *must* fold
to uppercase, and a heart — which has no Windows-1252 byte at all — cannot be addressed by
character in **any** way.

That is not a limitation of this firmware. It is a limitation of a one-byte alphabet that
spent seven of its letters on colours.

So there are two resolvers, and the reel has flaps the old protocol simply cannot name:

| | `m<id>-<char>` (legacy) | `POST /api/display/cells` |
|---|---|---|
| `r` | the **red** flap | the **letter** r |
| `a` vs `A` | both → `A` (folded) | two different flaps |
| `♥` | impossible — no byte exists | flap 223 |
| colours | the letters `r o y g b p w` | named: `{"color":"red"}` |

**The legacy protocol is untouched.** `m5-r` is still red, `hello` still renders `HELLO`, and
every existing controller works exactly as it did. The new endpoint is a *different way in*,
not a different reel:

```jsonc
POST /api/display/cells
{
  "start": 0,
  "step_ms": 15,
  "cells": [
    {"ch": "H"}, {"ch": "e"}, {"ch": "l"}, {"ch": "l"}, {"ch": "o"},
    {"ch": "♥"},              // a pictograph, drawn in its own red
    {"color": "red"},         // a colour flap, NAMED — 'r' here would be the letter r
    {"blank": true},          // home it
    {"skip": true}            // leave that module alone
  ]
}
```

It sends `m<id>+<n>` — the index command the modules have understood all along. Every cell is
resolved **before anything is sent**: a character the reel cannot show is a `400`, not a
silent blank, because a half-written wall is worse than a rejected request.

The pictographs come from the **bundled X11 fonts**, which already carry thousands of glyphs —
not from hand-drawn bitmaps. The only exception is `☀` at 5×8, which that face genuinely
lacks, so it is drawn by hand in `tools/genfont.py`. Each one can bring its own ink: a heart
that comes out white is not a heart.

### No lowercase *on the legacy path* — and that is load-bearing

The reel is printed in capitals, like a real one, so **lowercase folds to uppercase**
(`cp1252ToUpper`). That is not a stylistic choice. The seven colour flaps are addressed by the
**lowercase letters** `r o y g b p w` — that is the protocol. Put lowercase letters on the reel
and a lookup for `r` would find the *letter*, and every colour command ever written would
quietly start printing letters instead of colours.

Folding has traps, and `charset.cpp` owns all of them in one place: `ÿ` uppercases to `Ÿ`
(`0x9F`), **not** to `ß` — a naive `-0x20` turns a y-diaeresis into an eszett. `÷` is the
division sign, not a letter, so it keeps its own flap rather than folding onto `×`. `ß` has no
uppercase in CP1252 at all, so it keeps a flap too. `œ`, `š` and `ž` uppercase nowhere near a
`0x20` offset. `µ`, `ª` and `º` look lowercase but are symbols.

`tools/reel_test.cpp` pins every one of those down against the real code.

### Why `m5-r` shows red and not the letter r

`vmFlapIndexOf()` resolves a character in a fixed order, and the order *is* the contract:

1. **the colour codes first.** `r o y g b p w` are lowercase by protocol, not letters by
   meaning. Checking them before anything else is what guarantees `r` can only ever be red.
2. **`q`** — splitflap-os's legacy alias for the double-quote flap. The classic reel had no
   lowercase, so its char map borrowed that byte. This reel carries a real `"`, so the alias
   is honoured rather than the frame being dropped.
3. **fold to uppercase** (`cp1252ToUpper`) — the reel is printed in capitals.
4. **scan the 156 glyph flaps** — never the colours, which step 1 already owns.

```
m5-r      → flap 156  the RED colour flap   (colour codes are never folded)
m5-R      → flap 50   the letter R
m5-a      → flap 33   folded to 'A'         (the reel has no lowercase letters)
m5-é      → flap 132  folded to 'É'
m5-$      → flap 4    used to be a blank — the old 64-flap reel had no '$'
m5-q      → flap 2    the '"' flap          (the legacy alias)
m5+0      → flap 0    blank
```

So `hello` still renders `HELLO`, `m5-r` still means red, and **every existing colour command
works unchanged** — while 99 characters that used to come out blank now display.

---

## What is and is not emulated

**The protocol is emulated where it is used.** The firmware acts on the three commands the
gateway still emits: display (`-`, `+`) and homing (`h`), addressed to one module or broadcast
(`m*`, and the two-star v6 form). Everything else in the physical grammar — the
calibration/dump family, the removed `v`/`A` queries, by-serial `mX…` addressing — passes
through the sanitizer untrimmed and is silently ignored: there is nothing to calibrate,
nothing to dump, and (since v1.24) nothing that answers a question the REST API does not
already answer better.

**The mechanism is not emulated at all.** There is no stepper, no Hall sensor and no EEPROM.
Nothing can be out of tune, so nothing needs tuning: the calibration, diagnostics, provisioning
and backup commands are ignored rather than faked — no reply, no state. `h` simply shows flap
0, the blank.

**The wire is not emulated either.** The physical gateway's half-duplex serial wire at
9600 baud is slow — a broadcast query across 45 modules once took seconds of staggered reply
slots. Here a frame is a function call: delivery is instant, one-way, and **nothing ever
replies**, so there is no reply timing to emulate and no collisions to worry about. The
command log (`GET /api/log`) records the outbound frames, which are the whole of the
traffic.

---

## The panel

The module grid *is* the wall. Cell size falls out of it:

```
cellW = panelW / gridCols          cellH = panelH / gridRows
```

and the renderer then picks the roomiest bundled font that fits with a one-pixel seam. **Seven**
faces are bundled — `10x20`, `9x18`, `8x13`, `6x13`, `6x10`, `6x9` and `5x8` — all carrying the
full 216-glyph CP1252 set with real diacritics, plus the 14 pictographs. (`5x7` and `4x6` are
deliberately absent: at those sizes the source face has no room for accents and draws `À`
identically to `A`, which on this reel is a correctness bug, not an aesthetic one.
`tools/genfont.py` rejects any face that does this.)

The three big faces exist because a **256px-wide chain makes big cells possible**: a 15×3 wall
on 256×64 is **17×21 px per module**, roughly three times the area of the 8×12 cells a 128-wide
chain gives you — and `6x13` simply floats in that much room.

They were not possible before v1.8, for a reason worth knowing: the glyph rows were packed
**one byte per row**, which silently capped every face at **8 pixels wide**. That is the whole
reason nothing bigger than `6x13` had ever been bundled — not a design choice, a packing limit.
Rows are 16-bit now. (`render()` in `genfont.py` *raised* on a wider glyph rather than dropping
its right-hand columns, so the limit never shipped as a silent corruption — it just quietly
stopped anyone from trying.)

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
| 128 × 64 | 10 × 3 | 12 × 21 | **10×20** | 30 big, detailed flaps |
| **256 × 64** | **15 × 3** | **17 × 21** | **10×20** | **the biggest, most detailed flaps this firmware can draw.** Depth 3 |
| 256 × 64 | 32 × 5 | 8 × 12 | 6×10 | 160 modules — a dense wall two panels wide. Depth 3 |

**A 256px chain must run at colour depth 3.** At depth 4 the framebuffer needs 144 KB of
internal DMA RAM, over the 120 KB budget — and since 1.17.1 `panelBegin()` **clamps the depth
down** to the deepest that fits (256 × 64 lands on depth 3) rather than refusing and running
headless; it logs the clamp, and `GET /api/status` reports the depth actually running. Depth 3
needs 102 KB and refreshes at **~85 Hz**, which is actually *better* than a 128 × 64 wall at
depth 4 (79 Hz). The geometry presets carry the depth for you, so the clamp never has to fire.

Settings → *Geometry preset* offers each of these; picking one fills the Panel and Module Wall
cards and saves them. Power-cycle to apply (geometry is read once at boot).

**A 64-row panel halves the refresh rate**, because the same pixel clock now has twice the rows
to scan: ~157 Hz at 128 × 32 becomes **~78 Hz at 128 × 64**, and the framebuffer grows from 38 KB
to 77 KB of internal DMA RAM. Both are within budget — `panelBegin()` steps the bit depth down
until the framebuffer no longer starves WiFi of internal SRAM, refusing outright only if even
depth 1 will not fit (see [Known limitations](#known-limitations)) — but if you see flicker,
drop `panelBitDepth` to 3, which buys back refresh at the cost of colour depth.

The ceiling on the emulated wall is **`VM_MAX_MODULES` = 192** (`src/vmodule.h`), so a
32 × 5 = 160-module wall on a 256px chain still has room to spare. A grid that exceeds the
ceiling is quietly reduced, and the boot log says so.

### If every colour is wrong

Some HUB75 panels are wired **BGR**, not RGB. That is the panel's own hardware colour order,
and nothing in the firmware can detect it. On a BGR panel every colour comes out wrong in a
very specific pattern:

| you ask for | a BGR panel draws |
|---|---|
| red | **blue** |
| blue | **orange** |
| yellow | **cyan** |
| purple | **pink** |
| **green** | green — correct |
| **white** | white — correct |

Green and white look perfectly normal, because green is its own channel and white is all
three. That is exactly why this hides for so long: **text is white**, so nothing looks wrong
until the first time you draw a colour.

Tick **Settings → LED Panel → "Panel is wired BGR"**. It takes effect on the next frame — no
reboot — so you can tick it and see the answer immediately. It is stored per board, because
the next panel may well be RGB.

The swap happens in `panelPixel()`, the single choke point every pixel passes through
(`panelHLine`, `panelVLine` and `panelFillRect` all funnel into it), rather than by
re-mapping the pins: the pin map is correct and matches Waveshare's own reference map for
this board.

A 64-row panel needs the E address line, which is GPIO 9 on this board — wired directly on the
HUB75 header, no jumper to set. All five address lines are always mapped; the panel height
decides how many are actually clocked, so a 1/16-scan 32-row panel simply never drives E.

---

## The flip

Changing the displayed flap cascades forward through the reel one flap at a time. This is a
*rendering effect*, not a simulation.

`flapMax` caps one change at **64 flips** (a physical reel's full revolution, kept as the cap
even though this reel has 237 flaps); a longer jump starts its walk
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
| `gridRows` × `gridCols` | 3 × 15 | **on reboot** — this creates and destroys modules (up to `VM_MAX_MODULES` = 192) |
| `panelW` × `panelH` | 128 × 32 | **on reboot** — the panel driver takes geometry at init |
| `panelBitDepth` | 4 | **on reboot** — 1…6 RAM and EMI scale with it |
| `panelBGR` | false | next frame — see [If every colour is wrong](#if-every-colour-is-wrong) |
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
(the flap wall, the quiet-day checkboxes). Messages the JS *composes*
(`"Error: " + e`) are wrapped in
`t()`, because a walk only ever sees the finished string.

---

## Compatibility

The companion app's *core* contract is seven HTTP endpoints, and it models *nothing*
about a module — not the flap count, not the character set, not serial numbers. The
reel is invisible to it. Those seven are
`GET /api/config`, `GET /api/status`, `POST /api/frames/send`, `POST /api/frames/batch`,
`POST /api/companion`, and `GET`/`PUT /api/companion/settings`. (Since v1.22.0 the
`/api/frames/*` pair is the only send surface — the physical gateway's paths it once
aliased are gone.) Beyond that core the companion *feature-detects* through
`GET /api/capabilities`: on this gateway it finds and uses the index-addressed cell
surface (`POST /api/display/cells`) and the whole canvas family — QOI frames, delta
rects, the stream channel, the atlas library, transitions.

Two things do matter, and both are handled:

1. **`GET /api/config` must report `version >= 3.1`.** The companion parses `MAJOR.MINOR` out of
   it and gates its gateway-stored settings on `>= (3,1)`. This firmware implements that surface
   exactly, so it answers `3.1.0` and puts its own version in `fwVersion`.
2. **`POST /api/frames/send` and `/api/frames/batch` must forward frame bytes verbatim.** The
   companion sends `m00-A\n` style frames as `windows-1252`, one byte per glyph. They are not
   transcoded.

The `/api/companion/settings` gzip blob store is carried over untouched. (The physical
gateway's MQTT surface and Home Assistant discovery are **not** — they were removed from
this firmware in v3.0.)

---

## Repository contents

```
src/common.h        board config, panel defaults, buffer sizes, shared types
src/gateway.h       umbrella header: common.h plus every subsystem's public API
src/globals.cpp     single definition site for every shared global
src/config.*        runtime configuration (GwConfig) persisted in NVS
src/rtc.*           wall-clock time: the system clock, seeded by the PCF85063 battery
                    RTC at boot and disciplined by NTP (which writes the chip back)
src/charset.*       UTF-8 <-> Windows-1252 flap-byte transcoding
src/reel.h          the 237-flap reel and its two resolvers — Arduino-free, so
                    tools/reel_test.cpp compiles the same code
src/font1252.*      GENERATED bitmap glyphs: the 216 printable CP1252 flaps + 14 pictographs
src/aafont.h        GENERATED by tools/genaafont.py — Orbitron faces for the clock effect
src/frames.*        frame sanitization, the command log, the frameSend() choke point,
                    scheduled batch pacing
src/vmodule.*       the virtual split-flap modules: protocol dispatch and the shared reel
src/display.*       flap-wall geometry and the flap renderer (calls panel.*)
src/canvas.*        raw canvas: frames, rects, QOI decode, draw ops, on-device animation + ticker,
                    the animation/font libraries, transitions, sprite atlas, GIF import
src/effects.*       on-device effects: plasma, fire, matrix, flip-o-rama, clock, Life
src/panel.*         the low-level HUB75 driver: ESP32-S3 LCD_CAM + GDMA, no library
src/modules.*       high-level protocol send helpers (text/char/home) + FATFS mount
src/httpx.*         the native esp_http_server layer: route table, dispatch hook (CORS,
                    watchdog stamps), JSON/chunk/query/body helpers, heap-graded recv pacing
src/sse.*           GET /api/events: Server-Sent Events slots + the shared push buffer
src/web.*           HTTP server: dashboard (web_ui.h) + REST API + GET /lang/<code>
src/web_ui.h        GENERATED by tools/build_ui.py — do not edit
src/ota.*           firmware update: raw-body browser/curl upload + mDNS
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
tools/fontpack.py   packs a BDF into an MPFT blob for PUT /api/canvas/font
tools/genaafont.py  regenerates src/aafont.h from Orbitron (the clock effect's faces)
tools/Orbitron.ttf  vendored Orbitron variable font (SIL Open Font License)
tools/bdf/          public-domain X11 "misc-fixed" fonts (10x20, 9x18, 8x13, 6x13, 6x10, 6x9, 5x8)
tools/reel_test.cpp native regression test for the reel and its two resolvers

platformio.ini      build/upload configuration (single env: waveshare_matrix)
partitions-32MB.csv 4 MB app0 + 4 MB app1 + 23.9 MB FATFS — no tinyuf2 slot
ARCHITECTURE.md     why the non-obvious decisions were made
openapi.yaml        REST API reference
```

`modules.*` is the gateway's side of the module protocol — how a character, an index or a home
becomes a frame. `vmodule.*` is what acts on those frames. They talk only through protocol
frames, which is exactly why the port works. (Upstream, `modules.*` also holds a *registry* of
whatever is out on its serial wire. That has no meaning on a drawn wall and was removed in 1.10.)

---

## Building

One PlatformIO environment, `waveshare_matrix` (32 MB octal flash, `memory_type = opi_opi`,
`partitions-32MB.csv`):

```sh
pio run                 # build
pio run -t upload       # flash over USB (esptool)
pio device monitor      # 115200 baud, native USB CDC
```

**The first build recompiles the Arduino core** (v3.3.0): `platformio.ini` carries a
`custom_sdkconfig` block that rebuilds the core libraries with WiFi/lwIP buffers in PSRAM
(see RELEASE_NOTES.md and ARCHITECTURE.md for why). Expect ~10–20 minutes and network
access (IDF component fetches) the first time; later builds reuse the compiled core. If a
core rebuild fails half-way, wipe its artifacts and retry:

```sh
rm -rf .pio/build managed_components sdkconfig.defaults sdkconfig.waveshare_matrix
pio run
```

To return to the stock prebuilt core, delete the `custom_sdkconfig` block and do the same
wipe — no source changes are needed.

There is **no UF2 bootloader** on this board, so no drag-and-drop recovery: if a flash goes
wrong, hold **BOOT** while plugging in USB and run `pio run -t upload` (esptool) again. Once a
working firmware is on, web OTA (`/ota`) updates it as before.

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
c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp src/font1252.cpp \
    -o /tmp/reel_test && /tmp/reel_test
```

---

## Known limitations

- **The pixel clock stays at 5 MHz — but for a different reason than before.** On the
  MatrixPortal, 10 MHz starved the WiFi MAC of internal-SRAM bandwidth and broke association
  outright. On this board a 10 MHz A/B (2026-07-18) showed the **radio survives** — instant
  association, 0% ping loss, normal HTTP latency — but the thing 10 MHz would buy, depth 4 on a
  256×64 chain at ~80 Hz, is blocked by RAM instead: that geometry's 144.6 KB double-buffered
  internal framebuffer left 26 KB of free heap and a 1.7 KB minimum, which is unshippable. So
  `LCD_CLK_HZ` (`src/panel.cpp`) stays 5 MHz, where refresh is ample and the radio has margin.
  Future direction: if the driver ever grows single-buffering or PSRAM bounce buffers, 10 MHz +
  depth 4 is on the table on this board. WiFi modem sleep remains disabled (`src/main.cpp`).
- **The framebuffer does not go in PSRAM — even 16 MB of octal PSRAM.** The panel's GDMA stream
  and WiFi share the PSRAM/cache bus, and bounce-buffering frames through internal RAM would put
  the CPU back into the refresh loop the DMA design exists to keep it out of. So `panel.cpp`
  allocates the double-buffered framebuffer and the DMA descriptor chain from internal
  DMA-capable RAM (`MALLOC_CAP_DMA`), which bounds panel size and colour depth together
  (`PANEL_RAM_BUDGET` stays 120 KB), and `panelBegin()` clamps the bit depth down until the
  framebuffer stops starving WiFi of that pool — refusing only a geometry that will not fit even
  at depth 1. The virtual-module array is pinned to internal RAM too — `taskDisplay` walks it
  100×/s on the core the refresh runs on, and a PSRAM cache miss there causes a shimmer. (The
  command log, the scheduled-TX ring, the animation store and the atlas library *are* in
  PSRAM — and since v3.3.0 the WiFi/lwIP buffers are too; nothing in the refresh path
  touches any of them.)
- **Wall-clock time needs a backup cell or a network.** The PCF85063 seeds the system clock at
  boot only when it holds a plausible time; with no cell fitted (or a rejected/implausible
  reading), time is invalid from power-on until the first NTP sync. Every caller already
  handles that state; frame timestamps show `HH:MM:SS` uptime until then.

---

## Licence

CC BY-NC-SA 4.0, as the upstream Split-Flap Gateway. Split-flap module hardware and the initial
protocol by [Adam G Makes](https://www.youtube.com/@AdamGMakes). The bundled bitmap fonts are
the X11 `misc-fixed` faces: *"Public domain font. Share and enjoy."*
