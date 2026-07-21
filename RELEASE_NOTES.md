# Matrix Portal Gateway — Release Notes

## v3.1.0 — 2026-07-19

### Changed (breaking)

- **The sprite atlas is now a named, persistent library** — the single global sheet (and
  its "any upload replaces it, and blits no-op mid-upload" coordination problem) is gone.
  Up to **16 resident named sheets** share a **4 MB** PSRAM budget (2 MB per-sheet cap),
  LRU-evicted; uploads build in a fresh allocation and publish atomically at commit, so a
  bound sheet is never observed half-written. The old unnamed `PUT /api/canvas/atlas` is
  **removed**.

### Added

- `PUT /api/canvas/atlas/<name>` — upload a named MPTA sheet (12-byte header unchanged;
  name grammar `[a-z0-9._-]{1,32}`).
- `GET /api/canvas/atlas` — the library: `[{name,tiles,w,h,fmt,bytes,resident,persisted}]`,
  covering resident sheets and persisted `/atlas/<name>.mpta` files.
- `POST /api/canvas/atlas/<name>/save`, `DELETE /api/canvas/atlas/<name>` — persist to
  FATFS / remove everywhere. A persisted sheet that gets LRU-evicted **lazy-loads on its
  next bind**; nothing preloads at boot.
- Ops: `{"op":"atlas","name":"…"}` binds a sheet for subsequent `sprite` ops (sticky
  across batches — bind explicitly per batch when using content-fingerprint names). A
  `sprite` with nothing bound, or a bind to an unknown name, no-ops rather than failing
  the batch.
- `GET /api/canvas` reports `atlas: {bound, loaded:[…]}`; `GET /api/capabilities`
  advertises `canvas.atlas = {named, persist, maxSheets, maxBytes, maxSheetBytes}` for
  feature detection.
- Files-tab uploads route `.mpta` files to `/atlas/` automatically.
- **Atlas Library card on the Files tab** — every sheet with shape/size/state badges,
  Save/Delete, and a click-to-preview rendering the actual tiles in the browser via the
  new `GET /api/canvas/atlas/<name>` (the sheet back as its MPTA image).

### Performance (canvas)

- **Row blitters** replace per-pixel writes on every frame-shaped path (full-frame PUT,
  animation playback, QOI decode, transition tweens): the per-row plane masks are hoisted
  and pixels land in one tight pass — ~4–6× faster panel drawing, which also raises the
  playable animation frame rate.
- **Lazy tear-guard**: `panelShow` no longer sleeps ~a frame after every swap; the wait
  moved to the first buffer write afterwards, where network/compose time usually absorbs
  it entirely.
- **`PUT /api/canvas/rects` — multi-rect delta updates** (the `canvas.rects` capability):
  one binary body of N changed regions (`u16 count, u8 fmt, u8 0`, then per rect
  `u16 x,y,w,h` + pixels), drawn over the current frame and presented once. A client that
  diffs its frames sends 10–50× less than a full-frame push; measured pipeline floor
  ~12 ms for a typical small delta.
- **Dashboard preview readback switched to rgb565** — a third less data per refresh.
- **Inbound pacing retuned to danger-only tiers** (<45 K/<25 K): the old cruise tier sat
  above baseline-minus-one-stream, so every ordinary frame push throttled itself.

## v3.0.1 — 2026-07-19

### Fixed

- **The dashboard live preview now follows the panel into canvas mode.** The display
  state (REST + SSE) gained an additive `"mode":"wall"|"pixels"` field; when canvas, an
  effect, an animation or a ticker owns the panel, the preview switches from flap cells
  to a true pixel rendering of `GET /api/canvas/frame` (1 Hz readback into a scaled
  `<canvas>`), and back automatically. A mode flip pushes an SSE event even when no
  reel moved.
