# Matrix Portal Gateway — Release Notes

## v3.15.0 — 2026-08-01

### Added — gestures: clap and tap detection

- **Clap detection** (`claps` token, `clapEnabled` setting): a transient detector in the
  mic DSP — attack over the room floor plus a *not-bass* spectral gate calibrated from
  real claps (on these mics a clap is mid-spread, not bright; a music thump is bass-heavy).
  Claps in a burst are counted (double-clap → count 2).
- **Tap detection** (`taps` token, `tapEnabled` setting): the QMI8658's on-die tap engine
  (accel 4 g @ 500 Hz), thresholds live-calibrated for comfortable knuckle-taps.
- **Events over SSE**: `event: clap` / `event: tap` with `{count, seq}` — the companion
  can bind single/double gestures to actions. `GET /api/gestures` carries state, lifetime
  counters, and tuning telemetry.
- **A double gesture dismisses a running timer or ringing alarm on-device** (clap the
  timer away, tap the alarm quiet), audit-logged to the card; singles are the companion's
  to interpret. Human-rhythm pairing: two events within 1 s count as a double.
  Live-verified: comfortable double-tap dismisses in 2 s, double-clap in 8 s; a 4-minute
  ambient control produced zero false dismissals.

### Fixed — the RTC tells the truth now
Four intertwined bugs: PCF85063 CAP_SEL mis-set (crystal counted ~2× real time — +1 day
of drift per day), circular NTP discipline (`getLocalTime` accepted the chip's own seed
as "synced", so the write-back echoed the chip's error back forever — now waits for real
SNTP completion), write-back on the wrong I2C task (now on taskRTC with read-back verify
and a logged crystal rate check), and timezone-skewed register conversion. Measured after:
crystal at 119 s per 120 s real, write-back verified within 0 s, boot log stamps correct.

Also: taskRTC stack 3072 → 4096 (a mid-fix build blew its stack canary and boot-looped;
the panic-recovery FATFS reformat wiped uploaded animations/fonts/atlases — re-upload if
missed), gesture/SD-log plumbing hardened, `/api/gestures` JSON truncation fixed.


## v3.14.0 — 2026-07-31

### Added
- **Kitchen timer + daily alarms.** `POST /api/timer {"sec":N|"min":N}` puts a full-screen
  countdown on the panel (big anti-aliased digits, amber flash in the final 10 s), chimes
  at zero, holds a flashing 00:00 for ~10 s, then returns the panel to whatever was
  running. Four daily alarm slots (`GET/POST /api/alarms`, persisted, per-day bitmask)
  ring for up to 90 s with a red flashing screen. **Alerts override Quiet Time by design**
  (the speaker master enable still applies) and outrank every display mode, including a
  live companion canvas. Dashboard card on the Display tab (quick 5/10/15-min buttons,
  custom duration, alarm editor); `timer` capability token.
- **Event Log card** on the Status tab: the tail of the on-card `/logs/gateway.log`
  (boots + causes, watchdog reasons, OTA, heap heartbeats) with refresh + download —
  powered by a new `tail=N` option on `GET /api/sd/get`.


## v3.13.2 — 2026-07-31

### Added — a board that explains its own failures

Born of an unexplained reboot that left no evidence (nothing on USB, volatile logs lost):
- **Reset-cause ring** — `GET /api/status` now carries `resets`: the last 8 reboots as
  `[cause, minutes-alive]` pairs, held in RTC memory so they survive warm reboots
  (causes: 1 poweron, 3 sw, 4 panic, 5/6/7 watchdogs, 9 brownout, 11 USB).
- **On-card event log** — `/logs/gateway.log` on the microSD records boot post-mortems
  (reset cause, previous uptime, panic count), watchdog reboot reasons, GDMA self-heals,
  OTA events, and a 10-minute heap heartbeat. Rotates at 512 KB. Read it from the Files
  tab or `GET /api/sd/get` — no USB needed.

### Fixed
- **taskRTC stack 2048 → 3072.** The brightness-schedule tick pushed the firmware's
  leanest task to a 496-byte minimum under soak — a credible cause of the unexplained
  reboot. Post-fix minimum: 1392 bytes across an 8 h adversarial soak, zero alerts.
- **SD downloads keep their filename** — `/api/sd/get` was missing the
  `Content-Disposition` header, so browsers saved everything as "get".

Certified: 8 h adversarial soak — 223,343 steps, 0 reboots, 0 soft errors, 0 SSE drops,
658/658 SD round-trips, heap floor 127 K flat, +4 KB drift. (v3.13.1 was the interim
reset-ring build, folded in here.)


## v3.13.0 — 2026-07-30

### Added — the card earns its keep, plus a fourth audio visual

