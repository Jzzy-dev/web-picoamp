// gfx.h - the MSX2 SCREEN 4 look: a 256 x 192 picture made of 32 x 24 cells
// of 8 x 8 pixels, every 8-pixel row of a cell one foreground and one
// background colour, 16 colours from PICOAMP's palette.
//
// A colour byte is fg * 16 + bg, as in the VDP's colour table.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GFX_W 256              // SCREEN 4 (the player, the pages)
#define GFX_H 192
#define GFX_MAXW 512           // TEXT 2 (the news): 512 x 212, 80 columns of 6 pixels
#define GFX_MAXH 212

// palette indices (PICOAMP's own palette)
enum {
    BLK = 1, BLUE_D, BLUE, CYAN, WHITE, GREY, GREEN_D, GREEN, YELLOW, ORANGE,
    RED, MAGENTA, GREY_D, NAVY, GREY_L
};
#define COL(fg, bg) ((uint8_t)((fg) * 16 + (bg)))

extern uint8_t gfx_fb[GFX_MAXH][GFX_MAXW];  // palette indices
extern uint32_t gfx_pal[16];               // 0x00RRGGBB
extern int gfx_w, gfx_h;                   // the picture's size now (shown 4:3)

void gfx_set_size(int w, int h);           // 256 x 192 or 512 x 212

void gfx_init(void);
// a page may change colours of the palette (as PICOAMP does on the MSX):
// r, g, b 0..7 (the MSX2's 9-bit colours); gfx_reset_palette puts them back
void gfx_set_rgb(int index, int r, int g, int b);
void gfx_reset_palette(void);
void gfx_clear(uint8_t colour);            // the whole screen, one colour index
void gfx_cell(int row, int col, const uint8_t pat[8], uint8_t colour);
void gfx_cell2(int row, int col, const uint8_t pat[8], const uint8_t colours[8]);
void gfx_char(int row, int col, unsigned ch, uint8_t colour);
// text in 8-pixel cells, padded with spaces to width cells (UTF-8 in)
void gfx_str(int row, int col, const char *text, int width, uint8_t colour);
void gfx_fill(int row, int col, int cells, uint8_t colour);
// 6-pixel text (42 characters a row) in width cells; off = pixels scrolled
// left; cycle > 0: the text again cycle pixels further (a ticker)
void gfx_text6(int row, int col, int width, const char *text, uint8_t colour, int off, int cycle);
int  gfx_text6_px(const char *text);       // width in pixels (6 a character)
void gfx_digit(int row, int col, int d, uint8_t colour);   // 2 x 3 cells
void gfx_colon(int row, int col, uint8_t colour);          // 1 x 3 cells
void gfx_pset(int x, int y, uint8_t index);
void gfx_hline(int x, int y, int w, uint8_t index);
void gfx_rect(int x, int y, int w, int h, uint8_t index);
// 8 x 8 font text at any pixel (fg only: bg < 0 leaves the background)
void gfx_text_at(int x, int y, const char *text, uint8_t fg, int bg);
// 6-pixel text at any pixel; returns the x after it
int  gfx_text6_at(int x, int y, const char *text, uint8_t fg, int bg, int max_px);
// a glyph of the font (for pages that draw their own)
const uint8_t *gfx_glyph(unsigned ch);

// UTF-8 -> the MSX character set (CP437-like accents, '?' for the rest);
// returns the number of characters written (out has room for max-1 and a 0)
int  gfx_to_msx(const char *utf8, uint8_t *out, int max);

// the picture as 0x00RRGGBB pixels (gfx_w * gfx_h)
void gfx_to_rgb(uint32_t *out);
