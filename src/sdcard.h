// sdcard.h -- the onboard microSD / TF card slot (v3.10). The Waveshare board wires the
// slot for the ESP32-S3 SDMMC peripheral in 1-bit mode (CLK=1, CMD=44, D0=17; the SPI CS
// pin 14 is unused in this mode) -- matching Waveshare's own BSP + Arduino example. Gives
// the gateway file storage independent of the small internal FATFS partition: browse,
// download, upload and delete via GET/PUT/DELETE /api/sd/*, advertised as the `sd`
// capability, with card size/usage on the dashboard Status page.
//
// Mounted ONCE at boot (setup(), single-threaded, like the other peripherals). Absent-card
// and mount failures are handled gracefully -- sdReady() stays false and every endpoint
// answers 503, exactly as the speaker/sensor do when their hardware is missing.

#pragma once
#include <stdint.h>

void  sdInit();                 // mount the card (setup() only); no-op-safe if absent
bool  sdReady();                // card mounted and usable
// Capacity snapshot (all MB). Returns false when no card is mounted.
bool  sdInfo(uint64_t& sizeMB, uint64_t& usedMB, const char*& type);
// Recursively delete a directory (its contents, then the directory itself). Deletes only
// the first remaining child each pass so at most one directory handle is open at a time --
// FATFS's open-file table is tiny, so a recursive walk that holds a handle per level would
// exhaust it. Returns true only if the whole tree is gone. A file path also works.
bool  sdRemoveTree(const char* path);
// Append one line to the on-card event log /logs/gateway.log (v3.13.2): timestamped,
// mutex-serialized, rotated at 512 KB, silently dropped when no card is mounted.
// For the events that explain a dead board: boot cause, watchdog reboots, GDMA
// restarts, OTA, heap heartbeat. NOT a DBG firehose.
void  sdLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
