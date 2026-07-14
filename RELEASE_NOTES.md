# Matrix Portal Gateway — Release Notes

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
