#include "gateway.h"
#include "imu.h"
#include <Wire.h>

// imu.cpp -- see imu.h. Register map and the CTRL9 command/CAL-page protocol follow
// the QMI8658A datasheet, cross-checked against lewisxhe/SensorLib (MIT).

#define QMI_ADDR_A        0x6B      // SA0 high (try first)
#define QMI_ADDR_B        0x6A
#define REG_WHOAMI        0x00      // == 0x05
#define REG_CTRL1         0x02      // bit6 = register auto-increment
#define REG_CTRL2         0x03      // accel: range<<4 | odr
#define REG_CTRL7         0x08      // bit0 = accel enable
#define REG_CTRL8         0x09      // bit0 = tap engine enable
#define REG_CTRL9         0x0A      // command register
#define REG_CAL1_L        0x0B      // .. CAL4_H: command parameters
#define REG_CAL1_H        0x0C
#define REG_CAL2_L        0x0D
#define REG_CAL2_H        0x0E
#define REG_CAL3_L        0x0F
#define REG_CAL3_H        0x10
#define REG_CAL4_H        0x12
#define REG_STATUS_INT    0x2D      // bit7 = CTRL9 command done
#define REG_STATUS1       0x2F      // bit1 = tap event (read clears)
#define REG_TAP_STATUS    0x59      // bits1:0 -- 1 single, 2 double
#define REG_RESET         0x60      // write 0xB0
#define REG_RST_RESULT    0x4D      // == 0x80 when the reset finished
#define CMD_CONFIGURE_TAP 0x0C
#define CMD_ACK           0x00

static uint8_t  qmiAddr   = 0;
static bool     gImuReady = false;
// Published tap event (written on taskRTC, read from taskWeb) -- same pattern as claps.
static portMUX_TYPE imuMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  gTapPending = 0;
static volatile uint32_t gTapSeq = 0;
static volatile uint32_t gTapTotal = 0;       // events since boot (diagnostics)