- **Lowercase r/o/y/g/b/p/w no longer render as colour swatches in the preview.** The
  state JSON's `cells` letter is identical for a colour flap and its lowercase letter —
  indistinguishable by design of the legacy protocol. The state now also carries the
  additive `"flaps":[index…]` array (colour flaps are exactly indices 156–162), and the
  preview colours only those. Clients that ignore the new fields see no change.

## v3.0.0 — 2026-07-19

### Changed

- **The HTTP server is now ESP-IDF's `esp_http_server`, spoken natively.** The Arduino
  `WebServer` (one connection at a time; a slow stream starved every other client) is
  gone. Handlers are plain `esp_err_t fn(httpd_req_t*)` functions over a thin helper
  layer (`src/httpx.*`: dispatch hook with CORS + watchdog instrumentation, JSON/chunk/
  query helpers). The old cross-callback upload state machines (OTA, fs, companion blob,
  seven canvas uploads) collapsed into linear recv loops with local state. Multiple
  concurrent sockets, per-socket recv/send timeouts, LRU purge, and async requests.
- **BREAKING: `POST /api/ota/upload` and `POST /api/fs/upload` take raw bodies, not
  multipart.** OTA: `curl --data-binary @firmware.bin http://<gw>/api/ota/upload`.
  Files: `curl --data-binary @x.mpg 'http://<gw>/api/fs/upload?name=x.mpg'`. The `/ota`
  page and the Files tab already send this form. Everything else on the API surface is
  unchanged — every endpoint, schema and status code.

### Added

- **`GET /api/events` — SSE push stream for the live preview** (the `events` capability
  token). `text/event-stream`: a `display` event carries the same JSON as
  `GET /api/display/state`, pushed within ~150 ms of the wall changing (max ~7 events/s
  so a flip cascade streams as motion), snapshot on connect, `: ka` keepalive every 15 s,
  up to 3 concurrent streams (a 4th gets `503`). taskWeb — freed from serving HTTP — is
  the push pump, hashing the reels and broadcasting on change.
- **The dashboard's Live Display rides the stream**: near-real-time preview via
  `EventSource`, with the old 1.5 s poll kept as an automatic fallback.

### Removed

- **MQTT and Home Assistant support, entirely.** The broker connection, the topic tree
  (`<prefix>/frames|flap|display|quiet/*`, status, availability), HA auto-discovery, the
  per-frame wire mirror, the MQTT/HA settings cards, `POST /api/config/mqtt` and
  `POST /api/mqtt/test` are all gone; `/api/status` and `/api/config` lost their
  `mqtt`/`mq*`/`haEnabled` fields, `/api/capabilities` its `ha` token, and PubSubClient
  left the build. Nothing in this deployment used them — the companion is pure REST and
  the dashboard now has SSE — and the permanently-open broker socket + client buffers
  were weight exactly where this board is tightest. taskNetwork is now pure WiFi
  supervision. **Note for HA users upgrading:** previously-published retained discovery
  configs will leave ghost entities; remove the device from HA (or purge
  `homeassistant/+/sfgw_*` retained topics on the broker) after flashing.

- **ArduinoOTA (the Arduino-IDE espota push path), its task and its password.** Never
  used — every flash is the web/curl upload or esptool over USB — and it cost a 4 KB
  task, a UDP listener and a Settings card. mDNS (`http://<hostname>.local`) stays; the
  `otaPassword`/`otaPasswordSet` config fields and the Settings-page card are gone.
  Recovery path is unchanged: hold-BOOT + esptool.

### Optimized

