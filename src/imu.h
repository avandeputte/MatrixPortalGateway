// imu.h -- QMI8658 IMU: hardware TAP detection (v3.15). The chip's own tap engine
// (accel @500 Hz, on-die peak detection) recognises single and double taps on the
// board/enclosure; the firmware polls its status registers and publishes events on the
// SSE stream as "tap" {count, seq} -- advertised as the `taps` capability token.
//
// I2C discipline (the bus has NO lock, see sensor.h): imuInit() configures the chip at
// boot while the system is single-threaded; imuTapTick() -- the ONLY runtime I2C here
// -- runs on taskRTC (~100 ms cadence). imuTapPoll() is I2C-free (it reads a published
// flag) and safe from any task.

#pragma once
#include <stdint.h>

void imuInit();                                  // probe + configure (setup() only)
bool imuAvailable();                             // QMI8658 found and tap engine armed
void imuTapTick();                               // poll the tap status (taskRTC only)
bool imuTapPoll(uint8_t* countOut, uint32_t* seqOut);   // one event per tap, any task
uint32_t imuTapTotal();                          // tap events since boot (diagnostics)
int32_t  imuAccelPeakMg();                       // peak |a|-1g deviation since last read, mg
