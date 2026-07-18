# Architecture Notes

This document captures the reasoning behind non-obvious design decisions in the Matrix Portal
Gateway, so future maintainers (including future-you) don't have to re-derive them.

It assumes the [Split-Flap Gateway 3.1 architecture notes](../../SplitFlapGateway/3.1/ARCHITECTURE.md)
and only covers what this port changed or added. The task model, the watchdog, the choice of the
synchronous `WebServer`, the heap-stability work and the companion
settings blob are all inherited verbatim and are still true.

---

## The emulation seam is the UART, not the API

The single most important structural decision. `frameSend()` (`src/frames.*` — the physical
gateway's send choke point, renamed because there is no serial transceiver here) — which strips
terminators, trims anything past a complete well-formed command, re-frames, enforces Quiet
Time and mirrors to MQTT — is otherwise untouched. Where the physical gateway wrote the
finished bytes to a UART, it hands them to `vmDispatch()`.

That is the whole seam, and since v1.24 it is **one-way**: sanitize under `txMutex`, dispatch
under `vmMutex`, and nothing comes back. The virtual modules receive the same bytes a real
module would have received and act on the display commands (`-`, `+`, `h`); everything else in
the grammar is silently ignored, and nothing ever replies. (Through v1.23 the modules also
answered the `v`/`A` queries through a reply pipeline, `src/vlink.*`, that fed the same byte
accumulator the UART once fed. Nothing consumed those replies — every value in them was a
compile-time constant, and clients read the wall through `/api/display/state`,
`/api/flap/modules` and `/api/capabilities` — so v1.24 deleted the queries, the pipeline and
the accumulator; `taskFrames` now only drains the scheduled-batch ring.)

The alternative — short-circuiting at the API layer, so `POST /api/flap/char` directly sets a
cell — would have been a tenth of the code and would have tested nothing. The gateway's framing,
sanitization and Quiet Time enforcement would all have become dead code, and the first real
firmware change upstream would have silently diverged. Emulating at the protocol level keeps
every one of those paths live.

It also means the two "module" layers of the original survive intact and never touch each other
directly: `modules.*` is the gateway's side of the module protocol; `vmodule.*` is
what acts on it. They communicate only in ASCII frames.

## Why the mechanism is not emulated

There is no stepper, no Hall sensor and no EEPROM, so there is nothing to measure and nothing
that can be out of tune. Every module is a perfect one. That is not a shortcut so much as the
only coherent position: a simulated Hall sensor would report whatever we told it to, and a
simulated calibration would converge on the constant we seeded it with.

So the calibration, diagnostics, provisioning and backup commands are ignored rather than
faked — the sanitizer passes them through untrimmed, `vmDispatch()` has no case for them, and
nothing replies. (The last synthesized answers — the `v` version reply with its fabricated
`FA5E…` serial and the `A` reply with its nominal `homeOffset`/`totalSteps` and empty flap
map — went with the queries in v1.24. They only ever reported constants, and a `VModule` is
now ~16 bytes: an id and its runtime flip state.)

With no replies, buffer sizing is simple: `TX_MAX_BYTES` (512) is kept generous for raw frame
sends, and `MSG_MAX_BYTES` (320) caps what the MQTT wire mirror records per frame.
`MSG_MAX_BYTES` also sizes `mqttPublishMsg`'s stack buffer (`MSG_MAX_BYTES * 3 + 80`, since a
flap byte can expand to a 3-byte UTF-8 glyph), which `MQTT_BUF_SIZE` (1280) must be able to
hold. Raise one, check the other.

## The reel: 237 flaps, sectioned

The reel is **shared, fixed, and built at boot** — `reelBuild()` in `src/reel.h`, which is
deliberately Arduino-free so `tools/reel_test.cpp` compiles the *same* code rather than a copy
of it. It is derived from `isFlapByte()`, `cp1252IsLower()` and the font's `FONT_EXTRA_CP`
list — the code that already owns *which bytes exist*, *which are lowercase* and *which
pictographs we have* — so it cannot drift from the font or from the folding rule. It is not
stored per module and not configurable: a drawn reel that can already render everything has
nothing left to reconfigure, so the physical gateway's `N` command and flap-set editor are gone.

