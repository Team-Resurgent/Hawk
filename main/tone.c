// Hawk — recognizable test-tone generator (the stand-in "voice").
//
// Pattern (1 s period): three ascending 250 ms notes (C5, E5, G5) then 250 ms
// of silence. Instantly identifiable by ear, and pitch-sensitive: if the Xbox
// and device disagree about the sample rate the arpeggio audibly shifts, which
// makes rate bugs obvious without instruments.
#include <math.h>
#include <string.h>
#include "tone.h"

#define TONE_AMPLITUDE   12000   // of 32767 — loud but clear of clipping
#define TONE_TABLE_BITS  8
#define TONE_TABLE_SIZE  (1 << TONE_TABLE_BITS)

static int16_t  s_sine[TONE_TABLE_SIZE];
static uint32_t s_rate = 16000;
static uint32_t s_phase;          // Q8.24 index into s_sine
static uint32_t s_pos;            // sample position inside the 1 s pattern

typedef struct { uint16_t freq_hz; } tone_note_t;
static const tone_note_t s_notes[4] = { {523}, {659}, {784}, {0} }; // 0 = rest

void tone_reset(uint32_t sample_rate) {
    if (s_sine[TONE_TABLE_SIZE / 4] == 0) {          // build table once
        for (int i = 0; i < TONE_TABLE_SIZE; i++) {
            s_sine[i] = (int16_t)lrintf(TONE_AMPLITUDE *
                                        sinf(2.0f * (float)M_PI * i / TONE_TABLE_SIZE));
        }
    }
    s_rate  = sample_rate ? sample_rate : 16000;
    s_phase = 0;
    s_pos   = 0;
}

void tone_fill(int16_t *dst, uint32_t nsamples) {
    const uint32_t note_len = s_rate / 4;            // 250 ms per pattern slot
    for (uint32_t i = 0; i < nsamples; i++) {
        uint32_t slot = (s_pos / note_len) & 3;
        uint16_t freq = s_notes[slot].freq_hz;
        if (freq == 0) {
            dst[i] = 0;
            s_phase = 0;
        } else {
            // phase increment = freq/rate revolutions per sample, in Q8.24
            uint32_t inc = (uint32_t)(((uint64_t)freq << 32) / s_rate) >> 8;
            dst[i] = s_sine[s_phase >> 24];
            s_phase += inc;
        }
        if (++s_pos >= note_len * 4) s_pos = 0;
    }
}