static bool qw(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(qmiAddr);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission(true) == 0;
}
static int qr(uint8_t reg) {
  Wire.beginTransmission(qmiAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return -1;
  if (Wire.requestFrom(qmiAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

// CTRL9 command handshake: write the command, wait for STATUS_INT.bit7, ACK, wait clear.
static bool qcmd(uint8_t cmd) {
  if (!qw(REG_CTRL9, cmd)) return false;
  for (int i = 0; i < 100; i++) { int v = qr(REG_STATUS_INT); if (v >= 0 && (v & 0x80)) break; delay(1); }
  if (!qw(REG_CTRL9, CMD_ACK)) return false;
  for (int i = 0; i < 100; i++) { int v = qr(REG_STATUS_INT); if (v >= 0 && !(v & 0x80)) return true; delay(1); }
  return false;
}

void imuInit() {
  for (uint8_t a : { (uint8_t)QMI_ADDR_A, (uint8_t)QMI_ADDR_B }) {
    qmiAddr = a;
    if (qr(REG_WHOAMI) == 0x05) break;
    qmiAddr = 0;
  }
  if (!qmiAddr) { printf("[IMU] no QMI8658 at 0x6B/0x6A -- tap detection disabled\n"); return; }

  qw(REG_RESET, 0xB0);                            // full reset; result lands in 0x4D
  bool resetOk = false;
  for (int i = 0; i < 50 && !resetOk; i++) { delay(10); resetOk = (qr(REG_RST_RESULT) == 0x80); }
  if (!resetOk) { printf("[IMU] QMI8658 reset did not complete -- tap detection disabled\n"); return; }
  qw(REG_CTRL1, 0x40);                            // register auto-increment (the lib's baseline)

  // Tap engine parameters (datasheet defaults, tuned for an enclosure knock; the
  // engine's own windows are in ACCEL SAMPLES at the CTRL2 ODR -- 500 Hz here).
  // Phase 1 (CAL4_H=0x01): priority X>Y>Z, PeakWindow 20, TapWindow 50, DTapWindow 250.
  bool ok = true;
  ok &= qw(REG_CAL1_L, 20);        // PeakWindow
  ok &= qw(REG_CAL1_H, 0);         // priority: X > Y > Z
  ok &= qw(REG_CAL2_L, 50);  ok &= qw(REG_CAL2_H, 0);      // TapWindow  = 50
  ok &= qw(REG_CAL3_L, 250); ok &= qw(REG_CAL3_H, 0);      // DTapWindow = 250
  ok &= qw(REG_CAL4_H, 0x01);
  ok &= qcmd(CMD_CONFIGURE_TAP);
  // Phase 2 (CAL4_H=0x02): alpha 0.0625, gamma 0.25 (u1.7 fixed), PeakMagThr 0.8 g²,
  // UDMThr 0.4 g² (u16 in units of 0.001·g², g=9.81 -- the lib's exact encoding).
  const double g2res = 0.001 * 9.81 * 9.81;
  // 1.5/0.6 g^2 (was 0.8/0.4): desk vibration -- typing, footsteps -- crossed the
  // looser thresholds; a deliberate knuckle-tap on the enclosure clears these easily.
  const uint16_t peakThr = (uint16_t)(1.5 * 9.81 * 9.81 / g2res);
  const uint16_t udmThr  = (uint16_t)(0.6 * 9.81 * 9.81 / g2res);
  ok &= qw(REG_CAL1_L, (uint8_t)(0.0625 * 128));  // alpha
  ok &= qw(REG_CAL1_H, (uint8_t)(0.25 * 128));    // gamma
  ok &= qw(REG_CAL2_L, (uint8_t)(peakThr & 0xFF)); ok &= qw(REG_CAL2_H, (uint8_t)(peakThr >> 8));
  ok &= qw(REG_CAL3_L, (uint8_t)(udmThr & 0xFF));  ok &= qw(REG_CAL3_H, (uint8_t)(udmThr >> 8));
  ok &= qw(REG_CAL4_H, 0x02);
  ok &= qcmd(CMD_CONFIGURE_TAP);

  ok &= qw(REG_CTRL2, (1 << 4) | 4);              // accel 4 g, 500 Hz (tap tables assume 500)
  ok &= qw(REG_CTRL7, 0x01);                      // accel on (non-SyncSample: CTRL7.5 stays 0)
  ok &= qw(REG_CTRL8, 0x01);                      // tap engine on
  gImuReady = ok;
  printf("[IMU] QMI8658 at 0x%02X: %s (tap engine, accel 4g@500Hz)\n",
         qmiAddr, ok ? "armed" : "CONFIG FAILED");
}

bool imuAvailable() { return gImuReady; }

// taskRTC, ~100 ms: reading STATUS1 clears the event; TAP_STATUS then says which kind.
void imuTapTick() {
  if (!gImuReady) return;
  const int s1 = qr(REG_STATUS1);
  if (s1 < 0 || !(s1 & 0x02)) return;             // bit1 = tap happened since last read
  const int ts = qr(REG_TAP_STATUS);
  const uint8_t kind = (ts >= 0) ? (uint8_t)(ts & 0x03) : 0;   // 1 single, 2 double
  if (kind != 1 && kind != 2) return;
  taskENTER_CRITICAL(&imuMux);
  gTapPending = kind;                             // count: 1 = single, 2 = double
  gTapSeq = gTapSeq + 1;
  gTapTotal = gTapTotal + 1;
  taskEXIT_CRITICAL(&imuMux);
}

uint32_t imuTapTotal() { return gTapTotal; }

bool imuTapPoll(uint8_t* countOut, uint32_t* seqOut) {
  bool has = false;
  taskENTER_CRITICAL(&imuMux);
  if (gTapPending) {
    if (countOut) *countOut = gTapPending;
    if (seqOut)   *seqOut   = gTapSeq;
    gTapPending = 0;
    has = true;
  }
  taskEXIT_CRITICAL(&imuMux);
  return has;
}