- **SD animation streaming.** `POST /api/canvas/anim/play {"path":"/movies/x.mpg"}` plays
  an MPGA straight from the microSD card, one 32 KB frame in memory at a time — the 8 MB
  PSRAM cap is gone; length is bounded only by the card. Verified with a 19.7 MB /
  600-frame movie looping with the heap untouched.
- **WAV playback from SD.** `POST /api/sound {"wav":"/sounds/chime.wav","vol":80}` streams
  real audio through the speaker (strict 16-bit 16 kHz mono/stereo PCM — the duplex I2S
  clock is fixed). Runs on the same synth task that plays tones, so there is exactly one
  writer to the TX channel; Quiet Time and the master enable/volume apply as ever.
- **Spectrogram effect** (`spectro`). A scrolling frequency-vs-time waterfall of the mic:
  newest column on the right, history marching left via the exact-round-trip panelScroll,
  16 log bands interpolated over the panel height through the fire palette. `speed` sets
  the scroll rate.
- **Brightness schedule.** Settings → Brightness Schedule: auto-dim the panel in a daily
  local-time window (e.g. evenings), with its own dim level. Edge-triggered, so a manual
  brightness change inside the window sticks until the next boundary; **Quiet Time takes
  precedence** (while quiet the panel is blanked and the schedule waits, re-applying the
  right level the moment quiet ends).

All four verified on the 256×64 board (waterfall + tone sweep, audible chime, 600-frame
card stream with frames advancing, live dim/restore on schedule edges).

## v3.12.0 — 2026-07-29

### Removed — legacy-client support (breaking, by design)

The project is incubating with the companion kept in lockstep, so compatibility shims
for hypothetical old clients are gone:
- **One honest version.** `GET /api/config` now reports the real firmware version in
  `version` (the fixed `"3.1.0"` API-level masquerade is deleted, along with the `api`
  field in capabilities). Clients key behaviour on `product` + capability tokens.
- **`effectParams` is gone** from capabilities — `effectDefs` (fw 3.4) is the one
  source of effect parameters, and the param vocabulary is no longer order-constrained
  by the legacy flat list.
- Dead `boardId32()` removed; the NVS cross-product settings-portability claim dropped
  (key names unchanged, so settings survive the upgrade); stale "quad PSRAM too slow"
  and "legacy path" comments corrected.
- Companion must be updated in the same step (it reads `effectParams` and gates its
  settings-blob feature on `version >= 3.1`).

### Added — the binary encoding carries the full ops surface

The binary encoding (`POST /api/canvas/opsb` + stream record `0x06`, advertised as
`canvas.opsBin`) now expresses everything the JSON decoder does — one format, one
opcode table.

- **Transform stack**: `0x16` SAVE / `0x17` RESTORE / `0x18` TRANSLATE / `0x19` SCALE
  (u16 8.8 fixed-point) / `0x1A` ROTATE. Every coordinate now runs through the same
  affine matrix as the JSON decoder — vertex ops rotate fully, box fills anchor+scale.
- **Offscreen layers**: `0x1B` LAYER / `0x1C` COMPOSITE (x y mode alpha) — group opacity.
- **Macros**: `0x1D` DEFINE (id 0–7 + length-prefixed embedded binary ops) / `0x1E` CALL
  (scoped replay under a pushed translate, nestable to depth 4) — stamp a sprite-like
  shape at game rate without resending its ops.
- **`0x1F` BEZIER** (quadratic/cubic, aa flag) and **`0x20` AALINE**; additive `aa` flag
  bits on CIRCLE (bit 1) and POLY (bit 2).
- Cleanup: the decoders now share one transform implementation (the legacy binary
  translate-only globals are gone).

Also fixes the device-served `/openapi.yaml` being invalid YAML since v3.11.1 (an
unquoted `?` in the mkdir endpoint description broke parsers).

Verified on-device via readback: rotated binary line renders vertical (21/21 px),
save/restore isolation, 2× scale, layer composite at α128, macro stamping, bezier +
AA line, the circle aa flag, and format-1 ORIGIN behaviour unchanged.

## v3.11.1 — 2026-07-29

### Fixed — comprehensive-review sweep

A five-reviewer audit of the whole firmware. Six bugs fixed:
- **Oscilloscope died after 3 s** — the effect was missing from the audio-capture consumer
  list, so capture self-stopped and the trace went flat. Now listed; verified live at 6.5 s.
- **SD-browser XSS** — card filenames were rendered into the dashboard unescaped; a crafted
  name could execute script in the gateway's origin. Filenames are now HTML/attribute-escaped.
- **Animation play-by-name use-after-free** — the render task wasn't parked while the named
  file reloaded the frame store (the raw upload path always did this).
- **Atlas upload/delete race** — now blocked while a canvas stream is open (the pump could be
  mid-blit from the very sheet being freed).
