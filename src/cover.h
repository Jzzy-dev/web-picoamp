// cover.h - the cover view's picture (V a third time), as PICOAMP on the MSX
// makes it: the picture of the track that plays (its tag's, or an image in
// its folder), 80 x 80 pixels in SCREEN 4 cells (two colours in every 8
// pixels of a row) with five palette entries of its own (pva_cover.c, the
// engine's file).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { COVER_NONE, COVER_MAKING, COVER_READY };

// the palette entries the cover may set (as the MSX: 2, 7, 10, 13, 15)
#define COVER_FREE 0xA484u

// a file starts (its path; the platform reads it) / a station or nothing
void cover_track(const char *path);
void cover_none(void);
// a picture's bytes: the tag's (from_tag: before any folder image), or a
// folder image the platform found (the web); JPEG, PNG ...
void cover_from_bytes(const uint8_t *data, size_t len);
// a picture decoded already: RGB rows (the platform's decoder)
void cover_from_rgb(int w, int h, const uint8_t *rgb, int stride);
// the picture of a tag in memory (ID3v2 APIC, FLAC PICTURE): where, or false
bool cover_find_in_tag(const uint8_t *b, size_t n, size_t *off, size_t *len);

int cover_state(void);                  // COVER_xxx
unsigned cover_gen(void);               // +1 when it changes
// COVER_READY: palette (32 bytes), 800 pattern bytes, 800 colour bytes (pva_cover.h)
const uint8_t *cover_data(void);

// the platform: decode what TJpgDec cannot (PNG, progressive JPEG ...); false: it cannot either
// (on the web it says yes and gives the pixels later, through cover_from_rgb)
bool plat_decode_image(const uint8_t *data, size_t len);
