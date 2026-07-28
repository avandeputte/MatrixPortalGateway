// sensor.h -- the onboard environment sensor (v3.7): Sensirion SHTC3 temperature +
// humidity, on the shared I2C bus (0x70). Gives the gateway a free weather station:
// the reading appears in GET /api/status and GET /api/environment, is advertised as
// the `environment` capability token, and shows on the dashboard Status page.
//
// I2C on this board is NOT lock-protected (rtc.cpp uses raw Wire), so ALL runtime I2C
// stays on one task: sensorInit() probes at boot (single-threaded, like the audio
// codecs), and sensorPoll() -- the only runtime I2C here -- is called from taskRTC,
// the same task that already does rtcRead(). Readers take the cached values, never
// the bus.

#pragma once
#include <stdint.h>

void  sensorInit();                 // probe the SHTC3 (setup() only)
bool  sensorAvailable();            // found at boot
void  sensorPoll();                 // one measurement -> cache (taskRTC only)
// Latest cached reading. Returns false until the first successful poll. ageMs is how
// long ago it was taken.
bool  sensorRead(float& tempC, float& rh, uint32_t& ageMs);
