// picture.h - a picture for MSX SCREEN 8, as the MSX engine 0.17 makes it:
// decoded (TJpgDec for baseline JPEG, as the engine; else the platform's
// decoder: PNG, progressive JPEG, BMP, GIF ...), fitted into 256 x 192 with
// the MSX's dot shape (1.1 times as wide as high), each dot the average of
// the picture's pixels under it, and dithered (Floyd-Steinberg, serpentine)
// to SCREEN 8's 256 colours (GGGRRRBB: 3 bits green, 3 red, 2 blue).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { PIC_OK, PIC_E_OPEN, PIC_E_FORMAT };

// a picture's bytes -> RGB (malloc'd, w * h * 3), at least min_w x min_h when it is that big
uint8_t *pic_decode(const uint8_t *data, size_t len, int min_w, int min_h, int *w, int *h);
// RGB -> SCREEN 8 (black around it)
void pic_to_s8(const uint8_t *rgb, int w, int h, uint8_t out[192][256]);
// a picture's bytes / a file -> SCREEN 8; PIC_xxx
int  pic_bytes_s8(const uint8_t *data, size_t len, uint8_t out[192][256]);
int  pic_file_s8(const char *path, uint8_t out[192][256]);
bool pic_is_picture_name(const char *name);     // .jpg .jpeg .png .bmp .gif (the platform's: more)

// the platform: decode anything it can, made smaller to at most max_w x max_h when bigger (NULL: it cannot)
uint8_t *plat_decode_rgb(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);
