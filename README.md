# Matrix Portal Gateway

> ### 📖 [SplitFlap Wiki — the comprehensive documentation](https://github.com/avandeputte/SplitFlapGateway/wiki)
> Quick start · choosing a configuration · provisioning & calibration · the SplitFlap and
> Matrix Gateways · the companion and its apps · APIs and wire protocols — the whole
> ecosystem, documented in one place.


A split-flap display with no split flaps.

This is the [Split-Flap Gateway](../../SplitFlapGateway/3.1) v3.1 firmware, ported to an
**Adafruit MatrixPortal ESP32-S3** driving a HUB75 RGB LED matrix. The RS-485 transceiver is
gone. In its place is a software bus and a wall of *virtual* split-flap modules, each one
emulating the real module firmware's wire protocol byte for byte and rendering itself as a
flapping character cell on the panel.

Everything above the bus is unchanged: the web UI, the REST API, MQTT and Home Assistant
discovery, OTA, the bus monitor. The
[companion app](../../SplitFlapGatewayCompanion) drives it without modification and cannot
tell the difference. (One thing above the bus is *gone* rather than unchanged — the sticky
module registry. See **New in 1.10**.)

```
     ┌──────────────────────────────────────────────┐
     │  web UI · REST API · MQTT · OTA · monitor     │   unchanged from Gateway 3.1
     ├──────────────────────────────────────────────┤
     │  busSend()    framing · sanitization · Quiet  │   unchanged
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

## New in 1.20

- **The last "RS-485" is out of the code.** There is no RS-485 transceiver on this board, and
  now there is no `rs485` in the source either: `src/rs485.*` is **`src/bus.*`**, `rs485Send()`
  is `busSend()`, `taskRS485` is `taskBus`, `/api/status` reports the task's stack as
  `stk.bus`, and the Home Assistant stack sensor is `stkbus` (the legacy `stk485` discovery
  topic is deleted on connect, so HA is not left holding a ghost sensor). The REST endpoints
  are canonically **`/api/bus/send`** and **`/api/bus/batch`**; `/api/rs485/send` and
  `/api/rs485/batch` stay as compatibility aliases, because the companion app still POSTs to
  them. "RS-485" survives in prose exactly where it should: when it names the *other* product,
  the physical gateway.

- **Virtual-module persistence is gone — nothing it stored could vary.** Every field of
  `/vmods.dat` is deterministic: a module's id *is* its wall slot, its serial derives from the
  board's MAC, and the reel homes at boot. A physical module needs its EEPROM; a drawn one
  re-derives everything in `vmInit()`, which now also deletes a leftover `/vmods.dat`. FATFS
  holds exactly one file: the companion's settings blob, `/compset.gz`.

- **The frame sanitizer's grammar was trimmed to the commands this product speaks:** `-`, `+`,
  `h`, `v` and `A`, plus the by-serial `mX…` passthrough — what the companion, the web UI and
  MQTT emit. The physical gateway's calibration/dump family is no longer modelled: those
  frames pass through untrimmed, and the virtual modules silently ignore them.

- **`POST /api/flap/index` accepts the whole reel — `0`…`236`.** It previously stopped at 63,
  a leftover of the 64-flap reel: the lowercase and pictograph flaps were reachable through
  `/api/display/cells` but not by bare index, even though they are the *reason* index
  addressing exists.

- **`sfSendText` no longer sleeps 10 ms per character.** That delay was physical-bus pacing;
  on the emulated bus a send is a function call, so the sleep only froze the calling HTTP or
  MQTT task — up to 2.5 s per text. The visible cascade was never the delay's job: the flip
  animation provides it.

- **MQTT publishes now happen outside the queue mutex**, and the Home Assistant device
  reports its manufacturer.

## New in 1.19

- **Read the panel back — `GET /api/canvas/frame`.** A screenshot of whatever is on screen (wall,
  effect, canvas, animation, ticker), reconstructed from the framebuffer as raw pixels — `rgb888`, or
  `?fmt=rgb565`. The colours come back **quantised to the panel's real bit depth**, so it's what is
  physically lit, not the intended image (brightness, an OE duty cycle, isn't in the framebuffer, so
  it isn't reflected). Read-only — it never parks or disturbs the running mode — so a UI can poll it
  for a live preview. `X-Canvas-Width/Height/Format` headers describe the body. Verified by round-trip:
  a solid frame reads back bit-exact at every pixel.

## New in 1.18

- **The canvas does more, over far less WiFi.** Five additions, all Matrix-only (the RS-485 wall has
  no framebuffer):

  - **`PUT /api/canvas/rect`** — update *one rectangle* instead of resending the whole panel. Body is
    an 8-byte big-endian header `[x, y, w, h]` (u16 each) then `w × h` pixels, `rgb888` or `rgb565`
    (by length). It is drawn on top of what is already on screen — the back buffer is synced to the
    live frame first — so animating a small area costs only that area's bytes.
  - **`PUT /api/canvas/qoi`** — a full-panel image, [QOI](https://qoiformat.org)-encoded. Lossless,
    decodes in one pass, and typically 2–4× smaller than raw (a 256×64 gradient: ~16 KB vs 49 KB).
  - **`PUT /api/canvas/anim`** — upload a short loop *once* and it plays on-device from PSRAM, so the
    client can disconnect. Body is a 14-byte `MPGA` header (`ver, fmt, fps, flags, w, h, frames`)
    then the frames back-to-back. The 2 MB PSRAM the framebuffer can't use holds ~48 frames at
    256×64. `taskDisplay` plays it at the set fps; a split-flap command or Quiet Time stops it.
  - **`POST /api/canvas/ticker`** — `{"text", "color":[r,g,b], "speed":1-20}` scrolls one line of
    text across the panel on-device, no streaming. Empty text hands the panel back.
  - **Effect parameters** — `POST /api/canvas/effect` now takes optional `hue` (0–255) and `density`
    (1–100): recolour the matrix rain, tint plasma and Life, set the Life seed % or flip-o-rama
    churn. Omit them and every effect looks exactly as before.

  `GET /api/canvas` and `/api/capabilities` advertise all of it (`rect`, `anim`, `ticker`, the `qoi`
  format, and `effectParams`), so a client reads the feature set instead of sniffing the version.

- **A too-deep panel dims itself instead of going blank (1.17.1).** A 256×64 at bit depth 4 needs
  more internal DMA RAM than the driver will spend before WiFi; rather than refuse and run headless,
  it now steps the depth down to the deepest that fits (256×64 lands on depth 3) and lights up.
  `GET /api/status` reports the actual running `panel.depth`.

## New in 1.15

- **Three more on-device effects — flip-o-rama, clock, and Game of Life.** They join plasma, fire
  and matrix, all started with `POST /api/canvas/effect` and rendered on the display task at the
  panel's native rate:
  - **`fliporama`** — the whole board flips through random glyphs, like a live departure board.
  - **`clock`** — a digital clock with big **HH:MM** in a bundled **Orbitron** face, each digit
    centred in its own slot so the proportional numbers never jitter, a tight dot colon, drifting
    through a rainbow, with the spelled-out date and a seconds bar that shrinks over the minute. It
    adapts to the panel: HH:MM + date + bar on a tall wall, HH:MM + bar on a 128×32. The face is
    generated by `tools/genaafont.py` from Orbitron (SIL Open Font License, vendored in `tools/`)
    as 1-bit masks — grayscale anti-aliasing reads as muddy on the panel's few bitplanes, so every
    edge is hard.
  - **`life`** — Conway's Game of Life on a wrapped grid, cells coloured by age, reseeded when it
    settles.

- **The clock shows the right time — a timezone bug is fixed.** Every NTP sync called
  `configTime(0, 0, …)`, which resets the `TZ` environment to UTC and so silently clobbered the
  zone set on the Settings page — the bus-log timestamps, Home Assistant discovery and the new
  clock all reverted to UTC after the first sync. `rtcNTPSync()` now re-applies the configured
  zone right after each sync, so local time sticks across syncs and reboots.

---

## New in 1.14

- **On-device effects — smooth animation the panel renders itself.** Pushing frames over HTTP
  (raw canvas, above) tops out around **8 fps**: the web server closes the socket after every
  request, so each frame pays a fresh TCP connection and slow-start, and that fixed cost — not
  bandwidth — is the wall (rgb565 frames are no faster than rgb888). So animation moved *onto*
  the gateway. **`POST /api/canvas/effect {"type":"plasma|fire|matrix","speed":1-10}`** runs the
  animation on the display task at the panel's native **~70 fps**, with nothing on the network:

  - **`plasma`** — a flowing rainbow sine-interference field.
  - **`fire`** — bottom-up flames (a Doom-style spread: random sideways drift plus decay makes
    tongues, not a smooth wash) through a black→red→orange→yellow→white palette.
  - **`matrix`** — falling green streaks, bright heads and fading trails, per-column speeds.

  An effect is a third display mode beside the wall and the raw canvas: it owns the panel until
  `{"type":"none"}` (or `POST /api/canvas {"active":false"}`) hands it back. All integer/LUT work
  — a sine table, two palettes, a PSRAM heat buffer — so a full frame fits inside one refresh.

  Both are **advertised in `GET /api/capabilities`** — a `canvas` object (formats + panel size),
  an `effects` array, and `canvas`/`effects` tokens in `features` — so the companion lights up
  the right controls from capabilities, never from a firmware-version sniff. The Split-Flap
  Gateway, which has no framebuffer, answers the same URL without those keys.

- **rgb565 raw frames actually work now.** `PUT /api/canvas/frame` inferred its pixel format from
  the `?fmt=` query arg, but a raw-body handler cannot read URL args — the WebServer discards them
  once it starts streaming the body — so `fmt=rgb565` silently fell back to rgb888 and every
  16 KB frame was rejected as the wrong length. The format is now taken from the **body length**
  (`W×H×3` = rgb888, `W×H×2` = rgb565), which is unambiguous and needs no query arg.

---

## New in 1.13

- **Raw canvas — the panel with the flaps taken off.** Every cell of this wall is *drawn*, so
  under the split-flap costume it was always just a HUB75 framebuffer. Three new endpoints hand
  you that framebuffer directly, bypassing the wall entirely — something the physical gateway
  cannot offer, because it has no framebuffer to expose, only motors:

  - **`PUT /api/canvas/frame?fmt=rgb888|rgb565`** — a full frame as raw pixel bytes,
    `width × height`, row-major, top-left origin. Render whatever you like on the client and push
    it. The body is **streamed straight to the back buffer**, so a 256×64 frame is never buffered
    whole; its length must equal `width × height × bytesPerPixel` *exactly*, or it is a `400`.
    `rgb565` is 2 bytes, big-endian.
  - **`POST /api/canvas/ops`** — a JSON array of draw commands for shapes and labels without
    composing a frame client-side: `clear`, `pixel`, `hline`, `vline`, `rect` (outline or filled),
    `text` (the bundled CP1252 faces, sizes 8–20) and `show`. Colours are `[r, g, b]`; off-panel
    pixels are dropped, not clamped or crashed.
  - **`GET /api/canvas`** reports the panel size and whether a canvas is active; **`POST`** with
    `{"active": true|false}` takes over and blanks, or releases. Pushing a frame or ops takes over
    on its own, so this call is only for blank-and-hold, or hand-back.

- **The reel renderer stands down while a canvas is up.** `gCanvasMode` makes `taskDisplay` yield
  the panel exactly as it does during an OTA — so the HTTP handlers own every pixel and nothing
  repaints the wall from under them, and nothing writes the back buffer they are drawing into.
  Releasing marks the display dirty and the wall comes back from the modules' current state.
  Nothing is persisted: a reboot returns to the wall.

---

## New in 1.9

- **The wall comes up blank after a reboot.** `autoHome` defaults to true, `'A'` reports it,
  and a *real* module with the flag set homes on power-up — but the emulation stored the flag,
  reported it, and then quietly ignored it, so `/vmods.dat` faithfully restored whatever
  half-finished sentence was on the wall when the power went. Stale content presented as
  current. The flag is honoured now.

- **The Companion URL is no longer editable.** The companion registers it itself. It is shown,
  read-only, on the **Status** tab.

- **A dead-code audit, after everything that has been removed.** Gone: the RS-485 serial
  parameters (baud, data bits, parity, stop bits — there is *no UART*, and the only reader was
  a debug line that literally called them "cosmetic"), `POST /api/config/rs485`,
  `/api/flap/version`, `/api/flap/all`, `/api/flap/identify`, the `gDump` capture mailbox and
  the two query-and-wait helpers behind it. Every one of them existed to ask the emulated bus
  for a **compile-time constant**.

- **`-Wall` is on now, and the build is warning-clean.** Turning it on found a **real latent
  null dereference**: `sfUpsert(id, NULL)` forwards that `NULL` into `strcmp()` whenever
  `id == 255`. Nothing on this firmware produces id 255 — so the only thing standing between
  that call and a crash was an invariant nobody had written down.

---

## New in 1.8

- **256 × 64 panels.** Verified on hardware: a 32 × 5 wall = **160 modules** at ~85 Hz.
  `VM_MAX_MODULES` went 128 → 192, because a 256px chain at 8px cells is 32 columns and the
  old ceiling would have *silently* shrunk it to 25 × 5. Must run at **colour depth 3** —
  depth 4 is over the RAM budget and refused (and would flicker at 40 Hz anyway). The presets
  carry the depth so you cannot pick a layout that gets refused.

- **Three much bigger fonts: `10x20`, `9x18`, `8x13`.** A 15 × 3 wall on 256 × 64 gives
  **17 × 21 px cells** — three times the area of a 128-wide chain's — and `6x13` floated in
  them. The blocker was not the fonts: glyph rows were packed **one byte per row**, which
  capped every face at **8 pixels wide**. Rows are 16-bit now. +32 KB flash, no RAM cost.

- **`128 × 64 · 10×3` got better for free.** Its 12 × 21 cells were always big enough for a
  10×20 face — there simply wasn't one. Now there is.

---

## New in 1.7

- **The Modules tab is gone.** It made sense on the RS-485 gateway, where modules are real
  things that show up, go missing, carry a serial and hold an EEPROM. Here every cell of the
  wall *is* a module, all of them are always present, none has a serial worth reading and none
  has an EEPROM at all — so the page could only ever say the same thing seventy-five times.
  **Display** is the landing tab now.

  The **`/api/flap/modules` endpoint stays** — the companion reads it to learn the wall, and
  the Status tab still counts them. It is the *page* that had nothing to say, not the data.

---

## New in 1.10

- **The module registry is gone.** It was the last piece of the RS-485 gateway that had no
  meaning here, and it was actively wrong. A registry answers "what is out there on the bus?" —
  a real question when the bus is real and modules can be added, removed, renumbered or simply
  fail to answer. On this board the wall is *drawn*: `vmInit()` creates every module from
  rows × cols, none can appear or vanish, and `vmods[i].curIndex` **is** the flap on show. The
  registry was a second copy of that, and a worse one.

  Worse in a way you could see. It stored one **byte** per cell, so entering and leaving Quiet
  Time — which snapshots the wall and replays it — sent every cell back through the legacy
  uppercase fold:

  ```
  before quiet :  Hello world!é♥
  after quiet  :  HELLo worLD!É?      <- lowercase folded; the heart could not come back at all
  ```

  A byte *cannot* name a flap on a 237-flap reel: seven of the letters are colour codes, and a
  pictograph has no byte at all (that is the whole reason `/api/display/cells` addresses flaps
  by index). The pending flap now lives in the module itself, as an `int16_t` index, and a Quiet
  Time cycle round-trips the wall exactly.

  Everything that used to read the registry now reads the modules directly: `/api/display/state`,
  `/api/flap/modules`, `/api/status`, the MQTT display sensor and the Quiet Time snapshot. Gone
  with it: `/modules.dat` and its FATFS persistence, the stale-entry pruner, the boot `m*v`
  discovery broadcast and the reconciliation added in 1.4 to paper over the bug it caused,
  `sfMutex` (the lock order is now `txMutex → vmMutex`), and `sfQueryVersion()`, which nothing
  had called in some time.

- **`/api/display/state` reports code points, not bytes.** A pictograph flap has no CP1252 byte,
  so a byte-shaped read rendered a heart as `"?"` — the same blindness as the registry, one layer
  up. The wall now reads back what it is actually showing. `/api/flap/modules` reports its
  `flapChar` the same way.

- **`GET /api/capabilities`** — one call that answers *what characters can this display show?*
  The RS-485 gateway answers the same URL with the same shape, so a client never has to know
  which kind of wall it is talking to. Here the answer is always `uniform`: every cell renders
  from one drawn reel, so `union` and `common` are the same 230 characters, and the seven colour
  flaps are reported by name under `colors` rather than as the letters `roygbpw`. ~1.6 KB for a
  75-module wall.

---

## New in 1.6

- **Lowercase, and emoji.** The reel grows to **237 flaps**: the 156 Windows-1252 glyphs and
  7 colours as before, plus the **60 lowercase letters** and **14 pictographs**
  (♥ ♦ ♣ ♠ ☺ ♪ ● ■ ⌂ ← ↑ → ↓ ☀).
- **A new endpoint to reach them: `POST /api/display/cells`.** The legacy protocol carries one
  byte per character and *cannot* express either — the byte for `r` already means red, and a
  heart has no byte at all. The new endpoint addresses flaps **by index** and names colours
  explicitly. See [Two ways in](#two-ways-in-and-they-are-not-the-same).
- **The legacy protocol is untouched.** `m5-r` is still red, `hello` still renders `HELLO`, and
  the legacy flap indices never moved.
- **A colour-order setting for BGR panels.** Some HUB75 panels are wired BGR, and on those
  *every colour the firmware has ever drawn has been wrong* — red draws blue, blue draws
  orange, yellow draws cyan — while green and white look fine, which is why it hides. See
  [If every colour is wrong](#if-every-colour-is-wrong).
- **Fixed: the pictographs drew nothing at all.** `font1252Row()` bounds-checked the glyph
  index against `FONT1252_GLYPHS` (216), and the pictographs live at 216–229. The reel
  resolved, the frame went out, the module flipped to the right flap, and the renderer
  silently returned a blank row for every one of them. A bounds check that is one constant
  out of date does not crash — it erases.

---

## New in 1.5

- **The reel carries every Windows-1252 character.** 64 flaps became **163** — 156 glyphs +
  7 colours — so the 99 characters that used to come out blank (`$ % ( ) + < > [ ] { } © ° «
  » ¿ À Á Â Ã Å Æ Ç È É Ê Ë …`) now display. A physical reel has 64 leaves because it is a
  physical object; these are drawn, so there is nothing to ration.
- **The flap set is no longer editable, and no longer stored per module.** A reel that can
  already render everything has nothing to reconfigure, so the `N` command, `POST
  /api/flap/flapconfig`, the `<prefix>/flap/flapconfig` MQTT topic and the flap-set editor
  are all gone. That also handed back **5.1 KB of internal RAM** on a 75-module wall
  (`sizeof(VModule)` 108 → 40 bytes) — the same RAM the panel framebuffer and WiFi contend
  for.
- The reel is now **built at boot** from the font's own repertoire and the folding rule,
  rather than typed out, and lives in `src/reel.h` — free of Arduino, so
  `tools/reel_test.cpp` compiles the *same* code instead of a copy of it.

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

  *(Superseded in 1.10: the registry is gone, and with it this bug's entire habitat. The wall
  is the modules; there is nothing left to reconcile.)*

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
(15 × 3 = **45 modules** by default), with IDs `0`…`44` fixed by wall position. There is no
discovery, because there is nothing to discover: every module exists by construction, and the
array of them *is* the state of the wall. They still answer `m*v` with a firmware version and
serial number — the protocol is emulated faithfully — but nobody has to ask.

From then on, everything works. Send `m5-A` and module 5's reel flips forward until it lands
on `A`. Send text from the Display tab and it cascades across the wall. The Monitor logs
every command the gateway receives.

The difference is that nothing is moving. The modules are software, and the panel is where
they live.

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

**The protocol is emulated where it is used.** The firmware speaks the subset of the module
v31 command set the gateway still needs: display (`-`, `+`), homing (`h`), and the queries
(`v`, `A`) — plus the by-serial form `mXA`. Broadcasts, the two-star v6 form, and the ranged
`m*v0-49` / `m*A0-49` batch queries all work. The calibration/dump family is *accepted* —
those frames pass through the sanitizer untrimmed — but the modules silently ignore them:
there is nothing to calibrate and nothing to dump.

**The mechanism is not emulated at all.** There is no stepper, no Hall sensor and no EEPROM.
Nothing can be out of tune, so nothing needs tuning: the calibration, diagnostics, provisioning
and backup commands are ignored rather than faked — no reply, no state. `h` simply shows flap
0, and the `A` reply reports the nominal `homeOffset=2832` / `totalSteps=4096` with an empty
flap map.

**The bus is not emulated either.** A real RS-485 bus at 9600 baud is half-duplex and slow; a
broadcast `m*v` across 45 modules takes four seconds of staggered reply slots. Here replies
come back promptly, in module-ID order, spaced just enough that a broadcast train stays
legible in the MQTT wire mirror.

Collisions therefore do not happen. Two modules given the same ID will both obey a command, but
they answer one after the other rather than on top of each other, so the gateway's duplicate-ID
heuristic — which keys off garbled serial numbers — never fires.

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

The ceiling on the emulated wall is **`VM_MAX_MODULES` = 192** (`src/vmodule.h`), and the bus's
reply queue is sized to match, so a 32 × 5 = 160-module wall on a 256px chain still has room to spare. A grid that
exceeds the ceiling is quietly reduced, and the boot log says so.

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
re-mapping the pins: the pin map is correct and matches Adafruit's reference.

A 64-row panel needs the E address line, which is GPIO 21 on this board. A solder jumper on the
MatrixPortal selects which HUB75 connector pin that reaches (pin 8 by default, or pin 16); match
it to your panel.

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
(the flap wall, the monitor rows, the quiet-day checkboxes). Messages the JS *composes*
(`"Error: " + e`) are wrapped in
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

Stable across reboots, unique per board, and never mistakable for a real ATtiny SIGROW read.

---

## Compatibility

The companion app talks to the gateway over exactly seven HTTP endpoints and models *nothing*
about a module — not the flap count, not the character set, not serial numbers. The
reel and the `FA5E…` serials are invisible to it. Those seven are
`GET /api/config`, `GET /api/status`, `POST /api/rs485/send`, `POST /api/rs485/batch`,
`POST /api/companion`, and `GET`/`PUT /api/companion/settings`. (The `/api/rs485/*` pair are
compatibility aliases — the canonical paths are `/api/bus/send` and `/api/bus/batch`, but the
companion is frozen on the physical gateway's names and both reach the same handlers.)

Two things do matter, and both are handled:

1. **`GET /api/config` must report `version >= 3.1`.** The companion parses `MAJOR.MINOR` out of
   it and gates its gateway-stored settings on `>= (3,1)`. This firmware implements that surface
   exactly, so it answers `3.1.0` and puts its own version in `fwVersion`.
2. **`POST /api/rs485/send` and `/api/rs485/batch` must forward frame bytes verbatim.** The
   companion sends `m00-A\n` style frames as `windows-1252`, one byte per glyph. They are not
   transcoded — on either path, alias or canonical.

The gateway's own MQTT surface, Home Assistant discovery and the `/api/companion/settings` gzip
blob store are carried over untouched.

---

## Repository contents

```
src/common.h        board config, panel defaults, buffer sizes, shared types
src/gateway.h       umbrella header: common.h plus every subsystem's public API
src/globals.cpp     single definition site for every shared global
src/config.*        runtime configuration (GwConfig) persisted in NVS
src/rtc.*           wall-clock time: the ESP32's internal RTC + NTP
src/charset.*       UTF-8 <-> Windows-1252 flap-byte transcoding
src/reel.h          the 237-flap reel and its two resolvers — Arduino-free, so
                    tools/reel_test.cpp compiles the same code
src/font1252.*      GENERATED bitmap glyphs: the 216 printable CP1252 flaps + 14 pictographs
src/aafont.h        GENERATED by tools/genaafont.py — Orbitron faces for the clock effect
src/bus.*           frame sanitization, the command log, the busSend() choke point,
                    scheduled batch pacing
src/vbus.*          the emulated bus: frame delivery + the reply queue
src/vmodule.*       the virtual split-flap modules: protocol dispatch, the shared reel,
                    the synthesized replies
src/display.*       flap-wall geometry and the flap renderer (calls panel.*)
src/canvas.*        raw canvas: frames, rects, QOI decode, draw ops, on-device animation + ticker
src/effects.*       on-device effects: plasma, fire, matrix, flip-o-rama, clock, Life
src/panel.*         the low-level HUB75 driver: ESP32-S3 LCD_CAM + GDMA, no library
src/modules.*       high-level protocol send helpers (text/char/home) + FATFS mount
src/mqtt.*          MQTT client, publish queue, HA discovery
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
tools/genaafont.py  regenerates src/aafont.h from Orbitron (the clock effect's faces)
tools/Orbitron.ttf  vendored Orbitron variable font (SIL Open Font License)
tools/bdf/          public-domain X11 "misc-fixed" fonts (10x20, 9x18, 8x13, 6x13, 6x10, 6x9, 5x8)
tools/reel_test.cpp native regression test for the reel and the 'A' reply format

platformio.ini      build/upload configuration
ARCHITECTURE.md     why the non-obvious decisions were made
openapi.yaml        REST API reference
```

`modules.*` is the gateway's side of the module protocol — how a character, an index or a home
becomes a frame. `vmodule.*` is what answers those frames. They talk only through protocol
frames, which is exactly why the port works. (Upstream, `modules.*` also holds a *registry* of
whatever is out on the bus. That has no meaning on a drawn wall and was removed in 1.10.)

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
c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp src/font1252.cpp \
    -o /tmp/reel_test && /tmp/reel_test
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
  together, and `panelBegin()` clamps the bit depth down until the framebuffer stops starving
  WiFi of that pool — refusing only a geometry that will not fit even at depth 1. The
  virtual-module array is pinned to internal RAM too — `taskDisplay` walks it 100×/s on the core
  the refresh runs on, and quad PSRAM there causes a shimmer. (The monitor ring, the MQTT queue
  and the scheduled-TX ring *are* in PSRAM; nothing in the refresh path touches them.)
- **No battery-backed RTC.** Wall-clock time is invalid from power-on until the first NTP sync.
  Every caller already handles that state; frame timestamps show `HH:MM:SS` uptime until then.
- **No collision emulation**, so the duplicate-ID heuristic never fires.

---

## Licence

CC BY-NC-SA 4.0, as the upstream Split-Flap Gateway. Split-flap module hardware and the initial
protocol by [Adam G Makes](https://www.youtube.com/@AdamGMakes). The bundled bitmap fonts are
the X11 `misc-fixed` faces: *"Public domain font. Share and enjoy."*
