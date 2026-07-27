#include "gateway.h"
#include "sound.h"
#include "audio.h"

#include <Wire.h>
#include <driver/gpio.h>
#include <math.h>

// sound.cpp -- see sound.h. ES8311 register sequence ported from the esphome es8311
// driver (MIT); the 16 kHz / MCLK 4.096 MHz coefficient row from its table:
// pre_div 1, pre_mult 1, adc_div 1, dac_div 1, fs_mode 0, lrck 0x00ff, bclk_div 4,
// adc_osr 0x10, dac_osr 0x20.

#define ES8311_ADDR   0x18
#define PA_ENABLE_PIN GPIO_NUM_11
#define SND_RATE      16000

static bool gSoundPresent = false;
static volatile bool gSynthRun = false;

// The note queue: one pending sequence, replaced atomically by soundPlay.
static portMUX_TYPE sndMux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t qFreq[SOUND_MAX_NOTES], qMs[SOUND_MAX_NOTES];
static volatile int  qN = 0, qHead = 0;
static volatile uint8_t qVol = 60;
static volatile bool qStop = false;

static bool esw(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}
static bool esr(uint8_t reg, uint8_t& val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return false;
  val = (uint8_t)Wire.read();
  return true;
}

static bool es8311Configure() {
  bool ok = true;
  uint8_t v;
  ok &= esw(0x00, 0x1F);                  // reset
  ok &= esw(0x00, 0x00);
  ok &= esw(0x01, 0x3F);                  // MCLK from pad, all clocks on
  ok &= esr(0x02, v) && esw(0x02, (uint8_t)((v & 0x07) | ((1 - 1) << 5) | (0x01 << 3)));
  ok &= esw(0x03, (uint8_t)((0x00 << 6) | 0x10));   // fs_mode | adc_osr
  ok &= esw(0x04, 0x20);                  // dac_osr
  ok &= esw(0x05, (uint8_t)(((1 - 1) << 4) | (1 - 1)));   // adc_div | dac_div
  ok &= esr(0x06, v) && esw(0x06, (uint8_t)((v & 0xE0) | (4 - 1)));   // bclk_div 4
  ok &= esr(0x07, v) && esw(0x07, (uint8_t)((v & 0xC0) | 0x00));      // lrck_h
  ok &= esw(0x08, 0xFF);                  // lrck_l
  ok &= esr(0x00, v) && esw(0x00, (uint8_t)(v & 0xBF));   // slave mode
  ok &= esw(0x09, (uint8_t)(3 << 2));     // SDP in: 16-bit (the DAC's feed)
  ok &= esw(0x0A, (uint8_t)(3 << 2));     // SDP out: 16-bit (ADC unused, set anyway)
  ok &= esw(0x0D, 0x01);                  // power up analog
  ok &= esw(0x0E, 0x02);                  // PGA/ADC modulator power
  ok &= esw(0x12, 0x00);                  // DAC power up
  ok &= esw(0x13, 0x10);                  // enable output to HP drive
  ok &= esw(0x1C, 0x6A);                  // ADC eq bypass, DC offset cancel
  ok &= esw(0x14, 0x1A);                  // analog input stage (vendor writes it in DAC mode too)
  ok &= esw(0x15, 0x40);                  // vendor start sequence parity
  ok &= esw(0x17, 0xBF);
  ok &= esw(0x45, 0x00);                  // GP control -- vendor clears it explicitly
  ok &= esw(0x31, 0x00);                  // explicit DAC unmute (reset default trusted nowhere)
  ok &= esw(0x37, 0x08);                  // DAC eq bypass
  ok &= esw(0x32, 0xBF);                  // DAC volume 0 dB (loudness is scaled in samples)
  ok &= esw(0x00, 0x80);                  // power on
  return ok;
}

void soundInit() {
  gpio_set_direction(PA_ENABLE_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(PA_ENABLE_PIN, 0);       // amp off until something plays
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission(true) != 0) {
    printf("[SOUND] no ES8311 at 0x18 -- speaker disabled\n");
    return;
  }
  gSoundPresent = es8311Configure();
  printf("[SOUND] ES8311 at 0x18: %s (16 kHz DAC + PA on GPIO %d)\n",
         gSoundPresent ? "configured" : "CONFIG FAILED", (int)PA_ENABLE_PIN);
  if (gSoundPresent) {                    // boot-time register dump: the debug evidence
    printf("[SOUND] regs:");
    for (uint8_t reg = 0; reg <= 0x17; reg++) {
      uint8_t v = 0xEE; esr(reg, v);
      printf(" %02x=%02x", reg, v);
    }
    uint8_t v31 = 0, v32 = 0, v37 = 0, v45 = 0;
    esr(0x31, v31); esr(0x32, v32); esr(0x37, v37); esr(0x45, v45);
    printf(" 31=%02x 32=%02x 37=%02x 45=%02x\n", v31, v32, v37, v45);
  }
}

