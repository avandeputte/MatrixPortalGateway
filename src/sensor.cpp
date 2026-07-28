#include "gateway.h"
#include "sensor.h"
#include <Wire.h>

// sensor.cpp -- SHTC3 (Sensirion) driver, see sensor.h. Commands are 16-bit big-endian;
// each returned 16-bit word carries a CRC-8 (poly 0x31, init 0xFF) which we verify and
// reject on mismatch, keeping the last good reading. Measurement: wake, trigger
// (normal mode, clock stretch OFF, temperature first), wait ~13 ms, read 6 bytes, sleep.

#define SHTC3_ADDR   0x70
#define CMD_WAKE     0x3517
#define CMD_SLEEP    0xB098
#define CMD_MEAS_TF  0x7866    // normal mode, no clock stretch, T first
#define CMD_ID       0xEFC8

static bool     gPresent = false;
static volatile int16_t  gTC100 = 0, gRH100 = 0;   // centi-units; 16-bit writes are atomic
static volatile uint32_t gStampMs = 0;
static volatile bool     gValid = false;

static bool shtcCmd(uint16_t c) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write((uint8_t)(c >> 8));
  Wire.write((uint8_t)(c & 0xFF));
  return Wire.endTransmission(true) == 0;
}

static uint8_t crc8(uint8_t msb, uint8_t lsb) {
  uint8_t crc = 0xFF;
  const uint8_t data[2] = { msb, lsb };
  for (int i = 0; i < 2; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool sensorAvailable() { return gPresent; }

void sensorInit() {
  if (!shtcCmd(CMD_WAKE)) {
    printf("[ENV] no SHTC3 at 0x70 -- environment sensor disabled\n");
    return;
  }
  delayMicroseconds(300);
  if (!shtcCmd(CMD_ID)) { shtcCmd(CMD_SLEEP); printf("[ENV] SHTC3 ID cmd failed\n"); return; }
  if (Wire.requestFrom((uint8_t)SHTC3_ADDR, (uint8_t)3) != 3) {
    shtcCmd(CMD_SLEEP); printf("[ENV] SHTC3 ID read failed\n"); return;
  }
  const uint8_t idm = (uint8_t)Wire.read(), idl = (uint8_t)Wire.read();
  Wire.read();   // CRC of the ID (not checked)
  const uint16_t id = ((uint16_t)idm << 8) | idl;
  shtcCmd(CMD_SLEEP);
  // SHTC3 product code: bits per the datasheet -- (id & 0x083F) == 0x0807.
  gPresent = ((id & 0x083F) == 0x0807);
  printf("[ENV] SHTC3 at 0x70: %s (id 0x%04x, temp + humidity)\n",
         gPresent ? "found" : "UNRECOGNISED", id);
}

void sensorPoll() {
  if (!gPresent) return;
  if (!shtcCmd(CMD_WAKE)) return;
  delayMicroseconds(300);
  if (!shtcCmd(CMD_MEAS_TF)) { shtcCmd(CMD_SLEEP); return; }
  delay(13);                                          // normal-mode conversion time
  if (Wire.requestFrom((uint8_t)SHTC3_ADDR, (uint8_t)6) != 6) { shtcCmd(CMD_SLEEP); return; }
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = (uint8_t)Wire.read();
  shtcCmd(CMD_SLEEP);
  if (crc8(b[0], b[1]) != b[2] || crc8(b[3], b[4]) != b[5]) return;   // keep last good
  const uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
  const uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
  const float tC = -45.0f + 175.0f * rawT / 65535.0f;
  const float rh = 100.0f * rawH / 65535.0f;
  gTC100  = (int16_t)(tC * 100.0f);
  gRH100  = (int16_t)((rh < 0 ? 0.0f : rh > 100 ? 100.0f : rh) * 100.0f);
  gStampMs = millis();
  gValid = true;
}

bool sensorRead(float& tempC, float& rh, uint32_t& ageMs) {
  if (!gValid) return false;
  tempC = gTC100 / 100.0f;
  rh    = gRH100 / 100.0f;
  ageMs = millis() - gStampMs;
  return true;
}
