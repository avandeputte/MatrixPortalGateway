// sound.h -- the speaker path (v3.6): ES8311 DAC + the onboard power amp.
//
// The board carries an ES8311 codec (I2C 0x18 on the shared bus) whose DAC feeds a
// small power amplifier (enable on GPIO 11) and speaker header. This module gives the
// gateway a voice: TONES and short NOTE SEQUENCES (chimes, alerts, game feedback),
// synthesized on-device and played over the same full-duplex I2S port the microphones
// use (audio.h). No PCM upload/playback yet -- tones cover the use cases and keep the
// surface small; samples can join later without breaking anything.
//
// Lifecycle mirrors audio.h: soundInit() runs ONCE from setup() (single-threaded I2C
// window -- see audio.h's note about rtc's raw Wire use); playback spawns a small
// synth task on demand which disables the TX channel and the amp after ~5 s idle.
// Quiet Time refuses new sounds the same way it darkens the panel.

#pragma once
#include <stdint.h>

#define SOUND_MAX_NOTES 32

void soundInit();                 // probe + configure the ES8311 (setup() only)
bool soundAvailable();            // codec found and configured
bool soundPlaying();              // synth currently emitting
// Queue a sequence of {freqHz, ms} notes (freq 0 = rest) at vol 0..100.
// Replaces anything queued. Returns false if the codec is absent.
bool soundPlay(const uint16_t* freq, const uint16_t* ms, int n, uint8_t vol);
void soundStop();                 // stop now (also what Quiet Time calls)
