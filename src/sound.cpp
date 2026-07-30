#include "gateway.h"
#include "sound.h"
#include "audio.h"
#include "sdcard.h"
#include "SD_MMC.h"     // WAV playback from the card (v3.13)

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
// WAV request (v3.13): one pending path, replaced atomically like the note queue. The
// synth task is the ONLY writer to the I2S TX channel; WAV playback happens inside it.
static char          wavPath[96] = "";
static volatile bool wavReq = false;
static volatile bool wavActive = false;

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
bool soundPlaying()   { return gSynthRun && (wavActive || qHead < qN); }

/* ---- the synth task ----------------------------------------------------------------
   Renders 16-bit stereo sine at 16 kHz into the shared TX channel, one 128-frame
   block at a time. A 3 ms linear attack/release envelope on every note kills the
   clicks a hard sine edge makes on a small speaker. Self-stops (TX disabled, amp
   off) after ~5 s with nothing queued. */
/* ---- WAV streaming (v3.13) --------------------------------------------------------
   Strict format: RIFF/WAVE, PCM (fmt 1), 16-bit, 16 kHz (the duplex I2S clock is fixed
   -- see audio.h), mono or stereo. Streams 2 KB at a time from the card, expands mono
   to the stereo frame, scales by the request volume, and feeds the shared TX channel.
   Aborted by qStop (soundStop / Quiet Time) or by a NEWER wav request. */
static uint32_t rdU32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rdU16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }

static void wavStream(const char* path, uint8_t vol) {
  File f = SD_MMC.open(path, "r");
  if (!f) { printf("[SOUND] wav: open failed %s\n", path); return; }
  uint8_t h[12];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    printf("[SOUND] wav: not RIFF/WAVE: %s\n", path); f.close(); return;
  }
  uint16_t ch = 0, bits = 0; uint32_t rate = 0, dataLen = 0; bool haveFmt = false;
  while (f.available() >= 8) {                       // walk the chunks to fmt + data
    uint8_t ck[8];
    if (f.read(ck, 8) != 8) break;
    const uint32_t clen = rdU32(ck + 4);
    if (!memcmp(ck, "fmt ", 4)) {
      uint8_t fmt[16];
      if (clen < 16 || f.read(fmt, 16) != 16) break;
      if (rdU16(fmt) != 1) { printf("[SOUND] wav: not PCM\n"); f.close(); return; }
      ch = rdU16(fmt + 2); rate = rdU32(fmt + 4); bits = rdU16(fmt + 14);
      haveFmt = true;
      if (clen > 16) f.seek(f.position() + (clen - 16));
    } else if (!memcmp(ck, "data", 4)) {
      dataLen = clen; break;                          // file position is now at the samples
    } else f.seek(f.position() + clen + (clen & 1));
  }
  if (!haveFmt || !dataLen || bits != 16 || rate != 16000 || ch < 1 || ch > 2) {
    printf("[SOUND] wav: need 16-bit 16 kHz mono/stereo PCM (got ch=%u rate=%lu bits=%u)\n",
           ch, (unsigned long)rate, bits);
    f.close(); return;
  }
  printf("[SOUND] wav: %s %s, %lu bytes\n", path, ch == 1 ? "mono" : "stereo",
         (unsigned long)dataLen);
  static uint8_t rd[2048];
  static int16_t out[2048];                          // worst case: 1024 mono samples -> 2048
  uint32_t left = dataLen;
  wavActive = true;
  while (left && !qStop && !wavReq) {                // a NEWER wav request aborts this one
    const size_t want = left < sizeof(rd) ? left : sizeof(rd);
    const size_t got = f.read(rd, want);
    if (!got) break;
    left -= got;
    const int nsmp = (int)(got / 2);                 // 16-bit samples read
    int nframes;
    if (ch == 2) {
      nframes = nsmp / 2;
      for (int i = 0; i < nsmp; i++)
        out[i] = (int16_t)((int32_t)((int16_t)rdU16(rd + i * 2)) * vol / 100);
    } else {
      nframes = nsmp;
      for (int i = 0; i < nsmp; i++) {
        const int16_t v = (int16_t)((int32_t)((int16_t)rdU16(rd + i * 2)) * vol / 100);
        out[i * 2] = v; out[i * 2 + 1] = v;
      }
    }
    size_t wr = 0;
    i2s_channel_write(audioTxChan(), out, (size_t)nframes * 4, &wr, pdMS_TO_TICKS(400));
  }
  wavActive = false;
  f.close();
}

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
      qHead = 0; qN = 0; qStop = false;   // no chained assignment through a volatile
      taskEXIT_CRITICAL(&sndMux);
      continue;
    }
    if (wavReq) {                                    // WAV playback request (v3.13)
      char p[sizeof(wavPath)]; uint8_t v;
      taskENTER_CRITICAL(&sndMux);
      memcpy(p, wavPath, sizeof(p)); v = qVol; wavReq = false;
      taskEXIT_CRITICAL(&sndMux);
      wavStream(p, v);
      idleSince = 0;
      continue;
    }
    if (head >= n) {                                   // nothing to play
      if (!idleSince) idleSince = millis();
      if (millis() - idleSince > 5000) {
        // Shut down -- but a soundPlay() may land between the idle decision and the flag
        // clear (it would enqueue, see gSynthRun still true, spawn nothing, and the notes
        // would silently rot). Tear down FIRST, then atomically re-check the queue: new
        // notes mean re-arm and keep going; an empty queue means clear the flag and exit,
        // after which any soundPlay spawns a fresh task. (v3.11.1)
        gpio_set_level(PA_ENABLE_PIN, 0);
        i2s_channel_disable(audioTxChan());
        bool exitNow;
        taskENTER_CRITICAL(&sndMux);
        exitNow = (qHead >= qN) && !wavReq;
        if (exitNow) gSynthRun = false;
        taskEXIT_CRITICAL(&sndMux);
        if (exitNow) break;
        i2s_channel_enable(audioTxChan());             // late arrival: spin back up
        gpio_set_level(PA_ENABLE_PIN, 1);
        idleSince = 0;
        continue;
      }
      memset(blk, 0, sizeof(blk));                     // keep the DAC fed with silence
      size_t wr;
      i2s_channel_write(audioTxChan(), blk, sizeof(blk), &wr, pdMS_TO_TICKS(100));
      continue;
    }
    idleSince = 0;

    uint16_t f; uint32_t ms;                           // read the note under the lock:
    taskENTER_CRITICAL(&sndMux);                       // soundPlay may replace the queue
    f = qFreq[head]; ms = qMs[head];                   // concurrently (torn-read guard)
    taskEXIT_CRITICAL(&sndMux);
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

  // Amp/TX/gSynthRun were already torn down inside the idle-exit branch above.
  printf("[SOUND] synth idle -- amp off (writes ok=%lu fail=%lu)\n",
         (unsigned long)wrOk, (unsigned long)wrFail);
  vTaskDelete(NULL);
}

