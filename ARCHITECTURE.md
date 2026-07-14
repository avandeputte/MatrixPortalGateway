# Architecture Notes

This document captures the reasoning behind non-obvious design decisions in the Matrix Portal
Gateway, so future maintainers (including future-you) don't have to re-derive them.

It assumes the [Split-Flap Gateway 3.1 architecture notes](../../SplitFlapGateway/3.1/ARCHITECTURE.md)
and only covers what this port changed or added. The task model, the watchdog, the choice of the
synchronous `WebServer`, the heap-stability work and the companion
settings blob are all inherited verbatim and are still true.

---

## The emulation seam is the UART, not the API

The single most important structural decision. `rs485Send()` — which strips terminators, trims
anything past a complete well-formed command, re-frames, enforces Quiet Time, updates display
tracking, pushes to the monitor ring and mirrors to MQTT — is untouched. Its last three lines
call `vbusDeliver()` instead of writing the frame to a UART.

Everything above that line is the gateway, unmodified. Everything below is new. The virtual
modules receive the same bytes a real module would have received, and reply with frames that go
back up through the *same* byte accumulator, the same `ringPush`, the same `mqttPublishMsg`, the
same `sfParseResponse`.

The alternative — short-circuiting at the API layer, so `POST /api/flap/char` directly sets a
cell — would have been a tenth of the code and would have tested nothing. The gateway's framing,
sanitization, collision guard, staggered-broadcast handling and reply parser would all have
become dead code, and the first real firmware change upstream would
have silently diverged. Emulating at the protocol level keeps every one of those paths live.

It also means the two "module" layers of the original survive intact and never touch each other
directly: `modules.*` is the gateway's side of the module protocol; `vmodule.*` is
what answers. They communicate only in ASCII frames.

## Why the mechanism is not emulated

There is no stepper, no Hall sensor and no EEPROM, so there is nothing to measure and nothing
that can be out of tune. Every module is a perfect one. That is not a shortcut so much as the
only coherent position: a simulated Hall sensor would report whatever we told it to, and a
simulated calibration would converge on the constant we seeded it with.

So the calibration, diagnostics, provisioning and backup commands have been removed outright
rather than faked, and the surviving `A` reply always reports the nominal values with an
**empty flap map**. Reading back what you wrote would have meant storing it, and storing it
would have meant a 64-entry `uint16_t` map per module.

Combined with the 64-flap reel (below), that keeps the frames small, and the frames are what
size the buffers:

| | RS-485 gateway (64 flaps, real map) | here (64 flaps, no map) |
|---|---|---|
| worst-case `A` reply | ~570 B | **~117 B** |
| `TX_MAX_BYTES` | 768 | **512** |
| `MSG_MAX_BYTES` | 256 | **320** |
| `MQTT_BUF_SIZE` | 1024 | **1280** |

The `A` reply carries a `flapChars` tail but no flap map, so its worst case is ~117 B — a real
module's is ~570 B because it also serialises the 64-entry position map. `TX_MAX_BYTES` (512) is
kept generous for raw frame sends, and `MSG_MAX_BYTES` (320) is large enough that a whole `A`
reply lands in one monitor-ring entry untruncated, which is the frame you most want to read.

`MSG_MAX_BYTES` also sizes `mqttPublishMsg`'s stack buffer (`MSG_MAX_BYTES * 3 + 80`, since a
flap byte can expand to a 3-byte UTF-8 glyph). Raise one, check the other.

## The reel: 64 flaps, German by default

The reel is 64 flaps — the same count as a real module — because the flap **count** is protocol:
the `A` reply reports `flapCount`, and the [companion](../../SplitFlapGatewayCompanion) (and the
real controllers) reject any `flapCount` outside 1..64. A larger virtual reel would make every
`flapCount` sync fail silently and the controller fall back to its own map.

The default layout is German — `VM_DEFAULT_REEL` in `vmodule.cpp`:

```
 ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜß0123456789!@#&€-=;q:'.,/?*roygbpw
```

Flap 0 is blank; the seven colour flaps sit at the **end** (indices 57..63), and `VM_COLOUR_BASE`
is *defined* from that (`SF_MAX_FLAPS - SF_COLOUR_FLAPS`), so they must stay last.

