// audio.h -- the microphone frontend (v3.4): ES7210 dual-mic ADC -> I2S -> DSP.
//
// The Waveshare board carries a dual MEMS microphone array behind an ES7210 4-channel
// ADC (I2C control on the shared 47/48 bus, audio on I2S: MCLK=12 BCLK=43 WS=38 DIN=39
// -- the ES8311/speaker side of the same bus is unused). This module feeds the
// audio-reactive effects (spectrum, soundwall, and the "audio":true modulation of
// fire/matrix/plasma) with a small per-frame feature block; raw samples never leave
// the device and are never stored.
//
// Split lifecycle, deliberately:
//   * audioInit() -- called ONCE from setup(), BEFORE tasks exist: probes the ES7210
//     and writes its ~30 config registers. rtc.cpp's PCF85063 access uses raw Wire
//     with no bus lock, so all of audio's I2C happens while the system is still
//     single-threaded; after boot this module never touches I2C again.
//   * capture starts on demand (an audio effect starting) and self-stops a few
//     seconds after the last consumer leaves -- the I2S channel and its DMA buffers
//     exist only while something is actually listening.
//
// The DSP (audioTask, core 0): 16 kHz 16-bit stereo, 128-sample hops. Per hop:
// DC-removed mono mix, RMS level with slow auto-gain, a 128-point windowed FFT folded
// into AUDIO_BANDS log-spaced bands (each with its own slow normaliser), and a bass
// beat detector (energy vs its ~1 s average, 150 ms refractory).

#pragma once
#include <stdint.h>

#define AUDIO_BANDS 16

struct AudioFrame {
  float    level;               // 0..1 overall loudness (auto-gained RMS)
  float    peak;                // 0..1 peak with ~1 s decay
  bool     beat;                // true on the hop a beat fired (latched until read)
  float    bands[AUDIO_BANDS];  // 0..1 per log-spaced band, per-band normalised
  float    bassRaw;             // un-normalised bass energy (beat/debug)
  uint32_t seq;                 // increments per DSP hop; stale if it stops moving
};

void audioInit();                    // probe + configure the ES7210 (setup() only)
bool audioAvailable();               // ES7210 found and configured
void audioMaybeStart();              // a consumer exists: ensure capture is running
void audioRead(AudioFrame& out);     // latest features (zeroed when not capturing)
bool audioCapturing();               // I2S currently running (diagnostics)