The section order is the contract: everything the legacy one-byte protocol can reach comes
first, at the indices it has always had, so growing the reel can never move a flap an existing
controller addresses by number:

```
0..155     the CP1252 glyphs, no lowercase        legacy: m<id>-<char>
156..162   the colour flaps  r o y g b p w        legacy: m<id>-r  == RED
---- everything below is unreachable from the legacy protocol ----
163..222   the 60 lowercase letters               index only
223..236   the pictographs (heart, smiley, ...)   index only
```

There are two resolvers, because the one-byte path has a problem it can never solve — the byte
for lowercase `r` already means red:

- **`reelIndexOf()` — the legacy path.** Colour codes first (which is what guarantees `r` can
  only ever be red), then splitflap-os's `q` → `"` alias, then fold to uppercase, then scan
  **only the 156 glyph flaps**. It never sees the lowercase or pictograph sections: as far as
  this protocol is concerned, they do not exist.
- **`reelIndexOfCodepoint()` — the index-addressed path** (`POST /api/display/cells`,
  `POST /api/flap/index`). No folding, no colour-stealing, and it reaches every flap; colours
  are *named* on this path, which is exactly why that API had to have a different shape.

Two details are not what they look like:

- **`q` is the double-quote flap** on the legacy path. The classic reel had no lowercase, so
  splitflap-os's char map borrowed that byte; the companion rewrites `"` → `q` before sending,
  and `vmFlapGlyph`/`drawFace` renders the flap back as `"`.
- **Folding has traps, and `charset.cpp` owns all of them**: `ÿ` uppercases to `0x9F`, not to
  an eszett; `÷` is not a letter and `ß` has no CP1252 uppercase, so both keep flaps of their
  own; `œ`, `š` and `ž` uppercase nowhere near a `0x20` offset.

**The legacy sections must stay byte-clean.** A pictograph has no CP1252 byte, which is why
the pictograph flaps live *past* `SF_LEGACY_FLAPS` (163): any byte-shaped surface that walks
the legacy sections — the legacy resolver's scan, or a client holding the classic reel — must
never meet a NUL in the middle of them.

`tools/reel_test.cpp` compiles the real `reel.h`, `charset.cpp` and `font1252.cpp` and asserts
all of it — the section bases, the colour-first resolution, every folding trap, that every
printable CP1252 character resolves to a flap, and that no NUL byte hides inside the 163
legacy flaps.

## There is no wire timing to emulate