- **A 60 fps animation could be saved but never loaded back** (fps round-trip truncation).
- **Speaker shutdown race** — notes enqueued at the exact moment the synth idled out were
  silently dropped.

Plus hardening (CORS `DELETE`, recursive-SD-delete watchdog/stack guards, macro `call` state
isolation, SSE empty-event guard, atlas/rename/urlDecode edge cases, a font cache that removes
a per-text-op FATFS read), dead-code removal, and ~14 comment corrections.

Certified: 2 h adversarial soak on the release binary — 63,192 steps, 0 reboots, 0 errors,
0 SSE drops, 188/188 SD round-trips byte-identical, heap floor 139 K (drift −184 B).

## v3.11.0 — 2026-07-28

### Added — depth-4 color via a PSRAM framebuffer (experimental)

A new **Framebuffer in PSRAM** toggle (Settings → LED Panel; off by default) moves the panel
framebuffer from internal DMA SRAM into the board's octal PSRAM (80 MHz). That lifts the
internal-RAM cap that forced 256×64 down to **depth 3 (8 levels/channel)** — with it on, the
panel runs **depth 4 (16 levels/channel)** for visibly smoother gradients.

Why it works on this board specifically:
- **Octal PSRAM at 80 MHz** has ~16× the bandwidth the 5 MHz pixel clock needs; the earlier
  "PSRAM too slow" note was inherited untested from the quad-PSRAM MatrixPortal.
- **Cache coherency** is handled by flushing the finished frame (`esp_cache_msync`, C2M) at the
  double-buffer swap in `panelShow`, plus a 64-byte GDMA PSRAM burst alignment.
- **Refresh rate**: depth 4 doubles the words per frame, which at 5 MHz is only ~40 Hz and
  flickers — so the PSRAM path also raises the pixel clock to **10 MHz (~80 Hz)**. The
  MatrixPortal's radio broke at 10 MHz; this Waveshare board survives it (previously A/B'd).

Verified on the 256×64 board: exactly 16 distinct grey levels (vs 8 at depth 3), pixel-exact
solid colors, no flicker at 80 Hz, WiFi unaffected (60/60 requests, 18 ms median under load),
and ~93 KB of internal RAM freed as a side effect. Reboot to apply; turn it off + reboot to
fall straight back to the rock-solid depth-3 internal-SRAM path.

## v3.10.0 — 2026-07-28

### Added — microSD, an oscilloscope effect, and faster effects

- **microSD support.** The onboard TF card slot is now mounted at boot (SD_MMC 1-bit on the
  Waveshare-documented pins: CLK=1, CMD=44, D0=17). Browse, download, upload and delete via
  `GET /api/sd`, `GET /api/sd/list`, `GET /api/sd/get`, `PUT /api/sd/put`,
  `DELETE /api/sd/delete` — paths are card-absolute and `..`-guarded. Card size/usage shows
  on the Status page and rides `GET /api/status` under `sd`; advertised as the `sd`
  capability token (present only when a card is mounted). Absent-card and mount failures are
  handled gracefully — endpoints answer 503, exactly like the speaker/sensor when absent.
  The dashboard **Files tab** gains a microSD browser (appears only when a card is mounted):
  capacity bar, directory navigation, per-file download/delete, and upload to the current
  folder.
- **Oscilloscope effect** (`scope`). A live time-domain trace of the mic waveform: the
  DC-removed mono hop, auto-gain-scaled so quiet rooms still fill the trace, drawn as a
  continuous phosphor-green line over a dim centre reference, flashing white-hot on a beat.
  `hue` recolours it. Rounds out the audio-reactive set (spectrum / soundwall / ripple).
- **Row-buffer blitters for the effects path.** Plasma and fire — the two effects that touch
  every pixel every frame — now assemble one RGB888 row and hand it to the panel's fast
  blit path (one quantise + one word-loop per bitplane) instead of `W×H` per-pixel calls.
  ~4–6× faster on those renders per the driver notes, which buys headroom for the audio
  effects. Output is pixel-identical (verified: plasma 100 % fill, fire unchanged).

All three verified on-device: plasma/fire via readback, the oscilloscope tracing a played
tone (centre line across all columns, deflection in 203/256 columns), and the SD REST
surface routing correctly (info 200, file ops 503 with no card fitted).

## v3.9.0 — 2026-07-28

### Added — the last of the canvas/ops surface

Three features that complete the drawing model, all JSON-ops (the binary encoding keeps its
lean translate-only path). Advertised under `canvas.compositing` in capabilities.

