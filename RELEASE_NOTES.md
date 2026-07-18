# Matrix Portal Gateway — Release Notes

## v1.22.0 — 2026-07-18

The last of the bus. This product has no RS-485 transceiver and no bus of any kind, and
after this release it no longer talks as if it did.

### Breaking

- **`POST /api/frames/send` and `POST /api/frames/batch` are the send endpoints.** The
  `/api/rs485/*` compatibility aliases (kept through v1.21) and the short-lived
  `/api/bus/*` paths are **gone** and return 404. **The companion app must be updated**
  to POST to `/api/frames/*` (two URLs in its REST transport).
- **`/api/status` renames `stk.bus` to `stk.frames`**; the MQTT status JSON renames
  `stkbus` to `stkframes`, and the HA diagnostic sensor follows ("Stack Frames"). The
  retained discovery configs for both earlier ids (`stk485`, `stkbus`) are deleted on
  connect, so no dead sensors linger in HA.

### Changed

- **Internal naming**: `src/bus.*` → `src/frames.*` (`frameSend`, `taskFrames`,
  `FrameMsg`); `src/vbus.*` → `src/vlink.*` — the delivery/reply-queue seam between the
  gateway and its virtual modules (`vlinkDeliver`, `vlinkPoll`, `vlinkQueue`). The
  bus-quiet guard is now the reply-quiet guard (`TX_REPLY_GUARD_MS`). The FreeRTOS task
  and watchdog line say `Frames`.
- **Wording**: the dashboard's Status card heading is "Protocol", the timezone help
  says "command log", and every remaining comment or doc that described this product in
  terms of a bus now speaks of the frame link, the virtual modules, or — when referring
  to the other product — "the physical Split-Flap Gateway" and its serial wire. The
  stale NVS-migration comment naming the ancient `rs485gw` namespace is gone too.

## v1.21.1 — 2026-07-18

### Fixed

- **The gateway no longer advertises a Monitor tab to the companion.** `gwTabs` (the tab
  list the companion uses to deep-link this dashboard) still carried `monitor` after
  v1.21.0 removed the tab, so the companion's nav could link into thin air. It now
  advertises Display, Settings, Status.
- **Wording sweep for the bus that isn't there**: the OpenAPI "Bus" tag no longer says
  "Raw RS-485 bus access" (it is raw protocol-frame access to the emulated bus), and the
  last "bus monitor" references in the README/ARCHITECTURE/UI comments now say command
  log — or are gone, where they described the removed tab.

## v1.21.0 — 2026-07-18

### Removed

- **Maintenance mode is gone.** It was the physical gateway's service mode — ignore
  external MQTT commands so an operator could calibrate or repair modules without
  automations fighting them. This product has nothing to service: the modules are drawn,
  calibration does not exist, and Quiet Time already covers "hold the display still".
  Removed end-to-end: the `/api/maintenance` endpoint, the `<prefix>/maintenance/set`
  MQTT topic and its command gate, the Home Assistant "Maintenance Mode" switch (its
  retained discovery config is deleted on connect, like the v1.20 `stk485` cleanup),
  the `maint`/`maintenance` fields in `/api/status` and the MQTT status JSON, the
  `maintenance` capabilities token, and the dashboard's yellow maintenance banner with
  its CSS and translations. Neither the companion nor the web UI ever depended on it.
- **The Monitor tab is gone from the dashboard.** The command-log viewer and the raw
  Send Frame card were bus-debugging surfaces inherited from the physical gateway; with
  no wire to debug they earned their keep poorly, and the 600 ms `/api/log` poll they ran
  was the dashboard's chattiest request. The REST surface is untouched — `GET /api/log`
  and `POST /api/bus/send` (with `{"raw":true}`) still exist for scripted debugging —
  and the embedded page shrank by ~24 KB.

## v1.20.0 — 2026-07-18

