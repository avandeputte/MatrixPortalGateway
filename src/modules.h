// modules.h -- split-flap module registry, structs, and protocol API.

#ifndef SFGW_MODULES_H
#define SFGW_MODULES_H

#include "common.h"

/* ----------------------------------------------------------
   Module registry  (tracks known modules on the bus)
   See MAX_MODULES in common.h.
---------------------------------------------------------- */
struct SFModule {
  uint8_t  id;               // 0-254; 255 = slot empty
  char     serialNum[21];    // hex serial from advertisement (0-terminated)
  bool     provisioned;      // false = advertising (id==255 from adv)
  int      flapIndex;        // last known flap index (-1 = unknown)
  char     flapChar;         // last known displayed char (0 = unknown)
  char     fwVersion[8];     // firmware version string
  unsigned long lastSeen;    // millis() of last activity (resets on reboot)
  unsigned long lastSeenEpoch; // RTC wall-clock epoch of last activity (survives reboot)
  // Quiet Time: the display the host last requested while quiet (not yet shown).
  // pendChar holds a requested character, or pendIndex a requested flap index;
  // hasPend marks one is waiting. On Quiet Time -> off these drive the resync.
  char     pendChar;         // requested char while quiet (0 = none / index-based)
  int      pendIndex;        // requested flap index while quiet (-1 = none)
  bool     hasPend;          // a deferred display request is waiting
};

// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Sticky module persistence (FATFS file "/modules.dat")
// Persists known modules across reboots; prunes entries older than
// MODULE_STALE_SECS based on RTC wall-clock epoch. Only durable fields
// are stored (id, serial, provisioned, fwVersion, lastSeenEpoch) -- the
// transient display state is NOT persisted.
//
// Stored in the FATFS partition (already present in the default
// "16M Flash (3MB APP/9.9MB FATFS)" scheme) -- no custom partition needed.
// File format: a 4-byte magic+count header followed by N PersistedModule
// records written as raw bytes. (MODULES_FILE / MODULES_MAGIC are defined in
// common.h.)
// ------------------------------------------------------------------
struct PersistedModule {
  uint8_t       id;
  char          serialNum[21];
  bool          provisioned;
  char          fwVersion[8];
  unsigned long lastSeenEpoch;
};

struct ModulesFileHeader {
  unsigned long magic;   // MODULES_MAGIC
  int           count;   // number of PersistedModule records following
};


// ---- owned globals (defined in globals.cpp) ----
extern SFModule* sfModules;
extern SemaphoreHandle_t sfMutex;
extern StaticSemaphore_t sfMutexBuf;
extern int sfModuleCount;
extern volatile bool sfModulesDirty;
extern volatile unsigned long sfModulesDirtyMs;
extern bool sfFsReady;

SFModule* sfFindById(uint8_t id);
void sfFsInit();
void sfModulesSave();
void sfModulesLoad();
void sfModulesClear();
void sfTrackChar(int addr, char c);
void sfSendChar(int addr, char c);
void sfSendIndex(int addr, int idx);
void sfHome(int addr);
void sfQueryVersion(int addr);
void sfSendText(int startAddr, const char* text, bool blankUnused);
void sfSetQuietTime(bool on);
void sfParseResponse(const uint8_t* data, size_t len);

#endif // SFGW_MODULES_H