- **Transform stack.** `translate` / `scale` / `rotate` compose an affine transform;
  `save` / `restore` push and pop it (depth 8). Point/line ops (`line`, `polyline`, `poly`,
  `triangle`, `bezier`, `pixel`) transform every vertex, so **rotation works** — a
  horizontal line under `rotate 90` draws vertical. Box-anchored fills (`rect`, `circle`,
  `ellipse`, `roundrect`, `arc`) transform the anchor and scale the size but stay
  axis-aligned (rotate those via a sprite's own `rot`). `origin` stays as the
  backward-compatible pure-translate reset.
- **Offscreen layers.** `{"op":"layer"}` redirects drawing into a full-panel RGBA shadow;
  `{"op":"composite","x","y","mode","alpha"}` blends the whole group back with one group
  blend + opacity — **group opacity**, which per-op alpha can't express (fade an entire
  composed scene as a unit). Buffer lives in PSRAM only between `layer` and `composite`;
  an unclosed layer is discarded at batch end.
- **Ops macros.** `{"op":"define","name","ops":[…]}` registers a reusable op sequence;
  `{"op":"call","name","x","y"}` replays it under a pushed transform translated by `(x,y)`.
  Batch-scoped (≤12 macros), nestable to depth 4 — stamp a shape across the panel without
  re-sending its ops.

Verified pixel-exact on-device via readback: rotated line goes vertical, `save`/`restore`
isolates a nested translate, macros stamp at translated offsets, and a 50%-alpha blue layer
composites over red into a clean purple `(109,0,146)`.

## v3.8.1 — 2026-07-28

- **Stroke styling** on `line`/`polyline`/`poly`: `cap` (`butt`/`round`/`square`),
  `join` (`miter`/`round` — round fills thick-polyline corners), and `dash`
  (`[on,off]`, flowing continuously along a path). Rounds out the drawing surface.

## v3.8.0 — 2026-07-28

### Added — compositing and smooth drawing

The canvas ops surface gains what set it apart from GFX-class libraries: **compositing**.
- **Per-colour alpha** — `"color":[r,g,b,a]` on any drawing op composites over the back
  buffer.
- **Blend modes** — `{"op":"blend","mode":"over|add|multiply|screen|max"}`, batch-scoped.
  `add` is additive — the LED-glow mode where overlapping lights sum, which looks right on
  a matrix in a way it can't on paper.
- **Anti-aliased strokes** — `"aa":true` on `line`/`polyline`/`poly`/`circle` (Xiaolin-Wu
  lines, coverage-blended rings); coverage rides the compositing path, so AA honours alpha
  and blend mode.
- **`bezier`** — quadratic (3 points) / cubic (4) curves, AA or thick. The chart/gauge
  primitive.
- All of the above are in the **binary ops** encoding too (blend `0x14`, alpha `0x15`),
  and advertised as `canvas.compositing` in capabilities.

Verified pixel-exact on-device (alpha 50%% → clean midpoint blend, additive doubles,
multiply/screen, AA edge coverage, bezier curvature). Foundation: a per-pixel
read-modify-write in the panel driver, reusing the framebuffer readback path.

## v3.7.3 — 2026-07-27

- **Panel Transitions on the gateway's Settings tab.** The persisted transition
  (v3.7.2) now has a control card on the dashboard — type (none/crossfade/wipe/slide)
  and duration — feature-gated on the `canvas` capability.

## v3.7.2 — 2026-07-27

### Fixed

- **Canvas transitions now persist.** `POST /api/canvas/transition` was runtime-only,
  so the configured crossfade/wipe/slide reset to hard cuts on every reboot and reflash.
  It is now saved to NVS and restored at boot; `GET /api/config` reports it as
  `transitionType` / `transitionMs`.

## v3.7.1 — 2026-07-27

### Fixed

- **Quiet Time now blanks canvas content, not just flaps.** The raw-canvas endpoints
  (`frame`, `ops`, `opsb`, `rects`, `rect`, `qoi`, `stream`, `anim`, `gif`) had no
  Quiet-Time guard, so a companion pushing a canvas app lit the panel right through
  Quiet Time even though the flap path was suppressed. They now refuse with `409`
  while Quiet Time is on — the panel stays dark, matching effects, ticker, anim-play
  and sound (which already refused). The one flap page in a mixed playlist is no
  longer the only thing suppressed.

## v3.7.0 — 2026-07-27

### Added — the board reads the room

- **Onboard temperature + humidity** from the board's Sensirion **SHTC3** sensor
  (`environment` feature token). Polled every ~10 s on the one task that owns runtime
  I2C (the bus has no lock), with CRC-checked readings. Exposed three ways:
  `GET /api/environment` (`{available, tempC, tempF, rh, ageMs}`), an `env` object in
  `GET /api/status`, and an **Environment card on the dashboard Status page** (shown
  only when the sensor is present). Note it reads a few degrees warm — it sits next to
  the LED panel and the ESP32 —
  correct it with the **temperature offset** on the Settings page (`tempOffset`, °C;
  `GET /api/environment` also reports `rawTempC`, `rawRH` and `offsetC`). **Humidity is
  corrected from the same offset** — a hot sensor reads relative humidity low, and the
  temperature delta recovers the true ambient RH by the psychrometric (Magnus)
  relation, so one calibration fixes both temperature and humidity.