Two positions are not what they look like:

- **The colour flaps are the last seven.** `flapIndexOf()` is a first-match scan, and the only
  lowercase bytes on the reel are `q` and the seven colour codes — so `r` resolves to red (index
  57) and never to the letter `R` (a distinct byte at index 18). This matches the physical reel's
  ordering, which is the whole point.
- **`q` (index 49) is the double-quote flap.** The real reel has no lowercase, so the firmware's
  char map has always addressed the `"` flap as `q`; the companion rewrites `"` → `q` before
  sending. `dispFlapGlyph`/`drawFace` renders it back as `"`.

Two knock-on effects:

- **`sfSendChar()` uppercases again.** The 64-flap reel has no lowercase, so ASCII a–z is folded
  to A–Z — *except* the seven colour codes `roygbpw`, which stay lowercase and resolve to colours.
  Accented lowercase is folded too (`ä` → `Ä`), skipping `0xF7` (÷, not a letter) and `0xFF`
  (`ÿ`, whose CP1252 uppercase is `0x9F`, not `0xDF` — folding it blindly would forge an eszett).
- **The reel is length-counted, not sentinel-terminated.** An `N` command may set an arbitrary
  reel, and the real firmware reserves `0xFF` as its "unused flap" sentinel, so `flapChars` is a
  byte array with an explicit `flapCharsLen`, not a C string.

`tools/reel_test.cpp` compiles against the real `charset.cpp` and asserts these invariants — the
reel length, the colour positions, the first-match resolution and the `A` reply's colon arithmetic.

## Why the bus timing is *not* faithful

The emulated bus is deliberately **not** metered at `cfg.rs485Baud`, held half-duplex, or
staggered at the protocol's 100 ms / 700 ms reply slots.

At 9600 baud even a short reply takes tens of milliseconds to clock out, and a broadcast `m*v`
across 45 modules would stagger over several seconds of reply slots — faithful, and pure cost.
Nothing is learned by making a virtual bus slow.

What remains is the ordering, not the pacing: replies are queued as *intents* (not text), and
`vbusPoll()` renders the earliest-due one into a single shared buffer. A broadcast still produces
one frame per module in module-ID order, `VBUS_SLOT_MS` apart, and ranged batch queries
(`m*v0-49`) are still honoured. `rs485Send()`'s bus-quiet guard still runs — it can no longer
prevent a corruption that cannot happen, but it keeps command and reply frames from interleaving
in the monitor, and it keeps the code path identical to the one upstream runs.

Queueing intents rather than rendered text is what makes a broadcast `m*A` cheap: 45 rendered
replies held resident would be several KB, and more on a larger wall.

The one thing genuinely lost is **collisions**. Two modules sharing an ID answer one after the
other instead of on top of each other, so the gateway's duplicate-ID heuristic — which keys off
garbled serial numbers surviving intact framing — never fires. Every other consequence of a
duplicate ID is reproduced.

## Locking

Four mutexes are inherited (`sfMutex`, `msgMutex`, `timeMutex`, `mqttQMutex`, `txMutex`); one is
new: **`vmMutex`**, guarding the virtual-module array and the reply queue.

Lock order is `txMutex → {sfMutex, msgMutex, vmMutex}`, never inverted. Nothing takes `txMutex`
while holding any of the others, which is what makes `vbusDeliver()` safe to wait on `vmMutex`
with `portMAX_DELAY` — and it must wait, because timing out there would drop a command off the
wall with no error anywhere.

Two consequences worth remembering:

- **`vmSave()` takes `vmMutex` per record, not across the whole write.** Holding it for an ~11 KB
  FATFS write would stall `vbusDeliver()` — and therefore every command the gateway sends — for
  the duration. A record torn across the boundary is harmless: this is persistence, not a
  transaction.
- **`dispRender()` snapshots under the lock and draws outside it.** The drawing loop touches
  thousands of pixels and must not sit on the bus path. The snapshot is one `{char, colour}` pair
  per cell for the current and incoming flap.