A full-codebase audit release: the RS-485 terminology is gone, the last physical-gateway
leftovers are removed, and every stale comment the audit flagged is fixed. No display
behaviour changes.

### Changed

- **"rs485" is now "bus", everywhere.** There is no RS-485 transceiver in this product, and
  the name had outlived its accuracy: `src/rs485.*` is now `src/bus.*`, `rs485Send` is
  `busSend`, the bus task and its watchdog/stack telemetry renamed to match (`stk.rs485` →
  `stk.bus` in `/api/status`, the HA sensor `stk485` → `stkbus` — the old retained discovery
  config is deleted on connect). The REST endpoints are canonically **`/api/bus/send`** and
  **`/api/bus/batch`**; `/api/rs485/send` and `/api/rs485/batch` remain as aliases because
  the companion app still POSTs to them.
- **Text sends no longer sleep.** `sfSendText` paced frames with a 10 ms `delay()` per
  character — wire pacing for a bus that no longer exists, which froze the HTTP or MQTT task
  for up to 2.5 s on a long text. The flip animation is the cascade; the delay is gone.
- **`/api/flap/index` addresses the whole reel (0–236)**, including the lowercase and
  pictograph sections — the same `m<id>+<n>` command `/api/display/cells` sends. It was
  capped at the physical module's 0–63.
- **The frame sanitizer models only the commands this product speaks** (`-`, `+`, `h`, `v`,
  `A`, and by-serial `mXA`). The physical gateway's calibration/dump grammar (`c`, `d`, `g`,
  `s`, `o`, `t`, `w`, …) is no longer modelled — neither the companion nor the UI ever sends
  it; such frames pass through untrimmed and the virtual modules ignore them.
- **Virtual-module persistence is gone.** Every field of every module is deterministic from
  the configured grid (id = wall slot, MAC-derived serial, reel homed at boot), so
  `/vmods.dat` stored nothing that could vary; the file, the boot counter and the `autoHome`
  flag (always on) are removed, and a leftover file from older firmware is deleted at boot.
  FATFS now holds only the companion settings blob.
- **Home Assistant device metadata**: manufacturer is now "Alex Van de Putte" (was a
  placeholder).

### Fixed

- **Effect parameters no longer change on a rejected request**: `POST /api/canvas/effect`
  now checks Quiet Time *before* writing speed/hue/density, so a 409'd request cannot leave
  its parameters behind for the next start.
- **MQTT publishes no longer starve the queue.** The drain published up to 31 messages while
  holding the queue mutex; a slow broker made producers (10 ms timeout) silently drop. Each
  item is copied out under the lock and published outside it.
- **JSON error bodies are always valid JSON**: `sendJsonError` escapes its message, which
  matters for the one caller that echoes client-supplied text (an unknown colour name).
- **The dashboard's flap-index card matches the API** (0–236; it advertised 0–162 while the
  API rejected ≥64), the MQTT help text lists the topics that actually exist, and the Quiet
  Time banner no longer promises "calibration still works" on a product with nothing to
  calibrate. ~5 KB of dead CSS (the removed calibration wizard/editor and module-card pages)
  and the old 64-flap reel string are deleted from the embedded page.

### Internal

- Effects: Life/plasma read the volatile hue once per frame instead of per pixel; Life's
  toroidal wrap is precomputed per row/column; the 2·W·H scratch grid is only allocated for
  the two effects that use it. Broadcast `-`/`+` frames resolve their payload once, not per
  module, under the locks. The OTA page streams from flash instead of a per-request heap
  `String`.
- One shared glyph blitter (`dispDrawGlyph1252`), one shared pixel decoder, one
  `readJsonBody()` helper replacing thirteen copies of the body-parse preamble, one build
  ETag. Dead declarations (`mqttPublishSFEvent`, unread `BusMsg` fields, the vbus reply
  `arg`, `blankUnused`) are gone.