### Fixed

- **Dashboard Status page: the lower half is populated again.** A dead reference to the
  MQTT status element (removed in v3.0) threw in `renderStatus` and aborted it, leaving
  Modules, Min Heap Ever, Stack Min, Gateway Time and NTP Sync blank since v3.0. Removed.
- Status stat boxes are wider and no longer break values mid-token (IPs, temperatures).

## v3.6.1 — 2026-07-27

- **Quiet Time now silences the speaker.** Turning Quiet Time on stops any tone
  mid-play, and `soundPlay` refuses while quiet — the speaker follows the same
  "dark and silent" rule as the panel.
- **Settings: a Speaker card** (shown only when the wall has the `sound` capability)
  with a master **enable** toggle, a **volume** slider, and a Test button. Persisted
  as `soundEnabled` / `soundVolume` in `GET/POST /api/config`: the enable gates
  `POST /api/sound` (`403` when off), the volume scales every call
  (`effective = vol × soundVolume / 100`).

## v3.6.0 — 2026-07-27

### Added — the board has a voice

- **`POST /api/sound`** (`sound` feature token): tones and note sequences on the
  board's ES8311 DAC + power amp — `{"freq":880,"ms":200,"vol":60}` or
  `{"notes":[[880,120],[0,40],[1320,160]],"vol":60}` (freq 0 = rest, ≤32 notes,
  ≤2 s/note), `{"stop":true}`, `GET` for state. Synthesized on-device (16 kHz sine,
  3 ms anti-click envelopes), refused during Quiet Time, amp powered only while
  playing (+5 s), self-stopping synth. Game feedback, chimes, alerts. PCM sample
  playback deliberately deferred.
- The microphone I2S port became **full-duplex** to carry it (one wired clock set
  serves both codecs). One hard-won hardware fact is documented in the driver: **the
  TX side only clocks while RX is enabled**, so the RX channel stays up as the
  port's clock heartbeat — the microphone *pipeline* still runs only while audio
  effects want it.

### Improved — gradients grew up

- `gradient` gains **radial** (`dir:"r"`) and **angled** (`"angle":degrees`) modes,
  and **ordered (Bayer 4×4) dithering, on by default** — banding at 3–4 bitplanes
  breaks into a fine blend (`"dither":false` restores hard bands). Binary ops:
  gradient `dir` byte accepts 2 = radial.

### Certification

2-hour adversarial soak on the 256×64 board, release binary (full rotation with
stream bursts incl. binary ops, full-wall pages, all effects, and near-silent
`/api/sound` blips every 9th step to exercise the synth/I2S-TX/amp path):
**2,273 steps, 0 reboots, 0 errors, 0 refusals, min-heap 48.6 KB, heap drift +212 B,
34,244 SSE events / 0 drops, 252 stream bursts / 5,340 records / 0 errors,
2,821/2,821 polls.** Serial captured throughout: no panics, no stalls, no synth write
failures. The ~5 KB floor delta vs v3.5.0 is the full-duplex I2S port's resident TX
DMA buffers plus the always-on RX clock heartbeat.

## v3.5.0 — 2026-07-26

### Added — the ops surface grows up