- **~5 KB static RAM back**: the three per-handler 1.4 KB raw-body buffers merged into
  one shared `httpxBuf`; task stacks right-sized from watermark telemetry (Frames
  6144→5120, Network 8192→6144, plus taskOTA's 4096 gone entirely).
- **Concurrent-socket ceiling tightened to 4** (with LRU purge) after bisection showed
  overlapping large streams stacking TCP buffers ~20 KB per round — this bounds the
  worst-case heap dip that multi-socket serving makes possible.
- **Flash 32 KB smaller than v2.2.4** (1,370 KB vs 1,402 KB) despite the new features —
  dropping WebServer + ArduinoOTA outweighed everything added.
- **`stkhttpd` replaces `stkota`** in `/api/status` and the MQTT status payload: the
  esp_http_server worker's stack watermark is the one that now matters.

### Fixed

- **Heap backpressure now covers every large stream, both directions** (the OTA war's
  lesson, generalized): inbound raw bodies and outbound chunked replies pace themselves
  when internal heap runs low, closing the TCP window before buffer buildup can approach
  loop()'s reboot floor. Observed adversarial-soak watermark improved accordingly.

## v2.2.4 — 2026-07-19

### Fixed

- **Stale flaps, root-caused twice over.** (1) The scheduled-frame ring (`TXQ_SIZE`) was
  still sized for 45-module pages; one page of a 160-module wall overflowed it, and
  overflow frames fell back to inline sends that jump the queue — so an older page's
  queued frames could land *after* a newer page's and stick (reproduced deterministically:
  121 of 160 cells stale; zero after the fix; ring now 512 slots / ~28 KB PSRAM).
  (2) Setting a cell to the value it already shows *while mid-flip* cleared the flip state
  without a repaint, freezing the half-flap composite on the panel — the "letter stuck
  between characters" — until an unrelated repaint. Cancelling a mid-flip now repaints.

## v2.2.3 — 2026-07-19

### Added

- **`POST /api/system/reboot`** — clean remote restart, replying before rebooting (the
  same deliver-then-restart path web OTA uses). For applying geometry changes, kicking a
  wedged peripheral, or booting a committed OTA image without touching the hardware.

## v2.2.2 — 2026-07-19

### Fixed

- **Web OTA now works on RAM-tight geometries too.** v2.2.1's backpressure and floor
  exemption fixed OTA where heap was plentiful, but a 256×64 board *enters* the upload
  with only ~40 KB free (its 102 KB framebuffer is the difference) — no throttle can
  conjure headroom that isn't there. The panel now RELEASES its framebuffer and DMA
  descriptors for the duration of the upload (`panelRelease()`): the display goes dark,
  38–102 KB returns to the heap, and the TCP window has room to breathe. A successful
  upload reboots into the new image as always; a failed one re-creates the panel at the
  depth that was actually running (respecting the auto-clamp) and repaints. Verified on
  the 256×64 board at full speed on a strong link: HTTP 200, previously seven straight
  failures.

## v2.2.1 — 2026-07-19

### Fixed

- **Web OTA no longer reboots the board mid-upload on fast links.** On a strong WiFi
  link the sender fills this build's large TCP receive window (~95 KB) faster than
  flash writes drain it; free heap transiently dives, and `loop()`'s 20 KB emergency
  floor — designed to catch leaks — was rebooting the board in the middle of writing
  its own firmware. Every "mystery OTA reboot" observed on the bench was this. Two-part
  fix: the OTA handler now applies graded heap backpressure per chunk (paces the sender
  via TCP flow control), and the emergency floor stands down while an upload is in
  flight (`gOtaInProgress`), where a transient is expected, bounded, and self-clearing —
  and where a reboot is the one genuinely destructive response. Verified: the previously
  always-failing bench-distance OTA now completes with HTTP 200 at full speed.

## v2.2.0 — 2026-07-18

The FATFS partition gets a front door: a Files tab on the dashboard and the `/api/fs`
surface behind it.

### Added