- The audit's stale-comment sweep: no more "refresh ISR" (the driver is pure GDMA), no more
  10 MHz-era timing figures, no more 64/163-flap reel documentation, no phantom commands.
  openapi.yaml, README and ARCHITECTURE re-synced to the code; release notes backfilled for
  v1.11–v1.19.1.

## v1.19.1 — 2026-07-18

### Fixed

- **The 507 low-heap guard now covers the readback GET.** Streaming a ~48 KB screenshot out
  holds internal TX buffers; on a RAM-tight 256×64 board, done concurrently with the companion's
  frame pushes, that was observed to drive minimum heap to ~20 KB — right at `loop()`'s reboot
  floor. `GET /api/canvas/frame` now refuses with **507** below the same threshold the anim/QOI
  uploads use; a poller just retries. The companion-critical `/frame` PUT stays unguarded.

## v1.19.0 — 2026-07-18

### Added

- **Read the panel back — `GET /api/canvas/frame`.** A screenshot of whatever is on screen
  (wall, effect, canvas, animation, ticker), reconstructed from the bitplane framebuffer as raw
  pixels — `rgb888`, or `rgb565` with `?fmt=rgb565`. Colours come back **quantised to the
  panel's real bit depth** and with the BGR swap undone, so it is what is physically lit
  (brightness, an OE duty cycle, is not in the framebuffer, so it is not reflected). Read-only:
  it never parks or swaps, so a UI can poll it for a live preview without disturbing the running
  mode. `X-Canvas-Width/Height/Format` headers describe the body. Verified by round-trip: a
  solid frame reads back bit-exact at all 16384 pixels. Advertised as `canvas.readback` in
  `/api/capabilities`.

## v1.18.1 — 2026-07-17

### Fixed

- **Large canvas uploads are refused with 507 when heap is low.** A big panel (256×64) runs
  canvas uploads close to `loop()`'s 20 KB reboot floor. The animation and QOI endpoints now
  check free heap up front and return **507** (retry) below 40 KB — twice the floor — rather
  than pile a takeover plus a PSRAM allocation onto an already-stressed heap. The
  companion-critical `/frame` path and the usually-small `/rect` path are left untouched. A bad
  QOI now hands the panel back instead of leaving it parked.

## v1.18.0 — 2026-07-17

The canvas does more, over far less WiFi. All Matrix-only — the RS-485 wall has no framebuffer.

### Added

- **`PUT /api/canvas/rect`** — update *one rectangle* instead of resending the whole panel: an
  8-byte `[x, y, w, h]` header then `w × h` pixels (`rgb888` or `rgb565`, by length), drawn over
  the live frame — so animating a small area costs only that area's bytes.
