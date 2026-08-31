// Hawk — microphone audio source. The ESP32 has no microphone, so milestone 1
// "speaks" a recognizable repeating tone pattern into the mic endpoint; a later
// milestone streams a decoded OGG file through the same interface.
#pragma once
#include <stdint.h>

// (Re)start the pattern at the given output sample rate (called whenever the
// Xbox sets a rate via the vendor request).
void tone_reset(uint32_t sample_rate);

// Fill dst with the next nsamples of 16-bit mono PCM, advancing the pattern.
void tone_fill(int16_t *dst, uint32_t nsamples);