`vmDispatch()` has a `TX_MAX_BYTES` static scratch buffer with a single writer, because
`rs485Send()` serialises every send through `txMutex` and that is the only path that reaches it.
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
*before* `WiFi.mode()` in `setup()` so the panel gets first refusal, and `panelBegin()` **refuses
a geometry whose framebuffer would leave WiFi too little** (`PANEL_RAM_BUDGET`/`PANEL_RAM_RESERVE`
in `common.h`) rather than boot into a slow-motion malloc failure.

The same quad-vs-octal fact is why the pinout works at all — an octal-PSRAM S3 module consumes
GPIO 35/36/37, which this board uses for the HUB75 D, B and B2 lines.

**The pixel clock is 5 MHz, and that is a radio constraint.** The GDMA chain reads the
framebuffer out of internal SRAM continuously; at 10 MHz that traffic (~20 MB/s plus the
descriptor fetches) starved the WiFi MAC, which shares that SRAM — association failed with
`4WAY_HANDSHAKE_TIMEOUT` at close range and MQTT could not connect. 5 MHz still yields ~157 Hz
refresh at the default geometry, well above flicker. Do not raise `LCD_CLK_HZ`; if a long chain
ghosts, lower it.

Conversely, the monitor ring, the MQTT queue, the scheduled-TX ring and the
*saved* module state are in PSRAM (`gwPsramAlloc`), precisely to leave that internal SRAM free.
The virtual-module array is the exception — it is pinned to internal RAM, because `taskDisplay`
walks it 100×/s on the core the refresh runs on, and a quad-PSRAM cache miss there shimmers the
wall. None of the PSRAM structures is on a hot path, in an ISR, or fed by DMA.

If `panelBegin()` fails, the firmware logs why and **runs headless**. The web UI, MQTT and the
whole emulated bus still work; refusing to boot over a display fault would be worse.

### Geometry falls out of the grid

`cellW = panelW / gridCols`, `cellH = panelH / gridRows`, and then `font1252Best()` returns the
roomiest bundled face that fits with a one-pixel gutter on each axis. That gutter is the seam
drawn between adjacent flaps; without it neighbouring glyphs touch.

`dispPlan()` is pure — no hardware touched — so `setup()` can call it *before* `vmInit()`. That
ordering matters: the plan may shrink the grid (a 15-column wall does not fit a 64 px panel), and
the wall **is** the module list, so the module count must come from the plan and never from the
raw config. A cell too small to hold a glyph is not a module.

### Why 5x7 and 4x6 are not bundled

They cannot draw accents. At 5×7 the X11 misc-fixed face has no room above the capitals and
renders `À` with a bitmap **identical to `A`** — likewise `Š` and `S`. The bundled fonts carry
the whole 216-glyph CP1252 set (so any reel, including custom `N`-command reels, can be drawn),
and the German default reel already needs `Ä Ö Ü`, so this is a correctness bug, not an aesthetic
compromise: two distinct flaps would be indistinguishable.

This was found by a check that now lives in the generator. `tools/genfont.py` rasterises each face
and rejects it if any of eight accented/base pairs (grave, diaeresis, ring, cedilla, caron, acute
— both cases) come out byte-identical, if a glyph's ink spills past the declared width, or if any
glyph but the space is blank. Swapping in a different BDF cannot silently regress.

The bundled faces are `6x13`, `6x10`, `6x9` and `5x8` — 216 glyphs each, 8.6 KB of flash total,
one byte per row with bit 7 leftmost.

### The flip is a rendering effect

Walking a full 64-flap reel at the default 60 ms/flap takes ~3.8 seconds. `cfg.flapMax`
(≤ `FLAP_ANIM_MAX` = 64) caps one change at that many flips, and a longer jump starts its walk
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
  the markup the JS builds later (module cards, the flap wall, the status tiles) — the observer
  sees them when they are inserted.
- **`t("...")`** wraps messages the JS *composes* (`"Error: " + e`). A walk only ever sees the
  finished string, which is not a key.

The bus monitor (`#log`) is explicitly skipped by the walk. It renders protocol — the raw command
text and the channel it arrived on — not chrome.

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