- **`PUT /api/canvas/qoi`** — a full-panel [QOI](https://qoiformat.org) image. Lossless, decodes
  in one pass, and typically 2–4× smaller than raw.
- **`PUT /api/canvas/anim`** — upload a short loop *once* and it plays on-device from PSRAM, so
  the client can disconnect. A 14-byte `MPGA` header then the frames back-to-back;
  `taskDisplay` plays it at the set fps.
- **`POST /api/canvas/ticker`** — one line of text scrolled across the panel, rendered
  on-device, no streaming. Empty text hands the panel back.
- **Effect parameters** — `POST /api/canvas/effect` takes optional `hue` (0–255) and `density`
  (1–100): recolour the matrix rain, tint plasma and Life, set the Life seed % or flip-o-rama
  churn. Omit them and every effect looks exactly as before.

Animation and the ticker are autonomous display modes like effects: a split-flap command or
Quiet Time stops them. `GET /api/canvas` and `/api/capabilities` advertise `rect`, `anim`,
`ticker`, the `qoi` format and `effectParams`, so a client reads the feature set instead of
sniffing the version.

## v1.17.1 — 2026-07-17

### Fixed

- **A too-deep panel dims itself instead of going blank.** A 256×64 at bit depth 4 needs 144 KB
  of internal DMA RAM, over the 120 KB budget the driver will spend before WiFi, so
  `panelBegin()` refused and ran headless — a silent blank screen with no hint why. It now steps
  the depth **down** to the deepest that fits both the budget and the live free-RAM reserve
  (256×64 lands on depth 3, ~85 Hz), logs the clamp, and lights up. It still refuses only if
  even a single bitplane will not fit. `/api/status` reports the actual running `panel.depth`.

## v1.17.0 — 2026-07-17

*(There is no v1.16 — the number was skipped.)*

### Added

- **A split-flap command auto-stops the canvas.** Any text/char/index/home command now stops a
  running effect, animation, ticker or raw canvas and returns the panel to the wall, via one
  shared hook in the dispatcher — so every path (API, MQTT, companion) behaves the same.

### Fixed

- **A cross-core race in effect starts.** Starting an effect reset its state from the HTTP core
  while a render could be in flight on the display core — a divide-by-zero / use-after-free
  window previously papered over with a `delay(40)`. Effect starts now route through a request
  flag consumed only by `taskDisplay`, so a reset never runs under an in-flight frame.
- **Quiet Time owns the panel again**: it hands the panel back from any canvas mode and refuses
  new effects/canvas with a **409** while active.
- The three timezone-apply sites are unified under one mutex-guarded `cfgApplyTZ()`; Life
  null-guards its grid; the cross-core brightness fields are `volatile`.

### Changed

- **The clock got cheaper and steadier**: it reads the RTC once per frame and rebuilds strings
  only on a new second, with proportional digits centred in fixed slots. Its face is now
  **Orbitron**, emitted as 1-bit packed masks (16 KB → 2.5 KB of flash, visually identical).
- Perf and dead-code pass: Life steps by pointer swap instead of a per-step copy, fire walks
  row-major, raw frames drop a per-pixel divide, and the effect enum/name/list collapse into one
  table.

## v1.15.0 — 2026-07-16

### Added

- **Three more on-device effects — flip-o-rama, clock, and Game of Life.** They join plasma,
  fire and matrix, all started with `POST /api/canvas/effect` and rendered on the display task
  at the panel's native rate:
  - **`fliporama`** — the whole board flips through random glyphs, like a live departure board.
  - **`clock`** — a digital clock with big anti-aliased **HH:MM** in a bundled face, drifting
    through a rainbow, with the date and seconds. It adapts to the panel: HH:MM + date +
    seconds on a tall wall, one row on a 128×32. The face is generated by `tools/genaafont.py`
    (SIL Open Font License, vendored in `tools/`).
  - **`life`** — Conway's Game of Life on a wrapped grid, cells coloured by age, reseeded when
    it settles.
- **Canvas and effects are advertised in `GET /api/capabilities`** — a `canvas` object (formats
  + panel size), an `effects` array, and `canvas`/`effects` tokens in `features`, from one
  shared list. The Split-Flap Gateway, which has no framebuffer, answers the same URL without
  those keys, so the companion lights up the right controls without sniffing the firmware
  version.

### Fixed

- **The clock shows the right time — a timezone bug.** Every NTP sync called
  `configTime(0, 0, …)`, which resets `TZ` to UTC and silently clobbered the zone set on the
  Settings page — bus-log timestamps, Home Assistant and the new clock all reverted to UTC
  after the first sync. The configured zone is now re-applied after every sync.

## v1.14.0 — 2026-07-16

### Added

- **On-device effects — smooth animation the panel renders itself.** Pushing frames over HTTP
  tops out around **8 fps**: the web server closes the socket after every request, so each frame
  pays a fresh TCP connection and slow-start, and that fixed cost — not bandwidth — is the wall
  (rgb565 frames are no faster than rgb888). So animation moved *onto* the gateway.
  **`POST /api/canvas/effect {"type":"plasma|fire|matrix","speed":1-10}`** runs the animation on
  the display task at the panel's native **~70 fps**, with nothing on the network:
  - **`plasma`** — a flowing rainbow sine-interference field.
  - **`fire`** — bottom-up flames, a Doom-style drift-and-decay spread for real tongues.
  - **`matrix`** — falling green streaks, bright heads and fading trails, per-column speeds.

  An effect is a third display mode beside the wall and the raw canvas: it owns the panel until
  `{"type":"none"}` hands it back. All integer/LUT work — a sine table, two palettes, a PSRAM
  heat buffer — so a full frame fits inside one refresh.

### Fixed

- **rgb565 raw frames actually work now.** `PUT /api/canvas/frame` read its pixel format from
  the `?fmt=` query arg, but a raw-body handler cannot see URL args, so `rgb565` silently fell
  back to rgb888 and every correctly-sized frame was rejected as the wrong length. The format is
  now inferred from the **body length** (`W×H×3` = rgb888, `W×H×2` = rgb565), which is
  unambiguous.

## v1.13.0 — 2026-07-16

### Added

- **Raw canvas — the panel with the flaps taken off.** Every cell of this wall is *drawn*, so
  under the split-flap costume it was always just a HUB75 framebuffer. Three new endpoints hand
  a client that framebuffer directly, bypassing the wall entirely — something the physical
  gateway cannot offer, because it has no framebuffer to expose, only motors:
  - **`PUT /api/canvas/frame?fmt=rgb888|rgb565`** — a full frame as raw pixel bytes, row-major,
    **streamed straight to the back buffer**, so a 256×64 frame is never buffered whole.
  - **`POST /api/canvas/ops`** — a JSON array of draw commands (`clear`, `pixel`, `hline`,
    `vline`, `rect`, `text` in the bundled CP1252 faces, `show`) for shapes and labels without
    composing a frame client-side.
  - **`GET /api/canvas`** reports the panel size and whether a canvas is active; **`POST`**
    `{"active": true|false}` takes over and blanks, or releases.

### Changed

- **The reel renderer stands down while a canvas is up**, exactly as it does during an OTA — so
  the HTTP handlers own every pixel and nothing repaints the wall from under them. Take-over
  blocks until the display task acknowledges it has parked, so the reel renderer's closing frame
  cannot land the wall back over the canvas. Releasing repaints the wall from the modules'
  state; nothing is persisted — a reboot returns to the wall.
- The header wordmark is drawn with inline split-flap CSS tiles instead of the `/logo.svg`
  image (branding parity with the Split-Flap Gateway).

## v1.12.0 — 2026-07-15

### Added

- **Capabilities state how the wall moves** — a `motion` key, mirroring the RS-485 gateway's:
  `kind: drawn`, because a cell here is a repaint (a new value retargets it mid-flip, nothing
  queues, so sub-second updates are honest), and `settleMs` reporting the worst-case flip
  animation from the live config (cosmetic pacing, advisory). Additive.

### Fixed

- **A dead port-80 listener self-heals**: a ground-truth `listening()` check every 20 s re-arms
  the web server only when it is genuinely down. Boot now logs the running OTA slot and build
  time.

## v1.11.0 — 2026-07-15

### Changed

- **Branding parity with the companion app.** The header wordmark is just SPLIT-FLAP with
  "GATEWAY" as text beside it, and the firmware version moved out of the title bar into a quiet
  footer on every tab.
- **Settings reorganised** so related controls share a box: **Network** (Device Name + WiFi),
  **Localization** (Language + Timezone), and one **Module Wall** card that folds in the
  Geometry preset — the CSS-column layout kept splitting the pair across columns.

### Added

- **A panic-recovery safeguard.** Consecutive crash/watchdog reboots are counted in RTC memory
  (survives a reboot, zeroed on cold power-up and after 60 s of healthy running); after 3 in a
  row the boot logic reformats FATFS to break a corruption-driven boot loop — one self-healing
  reboot instead of a brick. This is the exact failure mode that once took both panels down.

### Fixed

- `/logo.svg` and `/favicon.svg` now revalidate with a build-time ETag instead of a 7-day
  max-age with no validator, so a changed logo is not served stale from the browser cache.

## v1.10.1 — 2026-07-14

Everything since the initial **v1.0.0** port, summarised. The firmware grew from a faithful
port of the Split-Flap Gateway into a fuller product: it speaks 13 languages, blanks the wall
at night, drives much larger panels, shows lowercase and pictographs its physical cousin
cannot, tells a client exactly what it can show — and, under the hood, dropped the module
registry entirely in favour of the one thing that was always the truth: the drawn wall.

Because these modules are *drawn*, not physical, this gateway is not bound by a real reel's 64
leaves. The whole release leans into that.

### Added

- **A dashboard in 13 languages** plus English, chosen automatically from the browser, with a
  Settings override and a `?lang=` parameter — all in one firmware image *(v1.1)*.
- **Bigger walls.** A **15×5** preset fills a 128×64 panel with **75 modules** *(v1.3)*, and
  **256×64** panels are supported with a 15×5 layout and three larger, more detailed fonts
  *(v1.8)*.
- **Lowercase, accents and pictographs — a 237-flap reel.** The reel carries every
  Windows-1252 glyph, the seven colours, all 60 lowercase letters, and 14 pictographs
  (♥ ♦ ♣ ♠ ☺ ♪ ● ■ ⌂ ← ↑ → ↓ ☀). A new index-addressed endpoint **`POST /api/display/cells`**
  reaches the flaps the one-byte legacy protocol cannot name; it accepts lowercase, named
  colours and pictographs by character *(v1.5, v1.6)*.
- **`GET /api/capabilities`** — one call that reports the wall's full character set (here it is
  always uniform: one drawn reel). The **Split-Flap Gateway answers the same URL identically**,
  so a client never has to know which kind of wall it is driving *(v1.10)*.