bool soundAvailable() { return gSoundPresent; }
bool soundPlaying()   { return gSynthRun && qHead < qN; }

/* ---- the synth task ----------------------------------------------------------------
   Renders 16-bit stereo sine at 16 kHz into the shared TX channel, one 128-frame
   block at a time. A 3 ms linear attack/release envelope on every note kills the
   clicks a hard sine edge makes on a small speaker. Self-stops (TX disabled, amp
   off) after ~5 s with nothing queued. */
static void synthTask(void* pv) {
  static int16_t blk[128 * 2];
  float phase = 0;
  unsigned long idleSince = 0;
  const esp_err_t en = i2s_channel_enable(audioTxChan());
  printf("[SOUND] synth start: tx enable=%d chan=%p\n", (int)en, (void*)audioTxChan());
  gpio_set_level(PA_ENABLE_PIN, 1);
  uint32_t wrOk = 0, wrFail = 0;

  while (true) {
    int   head, n;
    taskENTER_CRITICAL(&sndMux);
    head = qHead; n = qN;
    taskEXIT_CRITICAL(&sndMux);

    if (qStop) {
      taskENTER_CRITICAL(&sndMux);
      qHead = qN = 0; qStop = false;
      taskEXIT_CRITICAL(&sndMux);
      continue;
    }
    if (head >= n) {                                   // nothing to play
      if (!idleSince) idleSince = millis();
      if (millis() - idleSince > 5000) break;
      memset(blk, 0, sizeof(blk));                     // keep the DAC fed with silence
      size_t wr;
      i2s_channel_write(audioTxChan(), blk, sizeof(blk), &wr, pdMS_TO_TICKS(100));
      continue;
    }
    idleSince = 0;

    const uint16_t f  = qFreq[head];
    const uint32_t ms = qMs[head];
    const float amp   = 8000.0f * qVol / 100.0f;       // headroom under int16
    const uint32_t total = (uint32_t)SND_RATE * ms / 1000;
    const uint32_t ramp  = SND_RATE * 3 / 1000;        // 3 ms envelope
    const float step  = 2.0f * (float)M_PI * f / SND_RATE;
    uint32_t done = 0;
    while (done < total && !qStop) {
      const int frames = (int)((total - done) < 128 ? (total - done) : 128);
      for (int i = 0; i < frames; i++) {
        float env = 1.0f;
        const uint32_t k = done + i;
        if (k < ramp)              env = (float)k / ramp;
        else if (total - k < ramp) env = (float)(total - k) / ramp;
        const int16_t sm = f ? (int16_t)(sinf(phase) * amp * env) : 0;
        phase += step;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        blk[i * 2] = sm; blk[i * 2 + 1] = sm;
      }
      size_t wr = 0;
      const esp_err_t we = i2s_channel_write(audioTxChan(), blk, (size_t)frames * 4, &wr,
                                             pdMS_TO_TICKS(200));
      if (we == ESP_OK && wr == (size_t)frames * 4) wrOk++;
      else {
        if (!wrFail) printf("[SOUND] first write fail: err=0x%x wrote=%u/%u\n",
                            (unsigned)we, (unsigned)wr, (unsigned)(frames * 4));
        wrFail++;
      }
      done += frames;
    }
    taskENTER_CRITICAL(&sndMux);
    if (qHead == head) qHead = head + 1;               // soundPlay may have replaced the queue
    taskEXIT_CRITICAL(&sndMux);
  }

  gpio_set_level(PA_ENABLE_PIN, 0);
  i2s_channel_disable(audioTxChan());
  gSynthRun = false;
  printf("[SOUND] synth idle -- amp off (writes ok=%lu fail=%lu)\n",
         (unsigned long)wrOk, (unsigned long)wrFail);
  vTaskDelete(NULL);
}

bool soundPlay(const uint16_t* freq, const uint16_t* ms, int n, uint8_t vol) {
  if (!gSoundPresent || n < 1) return false;
  if (n > SOUND_MAX_NOTES) n = SOUND_MAX_NOTES;
  if (!audioAcquireI2S()) return false;
  taskENTER_CRITICAL(&sndMux);
  for (int i = 0; i < n; i++) { qFreq[i] = freq[i]; qMs[i] = ms[i]; }
  qN = n; qHead = 0; qVol = vol > 100 ? 100 : vol; qStop = false;
  taskEXIT_CRITICAL(&sndMux);
  if (!gSynthRun) {
    gSynthRun = true;
    xTaskCreatePinnedToCore(synthTask, "synth", 3072, NULL, 2, NULL, 0);
  }
  return true;
}

void soundStop() { if (gSynthRun) qStop = true; }