The physical wire's whole character is timing: half-duplex serial at 9600 baud, 100 ms /
700 ms reply slots, broadcast trains that stagger over seconds, collisions when two modules
share an ID. None of it survives here, because since v1.24 **nothing replies at all**: a
frame is a function call into `vmDispatch()`, delivery is instant and one-way, and there is
no reply to meter, stagger or collide. (Through v1.23 the reply pipeline reproduced the
*ordering* of the wire — replies in module-ID order, `VLINK_SLOT_MS` apart — purely so a
broadcast train stayed legible in the MQTT mirror. With the queries gone that machinery had
nothing left to order, and it went too, along with `frameSend()`'s reply-quiet guard.)

The one piece of wire pacing that remains is deliberate and outbound: `/api/frames/batch`'s
`step_ms` staggers a cascade through the scheduled-TX ring, drained by `taskFrames` on a 5 ms
tick, because the companion's animation styles rely on frames landing spread out in time.
That is display choreography, not wire emulation — and the MQTT mirror
(`<prefix>/frames/tx`) is now the complete wire trace, because outbound frames are the whole
of the traffic.

## Locking

Six mutexes exist. Four are inherited (`msgMutex`, `timeMutex`, `mqttQMutex`, `txMutex`); two
are this product's: **`vmMutex`**, guarding the virtual-module array, and
**`txQMutex`**, guarding the scheduled-TX ring that paces `/api/frames/batch` off the web task.
(`sfMutex` went with the module registry in 1.10; the reply queue `vmMutex` also guarded went
with the reply pipeline in 1.24.)

Lock order is `txMutex → vmMutex`, never inverted: `frameSend()` holds `txMutex` for the whole
send and takes `vmMutex` just around `vmDispatch()`. Nothing takes `txMutex` while holding any
of the others — which is why no caller may hold `vmMutex` across `frameSend()`, and what makes
it safe for `frameSend()` to wait on `vmMutex` with `portMAX_DELAY` — and it must wait, because
timing out there would drop a command off the wall with no error anywhere.

One consequence worth remembering:

- **`dispRender()` snapshots under the lock and draws outside it.** The drawing loop touches
  thousands of pixels and must not sit on the frame path. The snapshot is one `{char, colour}` pair
  per cell for the current and incoming flap.

`vmDispatch()` has a `TX_MAX_BYTES` static scratch buffer with a single writer, because
`frameSend()` serialises every send through `txMutex` and that is the only path that reaches it.
A 512-byte frame will not fit on the 6–8 KB task stacks that call it.

## The panel

### The driver is our own: LCD_CAM + GDMA, no ISR, no library

`src/panel.cpp` drives the HUB75 panel directly off the ESP32-S3's LCD_CAM peripheral and GDMA,
with no external library. Three properties fall out of that and are load-bearing:

- **All thirteen HUB75 signals live in the 16-bit LCD data word**, and GDMA walks a circular
  descriptor chain clocking every one of them. There is no refresh ISR at all, so nothing external
  — WiFi, a flash write, a cache miss — can delay a bit-banged latch and let the OE window wander,
  which is the classic HUB75 shimmer.
- **Brightness is the OE duty cycle.** Dimming shortens the OE-on window, which costs no colour
  levels — unlike a multiply into every colour before `color565()`, which at four bitplanes would
  quantise dim colours onto the shortest, flickeriest pulse.
- **Bit depth is binary-code modulation.** Each bitplane `p` is linked `2^p` times in the
  descriptor chain (the data is not duplicated, only the descriptor pointing at it), so the
  weighting is in the DMA schedule, not in any per-pixel gamma lookup.

**The framebuffer cannot leave internal SRAM.** The board's 2 MB PSRAM is **quad** SPI, far too
slow to feed a display, so `panelBegin()` allocates both framebuffers and the descriptor chains
from `MALLOC_CAP_DMA` (internal by definition). That single fact bounds panel size and colour
depth together — and because WiFi/lwIP draw from the same internal pool, `dispInit()` runs
*before* `WiFi.mode()` in `setup()` so the panel gets first refusal, and `panelBegin()` **clamps
the bit depth down until the framebuffer leaves WiFi enough** (`PANEL_RAM_BUDGET`/
`PANEL_RAM_RESERVE` in `common.h`) rather than boot into a slow-motion malloc failure — since
1.17.1 it refuses outright only when even depth 1 does not fit, so an over-deep geometry dims
instead of going blank.

The same quad-vs-octal fact is why the pinout works at all — an octal-PSRAM S3 module consumes
GPIO 35/36/37, which this board uses for the HUB75 D, B and B2 lines.

**The pixel clock is 5 MHz, and that is a radio constraint.** The GDMA chain reads the
framebuffer out of internal SRAM continuously; at 10 MHz that traffic (~20 MB/s plus the
descriptor fetches) starved the WiFi MAC, which shares that SRAM — association failed with
`4WAY_HANDSHAKE_TIMEOUT` at close range and MQTT could not connect. 5 MHz still yields ~157 Hz
refresh at the default geometry, well above flicker. Do not raise `LCD_CLK_HZ`; if a long chain
ghosts, lower it.

Conversely, the command log, the MQTT queue and the scheduled-TX ring
are in PSRAM (`gwPsramAlloc`), precisely to leave that internal SRAM free.
The virtual-module array is the exception — it is pinned to internal RAM, because `taskDisplay`
walks it 100×/s on the core the refresh runs on, and a quad-PSRAM cache miss there shimmers the
wall. None of the PSRAM structures is on a hot path, in an ISR, or fed by DMA.

If `panelBegin()` still fails — a geometry too big even at depth 1 — the firmware logs why and
**runs headless**. The web UI, MQTT and the
whole protocol emulation still work; refusing to boot over a display fault would be worse.

### Geometry falls out of the grid

`cellW = panelW / gridCols`, `cellH = panelH / gridRows`, and then `font1252Best()` returns the
roomiest bundled face that fits with a one-pixel gutter on each axis. The gutter is empty
spacing between adjacent flaps (nothing is drawn in it since the decorative grid seam went in
v1.24); without it neighbouring glyphs touch.

`dispPlan()` is pure — no hardware touched — so `setup()` can call it *before* `vmInit()`. That
ordering matters: the plan may shrink the grid (a 15-column wall does not fit a 64 px panel), and
the wall **is** the module list, so the module count must come from the plan and never from the
raw config. A cell too small to hold a glyph is not a module.

### Why 5x7 and 4x6 are not bundled

They cannot draw accents. At 5×7 the X11 misc-fixed face has no room above the capitals and
renders `À` with a bitmap **identical to `A`** — likewise `Š` and `S`. The bundled fonts carry
the whole 216-glyph CP1252 set — the reel has a flap for every one of those glyphs, `Ä Ö Ü`
and the rest of the diacritics included — so this is a correctness bug, not an aesthetic
compromise: two distinct flaps would be indistinguishable.

This was found by a check that now lives in the generator. `tools/genfont.py` rasterises each face
and rejects it if any of eight accented/base pairs (grave, diaeresis, ring, cedilla, caron, acute
— both cases) come out byte-identical, if a glyph's ink spills past the declared width, or if any
glyph but the space is blank. Swapping in a different BDF cannot silently regress.

The bundled faces are seven — `10x20`, `9x18`, `8x13`, `6x13`, `6x10`, `6x9` and `5x8` — each
carrying the 216 CP1252 glyphs plus the 14 pictographs, ~41 KB of flash total. Rows are
**16-bit** with bit 15 leftmost: one byte per row silently capped every face at 8 pixels wide,
which is the real reason nothing bigger than `6x13` was bundled before 1.8.

### The flip is a rendering effect

Walking 64 flaps at the default 60 ms/flap takes ~3.8 seconds. `cfg.flapMax`
(≤ `FLAP_ANIM_MAX` = 64 — a physical reel's full revolution, kept as the cap even though this
reel has 237 flaps) caps one change at that many flips, and a longer jump starts its walk
`flapMax` flaps short of the destination. Nothing here models a motor: there is no travel time to
respect and no position to lose, so retargeting mid-flip simply changes the destination.

One flap is two half-steps: a mid-flip frame, then the settled frame. The mid-flip frame draws the
**incoming** flap's top half above the **outgoing** flap's bottom half, split by a bright seam.
That is what a split-flap physically does, and it is why the animation reads correctly even in an
8×10 cell where each half is five pixels.

The seam sits on the *glyph's* mid-line rather than the cell's, so it reads as the fold across the
character however much vertical margin the cell has. A settled flap draws no fold at all — just
its glyph.

Brightness is applied in the panel driver as the OE (output-enable) duty cycle, not as a scalar
into every colour — so dimming costs no colour levels (a multiply into every colour would, at
four bitplanes, quantise dim colours onto the shortest, flickeriest pulse or to zero).

`taskDisplay` steps the reels on a 10 ms tick but repaints at most every `DISP_MIN_FRAME_MS`, and
only when `vmTick()` reports that something moved. An idle wall costs one `vmTick()` per tick and
no repaint at all — which is how we knew an idle wall that still shimmered could not be blamed on
anything this firmware draws.

## Time

The MatrixPortal S3 has no battery-backed RTC — unlike the Waveshare board upstream, which carries
a PCF85063. `rtc.cpp` keeps the same API and serves it from the ESP32's internal RTC, seeded from
NTP with a zero offset (so the system clock is UTC and `mktime` acts as `timegm`).

The visible difference is that time is **invalid from power-on until the first sync**:
`rtcNow.valid` stays false and `rtcEpochNow()` returns 0. Every caller already handles that — it
is exactly the state an RTC with a flat cell produces. Frame timestamps skip work while the
clock is untrustworthy, and frame timestamps fall back to `HH:MM:SS` uptime.

The `setenv("TZ", …)` heap leak documented upstream is avoided the same way: TZ is set once at
boot and again only when the timezone changes; `rtcFormatTime()` computes the UTC epoch by hand.

## Partition table

`tinyuf2-partitions-8MB.csv` — the board's own scheme: 2 MB `ota_0` + 2 MB `ota_1` (so ArduinoOTA
and the browser uploader both work), a 256 KB `uf2` factory slot at `0x410000`, and 3776 KB of
FATFS.