### Changed

- **Quiet Time now blanks the wall** as it begins — every reel flips down to blank — and
  restores what was showing when it ends *(v1.2)*.
- **Every board has its own identity.** The hostname, MQTT client id and Home Assistant device
  id are derived from the MAC bytes that actually differ between boards, so two gateways on one
  broker no longer evict each other in a loop *(v1.2.1)*.
- **The module registry is gone.** The wall is drawn: the array of virtual modules *is* the
  state of every cell, so a second, sticky copy of it was removed. That copy stored a **byte**
  per cell, which corrupted a Quiet-Time cycle — lowercase folded to uppercase and pictographs
  were lost. The pending display is now a flap **index** and round-trips exactly, and
  `/api/display/state` and `/api/flap/modules` report **code points**, so a heart reads back as
  a heart *(v1.10)*.
- **The Modules page is gone** — every cell is a module and always present, so the page only
  ever said the same thing 75 times. The wall **homes itself at boot** (no more stale content
  from before a reboot), and the companion URL is read-only in the UI, shown on the Status page
  *(v1.7, v1.9)*.

### Fixed

- **A BGR colour-order option.** Some HUB75 panels are wired with blue and red swapped; a
  per-board setting corrects it on the next frame, no reboot *(v1.6)*.
- **The "45 of 75" discovery bug** — growing the wall left the new modules undiscovered and the
  panel stuck at the old size *(v1.4)*. *(Moot as of v1.10: there is no registry to grow.)*
- **Companion tab advertisement** was accepted and silently discarded; both directions now work
  *(v1.4)*.
- **MQTT display-state** no longer publishes a stale — or, on the first call, blank — snapshot
  to Home Assistant when the reel lock is momentarily busy, and several dead-code leftovers of
  the registry removal were cleaned up *(v1.10.1)*.

---
*Board: Adafruit MatrixPortal ESP32-S3 (8 MB flash). Update via the dashboard's OTA upload or
`espota`; a fresh board is flashed over USB with PlatformIO.*
