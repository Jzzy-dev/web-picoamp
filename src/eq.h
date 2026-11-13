// eq.h - 5-band equalizer (as in the PicoVerse Audio Engine)
#pragma once
#include <stdint.h>

#define EQ_BANDS   5
#define EQ_MAX_DB  12
#define EQ_PRESETS 8

extern const int8_t eq_presets[EQ_PRESETS][EQ_BANDS];
extern const char *const eq_names[EQ_PRESETS + 1];   // [EQ_PRESETS] = "EQ OWN"

void eq_set(const int8_t *gains);   // any thread; taken by the audio thread
void eq_reset(void);                // audio thread: a new track
void eq_process(int16_t *stereo, uint32_t frames, uint32_t rate);   // audio thread
int  eq_find_preset(const int8_t *gains);    // EQ_PRESETS: none of them