Do **not** substitute a generic 8 MB table. The board manifest flashes `tinyuf2.bin` at
`0x410000`, which lands inside `app1` on most of them, and losing tinyuf2 costs the
double-tap-reset UF2 bootloader. The firmware currently uses **1.33 MB of the 2 MB app slot**
(63%), of which the 13 UI translation dictionaries are 61 KB. There is room for many more.

## Firmware identity vs API level

`GET /api/config` reports **`version: "3.1.0"`** — the *gateway API level* this firmware
implements, not its own version. The companion app parses `MAJOR.MINOR` out of that field and
gates its gateway-stored settings on `>= (3,1)`; reporting `1.0.0` would silently downgrade it to
local settings storage.

This firmware's own version lives in `fwVersion`, alongside `product`. `FW_VERSION` and
`API_VERSION` are separate macros in `common.h` for exactly this reason, and the comment there
says so.

v1.1 adds `GET /lang/<code>`, but `API_VERSION` deliberately stays at `3.1.0`. The language
endpoint is a *dashboard* concern, not part of the contract the companion negotiates. Raising
the API level to 3.5 to "match" the split-flap gateway would advertise calibration and
provisioning endpoints this product does not have and cannot have — its modules are drawn, not
driven.

## Multi-language UI

`src/web_ui.h` is **generated** from `ui/index.html` + `ui/strings/*.json` by
`tools/build_ui.py`. The page is no longer edited as a C string; edit the HTML and regenerate.
(`pio run` compiles the header, not the HTML, so a forgotten `build_ui.py` silently ships the
old page. `--check` exists to assert freshness.)

