// tasks.h -- the FreeRTOS task entry points (spawned in setup()).

#ifndef MPGW_TASKS_H
#define MPGW_TASKS_H

#include "common.h"

void taskBus(void* pv);       // drains the emulated bus (was: the UART)
void taskRTC(void* pv);
void taskWeb(void* pv);
void taskNetwork(void* pv);
void taskDisplay(void* pv);   // reel animation + HUB75 repaint (display.cpp)

// True if the quiet schedule is enabled AND the current user-local time is inside
// its window. Shared by the schedule tick, the /api/quiet/schedule readout, and
// the guards that let the schedule win over an external quiet-OFF (MQTT/manual).
bool quietSchedInWindow();

#endif // MPGW_TASKS_H
