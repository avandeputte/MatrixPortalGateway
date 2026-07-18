// modules.h -- the module protocol: character, index, home, text, quiet time.
//
// There is no module registry. The wall is drawn, not discovered: vmods[] (vmodule.h) is
// created whole from rows x cols and IS the state of every module -- nothing on this board
// can appear, vanish or fail to answer. See the note at the top of modules.cpp.

#ifndef SFGW_MODULES_H
#define SFGW_MODULES_H

#include "common.h"

// FATFS mounted? Set by sfFsInit(). The filesystem now holds only the
// companion's settings blob (/compset.gz).
extern bool sfFsReady;

void sfFsInit(bool forceFormat = false);

void sfSendChar(int addr, char c);       // legacy path: one CP1252 byte, folds lowercase
void sfSendIndex(int addr, int idx);     // by flap index: exact, reaches every flap
void sfHome(int addr);                   // flap 0 -- blank
void sfSendText(int startAddr, const char* text);
void sfSetQuietTime(bool on);

#endif // SFGW_MODULES_H