bool soundPlay(const uint16_t* freq, const uint16_t* ms, int n, uint8_t vol) {
  if (!gSoundPresent || n < 1) return false;
  if (gQuietTime) return false;           // Quiet Time silences the speaker (v3.6)
  if (n > SOUND_MAX_NOTES) n = SOUND_MAX_NOTES;
  if (!audioAcquireI2S()) return false;
  taskENTER_CRITICAL(&sndMux);
  for (int i = 0; i < n; i++) { qFreq[i] = freq[i]; qMs[i] = ms[i]; }
  qN = n; qHead = 0; qVol = vol > 100 ? 100 : vol; qStop = false;
  taskEXIT_CRITICAL(&sndMux);
  if (!gSynthRun) {
    gSynthRun = true;
    // 4 KB: the same task also streams WAVs (wavStream's File + chunk walk) -- v3.13
    if (xTaskCreatePinnedToCore(synthTask, "synth", 4096, NULL, 2, NULL, 0) != pdPASS) {
      gSynthRun = false;                // creation failed: don't wedge sound until reboot
      return false;
    }
  }
  return true;
}

void soundStop() { if (gSynthRun) qStop = true; }

// Queue a WAV from the SD card (v3.13). Returns false when the speaker is absent, quiet
// time is active, or the task cannot start. Format errors surface in the log -- the
// file is parsed on the synth task, not here.
bool soundPlayWav(const char* path, uint8_t vol) {
  if (!gSoundPresent || !path || !path[0]) return false;
  if (gQuietTime) return false;
  if (!audioAcquireI2S()) return false;
  taskENTER_CRITICAL(&sndMux);
  qHead = 0; qN = 0;                       // a wav replaces any queued tones
  strlcpy(wavPath, path, sizeof(wavPath));
  qVol = vol > 100 ? 100 : vol;
  wavReq = true;
  taskEXIT_CRITICAL(&sndMux);
  if (!gSynthRun) {
    gSynthRun = true;
    if (xTaskCreatePinnedToCore(synthTask, "synth", 4096, NULL, 2, NULL, 0) != pdPASS) {
      gSynthRun = false;
      return false;
    }
  }
  return true;
}