Nine additions that take `POST /api/canvas/ops` past GFX-class libraries and close to
HTML-canvas ergonomics (all shared with the stream channel's ops record):

- **`arc`** — arcs and pie slices: `{x,y,r,t,start,end,fill}`, 0° at 12 o'clock,
  clockwise. The gauge/meter primitive.
- **`poly`** — closed polygons, even-odd scanline filled (up to 16 vertices) or
  outlined with thickness.
- **Thickness (`t`) everywhere** — `line`, `polyline`, and `rect`/`circle`/`ellipse`
  outlines take a stroke width; circles render a true annulus.
- **`clip`** — clip all subsequent ops to a window (bare `clip` clears). Batch-scoped;
  enforced down in the pixel/fill/blit fast paths so every primitive honours it.
- **`origin`** — translate all subsequent coordinates (including `points` arrays);
  clients can build placeable components. Batch-scoped.
- **Anti-aliased text** — `{"op":"text","aa":true,"size":N}` renders smooth Orbitron
  (34/24/13 px faces; A–Z 0–9 `:.-+%/`, folded to uppercase). The faces were
  regenerated with the full charset (~8.5 KB flash for all three).
- **`textbox`** — word-wrapped text in a box with `align`/`valign`, explicit `\n`
  honoured, clipped to the box.
- **Text styles** — `"outline":[r,g,b]` (1 px ring) and `"shadow":[r,g,b]` (+1,+1) on
  the bitmap-font `text` op.
- **Sprite transforms** — `flip:"h"|"v"|"hv"`, `rot:90|180|270`, `scale:1..4` on atlas
  blits; one sheet now serves every orientation.

- **Binary ops** (`canvas.opsBin` capability, format 1) — a fixed-layout binary
  encoding of the ops surface for game-rate clients: stream record `0x06` and the
  REST twin `POST /api/canvas/opsb`. Signed 16-bit coordinates (draw off-panel
  freely), strict decode (feature-detect, don't probe). Measured: **6.3× smaller**
  frames and **1.5–1.9× the frame rate** on op-heavy scenes (a 66-op frame: 25 → 46+
  fps — JSON parse time was the ceiling), pixel-identical output to the JSON path
  (readback-verified, 0 differing bytes).
- **The black-panel wedge is now self-healing.** The GDMA output stage can halt with
  the framebuffer, API and `panel.ok` all healthy — only human eyes ever caught it
  (twice in v3.0.1, once at a v3.5 OTA boot). taskDisplay now samples the channel's
  working-descriptor register every second; frozen twice in a row = stalled, and the
  cure a reboot always applied (stop, reset the LCD FIFO, re-arm the chain, restart)
  runs in place, loudly logged. Verified end-to-end by deliberate wedge injection:
  detected and healed in ~2 s with zero false positives under effects, max-rate
  streaming and idle.

### Fixed

- Stream close drain 30 → 60 ms: the v3.4.0 soak still saw ~1 in 50 bursts lose the
  closing 200 to an RST on the PSRAM core (work always completed; only the handshake
  clipped). The 50-burst close test is the regression gate.

### Certification

2-hour adversarial soak on the 256×64 board, final release binary (full rotation with
stream bursts — half of them binary ops — full-wall pages, audio effects in the cycle,
2 SSE clients, dashboard polls): **2,251 steps, 0 reboots, 0 errors, 0 refusals,
min-heap 53.5 KB, heap drift −132 B, 38,776 SSE events / 0 drops, 281 stream bursts /
5,967 records / 0 errors, 281/281 clean closes, 2,811/2,811 polls.** Serial captured
throughout: no panics, no watchdogs, no stalls. The ~12 KB floor delta vs v3.3.0 is
the audio capture task's internal-RAM footprint while audio effects run.

## v3.4.0 — 2026-07-24 *(folded into the v3.5.0 release; never tagged separately)*

### Added — the board can hear now

- **Microphone frontend** (`src/audio.*`): the board's ES7210 dual-microphone ADC is
  brought up at boot (I2C, deliberately before any task exists — the RTC's bus access
  holds no lock) and captured over I2S on demand — 16 kHz stereo, started when an
  audio consumer appears and self-stopping 3 s after the last one leaves. Per 8 ms
  hop: DC-removed mono mix, RMS level with slow auto-gain (quiet rooms still
  visualise), a 128-point FFT folded into 16 log-spaced bands with per-band
  normalisers, and a bass beat detector. **Only derived numbers exist** — no audio
  samples are stored, exposed, or transmitted.
- **`spectrum` effect** — 16 log-band analyzer bars with falling peak caps and a
  bass→treble hue gradient (`hue` rotates the palette).
- **`soundwall` effect** — the split-flap wall itself is the visual: each beat flips
  a loudness-scaled splash of random cells to a colour flap chosen by the dominant
  frequency band (bass red → treble white), with the flips animating through the
  normal reel mechanics; ~3 s of quiet settles the wall home a few cells at a time.
  Runs in genuine wall mode, so the dashboard's flap preview follows it.
- **`"audio": true`** on `POST /api/canvas/effect` — fire breathes with loudness and
  throws sparks on beats, matrix rain falls harder and bursts on beats, plasma
  speeds up and lurches. Explicit per start, like `hue`/`density`.
- **`maze` effect** — watch a Hunt-and-Kill maze carve itself: 2 px-wide corridors
  painted as a **rainbow tracing carve order** (a rolling hue advances per cell —
  `hue` rotates the palette, each maze starts at a random point on the wheel), a
  bright 2×2 carving head with a fading fresh-carve flash, then an animated
  dead-end-filling solve that retracts every corridor until only the
  corner-to-corner solution remains, held golden before a fresh maze begins.
  `speed` spans meditative (1: one carve every 4th frame) to blazing (10: ~49
  carves/frame); the grid is (W−1)/3 × (H−1)/3 cells — 85×21 on a 256×64 panel.
- **`ripple` effect** — every beat launches an expanding ring from a random spot,
  coloured by the dominant frequency band (bass red → treble violet, `hue` rotates
  the palette) and scaled by the beat's loudness, fading as it grows; a faint centre
  glow breathes with the room level between beats. (`speed` = expansion rate.)
- **`GET /api/canvas/audio`** — live features (level, peak, beat, bands) for
  diagnostics and clients.
- **The device serves its own API contract**: `GET /openapi.yaml` (gzipped at build
  time, ~28 KB wire, ETag/304), discoverable through the standard RFC 9727
  `/.well-known/api-catalog` linkset and an `openapi` field in capabilities — a
  client always gets the exact spec for the firmware it is talking to.
- Capabilities: an `audio` feature token appears **only when the ES7210 actually
  answers**, and `effectParams` gains `"audio"`.
- **Self-describing effects** (`effectDefs` feature token): `GET /api/capabilities`
  now carries `"effectDefs"` — one object per effect declaring exactly the params it
  consumes (`key`, `type` `int`/`bool`, `min`/`max`, optional `default`, display
  `label`), so a client can build its effect UI dynamically and future effects and
  options appear with no client release. **Additive only**: the flat `"effects"`
  list and `"effectParams"` union are unchanged, byte for byte — `effectDefs` is
  the forward path, the legacy keys remain for existing clients. Single source of
  truth in the firmware: a param vocabulary + per-effect index lists next to the
  effect table, with a `static_assert` forcing every registered effect to carry a
  def; the legacy union derives from the same tables. `POST /api/canvas/effect`
  accepts each declared param by key, clamps ints into their declared range, and
  ignores unknown keys in both directions of version skew.

One hard-won note is written into the driver: after configuring the ES7210, the
REG01 clock re-enable is **load-bearing** — without it the ADC stays silent behind a
perfectly healthy-looking I2S interface.

## v3.3.0 — 2026-07-22

### Changed

- **The Arduino core is now recompiled at build time** (pioarduino `custom_sdkconfig`)
  **with WiFi/lwIP buffers allocated from PSRAM** (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`).
  This is the structural end of the deep-heap-trough class: network ingest/egress no
  longer competes with the application for internal SRAM. Measured on the 256×64 board
  (worst-alignment trough repro, clean boot each run): floors of **66.5 / 67.2 /
  68.1 KB** versus 12–27 KB on the stock core; idle internal heap +9 KB; ops latency
  and stream throughput unchanged (0.27 vs 0.28 MB/s on the identical full-frame
  workload). Two candidate levers were measured as nulls first and are not shipped:
  shrinking the WiFi RX buffer counts changed nothing, and the WiFi IRAM options are
  already disabled in the stock Arduino core.
- **Stream close handshake hardened**: the end-record 200 now drains before the
  session closes (the PSRAM core's flush timing exposed a race — clients saw an RST
  instead of the 200 about once in 15 bursts), and stream state clears before that
  drain so an immediate follow-on canvas REST call never bounces with a 409.

### Certification

2-hour adversarial soak on the 256×64 board (full rotation with stream bursts and
full-wall pages, 2 SSE clients, dashboard polls): **2,243 steps, 0 reboots, 0 errors,
0 refusals, min-heap 65.3 KB** (was 15.4 KB on v3.2.0 under the identical profile),
heap drift +132 B, 29,874 SSE events / 0 drops, 280 stream bursts / 7,560 records /
0 errors. Serial captured throughout: no panics, no watchdogs.

Note for builders: the first `pio run` after this change recompiles the entire core
(~10–20 min) and needs network access for IDF component fetches. To return to the
stock prebuilt core: remove the `custom_sdkconfig` block from `platformio.ini`, delete
`.pio/build`, `managed_components/`, `sdkconfig.*`, and rebuild.

## v3.2.0 — 2026-07-22

### Added

- **`PUT /api/canvas/stream` — a persistent TLV draw channel.** One long-lived PUT
  carries draw records back-to-back (full frame, rect deltas, ops JSON, atlas bind,
  show, end), executed as they arrive by a pump on the web task. No per-frame HTTP
  round trip and no per-record response, so the ~40 ms delayed-ACK floor on
  request/response traffic does not apply — a rect-delta animation measured **28 fps
  client-paced** over one connection. One stream at a time; the drawing REST endpoints
  answer `409` while it is open; a malformed record or a 30 s idle spell aborts the
  stream (the panel keeps its last frame). `GET /api/canvas/stream` reports channel
  state and why the previous stream ended. Advertised as `canvas.stream` in
  capabilities. **Client note:** send the first record in the same write as the
  request head — a bare body-carrying head parse-blocks stock esp_http_server's
  worker for the 8 s socket timeout (a generic behaviour of the embedded server, not
  specific to this route).
- **SSE `status` events** — `GET /api/events` now pushes the `GET /api/status` JSON
  every 5 s alongside `display` events. The dashboard's 3 s status poller stands down
  while the stream is up (and returns as a fallback when it drops): fewer connections,
  fresher numbers.

### Hardening

- **The deep-trough alignment, root-caused and fixed.** This build's prebuilt lwIP
  advertises a ~95 KB TCP receive window, so a client blasting the stream channel can
  have tens of KB of records sitting in internal-heap pbufs before the pump reads a
  byte — and SSE pushes run on the same task as the pump, so one unresponsive event
  client blocking a send for the global 8 s socket timeout let those pbufs pile up
  (worst observed watermark: 3.1 KB). Three changes: the stream pump drains **eagerly,
  always** (reading moves bytes into the PSRAM record buffer and frees internal heap —
  a heap-graded drain throttle was measured to make troughs worse and is explicitly
  rejected); SSE sockets get a **600 ms send timeout** (a client that cannot take an
  event in that window is dropped; EventSource reconnects); and SSE `display`
  broadcasts are skipped while internal heap is under 40 KB (the preview drops frames
  rather than the heap). A/B on the worst constructible alignment — stuck SSE client +
  continuous 160-module cascades + unpaced stream blasts + readback loop: watermark
  floor 12.0 KB before, **27.0 KB** after. Stream clients should still pace themselves
  modestly rather than sending unboundedly ahead.

### Performance

- **Glyph run-blitter** — flap-font glyphs (ops `text`, ticker, wall cells in pixel
  paths) draw as contiguous set-bit runs through the row blitter instead of per-pixel
  writes, keeping transparency for text over canvas content.
- **Pre-gzipped dashboard** — the page is compressed at build time (54 KB → 17 KB on
  the wire) and served with `Content-Encoding: gzip` when the browser allows it, in
  4 KB chunks; plain streaming remains the fallback. The footer version is now
  client-rendered from `GET /api/config`, which is what made the page byte-stable
  enough to pre-compress.

### Certification

2-hour adversarial soak on BOTH boards, final build (full rotation now including
stream-channel bursts and full-wall pages on every module — a substantially harder
profile than any previous certification — plus 2 persistent SSE clients and
dashboard-style polling):

- **256×64 board:** 2,255 steps, **0 reboots**, 0 errors, 0 refusals; min-heap
  15.4 KB (stochastic trough; live heap steady ~68 KB, drift −2.7 KB ≈ noise);
  29,964 SSE events / 0 drops; 281 stream bursts / 7,587 records / 0 errors;
  2,801/2,801 polls.
- **128×32 board:** 2,273 steps, **0 reboots**, 0 errors; min-heap 72.3 KB;
  29,770 SSE events / 0 drops; 284 bursts / 7,668 records / 0 errors.

The 256×64 board's 15 KB floor is a property of the torture profile (worst-case
alignments of four concurrent sockets on ~68 KB of free internal heap), not of
real-world load; the structural remedy — recompiling the core with smaller WiFi
RX buffering — is queued as v3.3.0.

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
- ~~Inbound pacing retuned to danger-only tiers~~ — reverted; see Fixed below.

### Fixed (hard-won, late in the cycle)

- **The black-panel wedge, root-caused.** `panelShow` relinks the DMA descriptor chain
  while GDMA is traversing it; the old always-sleep-after-swap pacing accidentally
  enforced the hardware's real requirement (one relink per scan pass), and the v3.1
  latency work removed it — back-to-back swaps could halt the channel: panel dark,
  framebuffer and API perfectly healthy. `panelShow` now explicitly waits out the
  remainder of the previous swap's pass. Confirmed by framebuffer readback showing the
  correct wall while the physical panel showed nothing; endurance-tested 2 h clean.
- **TCP_NODELAY reverted (from v3.0.1).** A/B on clean boots: an identical concurrent
  scenario holds a 47 K min-heap with Nagle and collapses to 700 B with NODELAY — its
  eager per-chunk segments churn pbufs under load. The ~40 ms delayed-ACK floor on
  keep-alive responses returns as the price of heap stability; ops still land ~2×
  faster than pre-3.0.1 thanks to the fill/show work.
- **`GET /api/capabilities` was truncated into invalid JSON** once the atlas descriptor
  and `rects` flag outgrew its fixed 480-byte canvas block. Now 768 B with a loud
  truncation check.
- Effects pulsing/stutter (the lazy tear-guard on the display task's render loops) —
  the display task keeps the original show pacing; HTTP paths keep the lazy guard.

### Certification

2-hour adversarial soak on the final build (full rotation incl. delta-rects, 2
persistent SSE clients, dashboard-style polls): **2,324 steps, 0 reboots, 0 alerts,
0 refusals, min-heap 33.1 KB (plateau), heap drift +1.3 KB, SSE 21,361 events / 0
drops, 2,820/2,820 polls.** Panel visually verified alive throughout.

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