- **Files tab** (between Display and Settings): storage-usage bar; the full file list
  with per-row **Download** and **Delete** (the `/compset.gz` confirm warns it is the
  companion's settings); **Play** and **Set as boot** on `/anim/*.mpg` rows, with the
  current boot animation marked and a one-click **Clear boot animation**; and an upload
  card that refreshes the list on success. Translated in all 11 full UI languages.
- **File API.** `GET /api/fs` streams `{total, free, files:[{path,size}]}` (recursive,
  bytes); `GET /api/fs/file?path=…` streams a download with the basename as its
  attachment name; `POST /api/fs/delete {"path"}`; `POST /api/fs/upload` takes a
  multipart part named `file`, sanitizes the client filename (lowercase, `a-z 0-9 . _ -`,
  1–40 chars), routes by extension (`.mpg` → `/anim/`, `.fnt` → `/fonts/`, else `/`),
  streams to a `.tmp` and renames — `413` when less than 64 KB would remain free, `507`
  on write failure. Paths are validated everywhere (absolute, `a-z 0-9 . _ - /`, no
  `..`, ≤ 48 chars).
- **`GET`/`POST /api/display/brightness`.** Reads or sets the panel brightness
  (`1..255`) live — applied on the next presented frame, whatever is presenting — and
  persists it (the same value as `panelBright`).
- `GET /api/capabilities` now advertises a **`brightness`** feature token pointing at
  that endpoint, and the gateway's advertised tabs (`gwTabs`) include **Files**.

## v2.1.0 — 2026-07-18

The canvas grows into the new memory. Everything below lives in the 16 MB PSRAM and
23.9 MB FATFS the v2.0 board brought; all of it is API-first (no dashboard controls yet).

### Added

- **Animation library.** Animations persist as named files on FATFS: `POST
  /api/canvas/anim/save {"name"}` snapshots the loaded store, `.../anim/play {"name"}`
  loads and plays, plus list (`GET /api/canvas/anims`) and delete. A configured
  **`bootAnim`** autoplays at power-on before WiFi, yielding to the first display
  command.
- **Overlay ticker.** `POST /api/canvas/ticker {"overlay":true}` composites a
  lower-third scrolling band over *whatever* is presenting — wall pages, effects,
  animations, canvas pushes — via a hook inside `panelShow()`. It survives page and
  effect changes; only an explicit stop or Quiet Time removes it.
- **Transitions.** `POST /api/canvas/transition {"type":"crossfade|wipe|slide","ms"}` —
  full-frame canvas PUTs stage in PSRAM and tween from the previous frame on-device
  instead of hard-cutting.
- **Sprite atlas.** `PUT /api/canvas/atlas` uploads a tile sheet (2 MB cap, magenta
  transparency); the ops API gains `{"op":"sprite","i","x","y"}` for low-bandwidth
  sprite blits.
- **GIF import.** `PUT /api/canvas/gif` decodes a GIF on-device (AnimatedGIF) straight
  into the animation store — centered, fps from the GIF's own delays — and plays it;
  save it to the library like any upload.
- **Uploadable fonts.** `tools/fontpack.py` packs a BDF into an MPFT blob;
  `PUT /api/canvas/font` makes it the "custom" face, with a FATFS font library
  (save/list/delete) and a `"font"` field on the ticker and the ops text op.

### Fixed

- Starting an animation no longer half-stops an overlay ticker (`claimPanel` now
  preserves the overlay).

## v2.0.0 — 2026-07-18

**New hardware: the Waveshare ESP32-S3-RGB-Matrix driver board.** A hardware port, not an
API change — every endpoint, topic and behaviour is v1.25.0's. The final MatrixPortal S3
version lives on the `matrixportal` branch.

### Changed

- **Board**: ESP32-S3 with **32 MB octal flash (1.8 V)** and **16 MB octal PSRAM**
  (`opi_opi`), replacing 8 MB / 2 MB quad. New HUB75 pin map (the octal PSRAM consumes
  GPIO 33–37, which the old map used); all 13 signals still route through the GPIO matrix
  into the same LCD_CAM + GDMA driver, unchanged.
- **Battery-backed PCF85063 RTC** (I2C 47/48): seeds the system clock at boot — wall-clock
  time is valid seconds after power-on with no network — and is disciplined by every NTP
  sync. A plausibility window (build time … +5 years) rejects a factory-fresh chip's
  garbage (the first boot read 2056). With no backup cell fitted, behaviour falls back to
  the old wait-for-NTP path.
- **Partitions**: 4 MB + 4 MB OTA slots (was 2+2) and a **23.9 MB FATFS** (was 3.7). No
  UF2 bootloader on this board: recovery is hold-BOOT + esptool; web OTA is unchanged.
- **Animation store: 8 MB** (was 1.5 MB) — ~256 rgb565 frames at 256×64; verified with a
  real 6.3 MB, 200-frame upload.

### Findings

- **The 5 MHz pixel-clock cap was MatrixPortal-specific.** A/B at 10 MHz on this board:
  the radio survived (instant association, 0 % loss). Depth 4 at 256×64 remains blocked
  by the 144.6 KB *internal* framebuffer (26 KB heap free — unshippable), so the clock
  stays at 5 MHz; the framebuffer stays internal even on fast octal PSRAM (the GDMA
  stream would share the PSRAM/cache bus with WiFi, and bounce-buffering puts the CPU
  back in the refresh loop). Single-buffering or bounce buffers are the future path to
  depth 4 at ~80 Hz.
- The companion auto-discovered the new gateway and drove it unchanged; the panel needed
  `panelBGR` (this panel is BGR-wired — confirmed visually and now persisted).

## v1.24.0 — 2026-07-18

Frames now flow one way. The physical protocol's query commands existed so a controller
could discover hardware it couldn't see; a drawn wall has nothing to discover.

### Removed

- **The `v`/`A` query commands and the entire reply pipeline.** No client ever sent them
  (the companion reads `/api/config` and `/api/capabilities`; the wall reads back through
  `/api/display/state`). With them go: the by-serial `mX` addressing, the reply queue and
  its seam (`src/vlink.*` deleted — `frameSend` now hands frames straight to `vmDispatch`
  under `vmMutex`), the reply-quiet guard, taskFrames' reply drain, the **`frames/rx`**
  MQTT topic, the `rx` counter (status JSON, `[WDG]` line, dashboard tile) and the HA
  "Frames Received" sensor (retained config deleted on connect), and the `panel.drop`
  counter. The frame sanitizer now models exactly `-`, `+`, `h`.
- **The fake module serial numbers.** They existed because ATtiny modules have factory
  ids and a physical wire has unprovisioned hardware to address; here a module's identity
  is its wall slot. `VModule` shrinks ~40 → ~16 bytes; `GET /api/flap/modules` rows are
  now `{id, flapIndex, flapChar}` (the `sn`/`provisioned`/`fwVersion` fields had no
  consumer).
- **The grid seam.** The decorative border between module cells (`gridColor`/`gridBright`,
  the Grid color/brightness settings, `drawGrid()`) is gone — it never looked good. The
  module grid *layout* (`gridRows`/`gridCols`) is unchanged.

## v1.23.0 — 2026-07-18

### Breaking

- **The MQTT frame topics moved under `frames/`**, completing the symmetry with the REST
  rename: the wire mirror publishes on **`<prefix>/frames/tx`** and **`<prefix>/frames/rx`**
  (was `<prefix>/tx` / `<prefix>/rx`), and raw protocol frames are accepted on
  **`<prefix>/frames/send`** (was `<prefix>/send`). All other topics are unchanged
  (`flap/set|home`, `display/set|state`, `quiet/set|state`, `status`, `availability`, HA
  discovery). Clients on the old names — the MQTT serial bridge
  (`sfgw_serial_bridge.py` publishes `<prefix>/send`, subscribes `<prefix>/rx`) — must be
  updated. The dashboard's MQTT help text follows, and also drops the removed
  `maintenance/set` it still listed.

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