**Strings are keyed by their English text.** That is safe here because the UI has no homographs:
every string that repeats means the same thing in each place. Keying by text means the markup
needs no `data-i18n` tagging at all, so the static page is translated with zero edits to it.

Two mechanisms, because neither covers both cases:

- A **DOM walk + `MutationObserver`** translates text nodes. That covers the static page *and*
  the markup the JS builds later (the flap wall, the monitor rows, the quiet-day checkboxes) —
  the observer sees them when they are inserted.
- **`t("...")`** wraps messages the JS *composes* (`"Error: " + e`). A walk only ever sees the
  finished string, which is not a key.

Two traps are worth knowing about, because both fail *silently* (the string simply stays English
and nothing reports it):

- `is_key()` in `tools/i18n_extract.py` rejects a bare lowercase identifier-shaped word, because
  in markup that shape is a CSS selector or an element id, not prose. So `t("panel")` can never
  have a key. Composed messages therefore use labelled, capitalised or multi-word keys — which
  also read better for a translator, who sees `Panel` as a label rather than a bare noun.
- The catalog key is the **decoded** string. A source literal written `"Homing…"` is six
  characters of escape in the file but one ellipsis at runtime, which is what `t()` is handed.
  `js_unescape()` exists for exactly this.

**No language state lives on the gateway** — no config field, no NVS write, no API. Resolution is
the browser's: `?lang=` (a one-off view for the companion, deliberately not saved) → the Settings
override in `localStorage` → `navigator.languages` → English. Dictionaries are gzipped into flash
and served with `Content-Encoding: gzip`, which is *correct* here — those bytes are a transfer
encoding of JSON the browser inflates. (Contrast `/api/companion/settings`, where the gzip **is**
the payload and the header would be a lie.)

English ships no dictionary. It is the text already in the page, and the per-key fallback for
every other language, so a partial translation degrades to English instead of breaking.
