// backup.h -- FATFS asset backup to the microSD card, and self-restore (v3.16).
//
// The panic-recovery reformat (main.cpp) is the right call for a corrupt FATFS,
// but it costs every uploaded asset: animations, fonts, atlases, the companion
// settings blob, the virtual modules' state. This module turns that from data
// loss into a log line: the FATFS tree is mirrored to /backup/fatfs on the card
// (plus the full settings export as /backup/config.json), and a boot whose FATFS
// was just (re)formatted restores the mirror before anything reads assets.
//
// The mirror runs on the loop() task -- the one task with FatFs stack headroom
// and no latency budget. Triggers: POST /api/backup (flag, picked up within 1 s),
// a nightly pass at 03:30 local when cfg.backupEnabled, and one automatic first
// pass ~2 min after boot when the card has no mirror yet. Incremental: a file is
// recopied when its size differs or the source is newer; files that vanished
// from FATFS are pruned from the mirror so a restore cannot resurrect them.

#pragma once
#include <stdint.h>

// One status snapshot for GET /api/backup and the dashboard card.
struct BackupStatus {
  bool     running;        // a mirror pass is executing right now
  bool     everRan;        // at least one pass finished since boot
  bool     lastOk;         // last pass completed without errors
  uint32_t lastEpoch;      // wall-clock time the last pass finished (0 = clock unset)
  uint32_t lastMs;         // duration of the last pass
  uint32_t copied, pruned, skipped;  // files copied / removed from mirror / unchanged
  uint64_t bytes;          // bytes written by the last pass
};

void backupRequest();                 // ask loop() to run a pass now (web-safe)
void backupTick();                    // loop() only: scheduler + executor
const BackupStatus& backupStatus();
// setup() only, after sfFsInit()+sdInit(): if FATFS was (re)formatted this boot
// and the card holds a mirror, copy it back. Returns files restored (0 = nothing).
int  backupRestoreIfNeeded(bool fatfsWasFormatted);
