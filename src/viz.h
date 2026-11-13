// viz.h - spectrum, VU and scope data (as the PicoVerse Audio Engine makes it)
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VIZ_BANDS 24
#define VIZ_MAX   31
#define VIZ_WAVE  64

typedef struct
{
    uint8_t  level[VIZ_BANDS];   // 0..31, 2 dB a step
    uint8_t  peak[VIZ_BANDS];
    uint8_t  vu[2];              // left, right 0..31
    int8_t   scope[VIZ_WAVE];    // the newest samples / 256, every 4th
    uint16_t freq[VIZ_BANDS];    // centre of each band, Hz
} viz_data;

void viz_init(void);
void viz_feed(const int16_t *stereo, uint32_t frames, uint32_t rate);   // audio thread
bool viz_poll(bool playing, viz_data *out);      // screen: true when it was updated
