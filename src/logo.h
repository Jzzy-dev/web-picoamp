// logo.h - the default pictures (defaults.c): what the cover view shows when nothing has a
// cover (as PICOAMP on the MSX). The web finds logos and covers in the page (picoamp.js).
#pragma once
#include <stdint.h>

#define DEFAULT_PICTURES 3
extern const uint8_t *const default_picture[DEFAULT_PICTURES];
extern const unsigned default_picture_len[DEFAULT_PICTURES];
