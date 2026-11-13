// fullscreen.c - Z: three pictures made of the spectrum and the levels, as
// PICOAMP's full screen: Bars (24 bars of 20 cells, 40 steps, with peaks),
// Mirror (the bars from the middle up and their reflection down), Plasma (four
// sines, its 12 colours turning faster when the music is louder).
// Row 0 what plays; row 23 the picture's name and the time.
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "sys.h"

enum { FS_BARS, FS_MIRROR, FS_PLASMA, FS_N };
static const char *const fs_names[FS_N] = { "BARS", "MIRROR", "PLASMA" };
static int fs_mode;

static const uint8_t fs_segof[32] = { 0,1,3,4,5,6,8,9,10,12,13,14,15,17,18,19,21,22,23,25,26,27,28,30,31,32,34,35,36,37,39,40 };
static const uint8_t seg_of[32] = { 0,1,1,2,3,3,4,5,5,6,6,7,8,8,9,10,10,11,12,12,13,14,14,15,15,16,17,17,18,19,19,20 };

static uint8_t bars_col(int seg) { return seg < 22 ? GREEN : seg < 30 ? YELLOW : seg < 35 ? ORANGE : RED; }
static uint8_t mir_col(int seg)  { return seg < 6 ? BLUE : seg < 16 ? CYAN : WHITE; }

// one LED segment: 6 x 3 pixels in its cell (upper rows 0-2, lower rows 4-6)
static void segment(int col, int row, bool upper, bool flip, uint8_t c)
{
    int y0 = row * 8 + ((upper != flip) ? 0 : 4);
    if (flip) y0 += 1;                     // a reflected cell: rows 1-3 and 5-7
    for (int y = 0; y < 3; y++)
        for (int x = 1; x < 7; x++) gfx_pset(col * 8 + x, y0 + y, c);
}

// a bar of cells from the bottom row up (mirror: also down from row + 1)
static void bar(int col, int bottom, int cells, int lit, int pk, uint8_t (*colour)(int), bool mirror)
{
    for (int s = 0; s < cells * 2; s++)
    {
        bool on = s < lit, peak = s == pk;
        if (!on && !peak) continue;
        uint8_t c = peak && !on ? WHITE : colour(s);
        int cell = s / 2;
        bool upper = s & 1;
        segment(col, bottom - cell, upper, false, c);
        if (mirror) segment(col, bottom + 1 + cell, upper, true, c);
    }
}

static void draw_bars(bool mirror)
{
    int cells = mirror ? 10 : 20, bottom = mirror ? 11 : 21;
    for (int b = 0; b < VIZ_BANDS; b++)
    {
        int lv = ui_viz.level[b] & 31, pkl = ui_viz.peak[b] & 31;
        int lit = mirror ? seg_of[lv] : fs_segof[lv];
        int pk = -1;
        if (pkl)
        {
            pk = mirror ? seg_of[pkl] : fs_segof[pkl];
            if (pk < lit) pk = -1;
            else if (pk > cells * 2 - 1) pk = cells * 2 - 1;
        }
        bar(4 + b, bottom, cells, lit, pk, mirror ? mir_col : bars_col, mirror);
    }
}

// the plasma: v = S(12c) + S(3y) + S(7c + 2y) + S(5y - 9c) over the cells (c)
// and lines (y), 12 colours with dithered steps between them
static uint8_t sin64[256];
static uint8_t pl_v[176][32];
static const uint8_t pl_cols[12] = { 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 15 };
static const uint8_t dither[2][8] = { { 0x00,0x80,0x88,0xA8,0xAA,0xEA,0xEE,0xFE }, { 0x00,0x08,0x88,0x8A,0xAA,0xAE,0xEE,0xEF } };
static int      pl_phase = -1;          // the colour wheel: 0..191 (48 hues, 4 steps each)
static uint32_t pl_last;
static const uint8_t wheel[48][3] = {
    {7,0,0},{7,1,0},{7,2,0},{7,3,0},{7,4,0},{7,4,0},{7,5,0},{7,6,0},{7,7,0},{6,7,0},{5,7,0},{4,7,0},{4,7,0},{3,7,0},{2,7,0},{1,7,0},
    {0,7,0},{0,7,1},{0,7,2},{0,7,3},{0,7,4},{0,7,4},{0,7,5},{0,7,6},{0,7,7},{0,6,7},{0,5,7},{0,4,7},{0,4,7},{0,3,7},{0,2,7},{0,1,7},
    {0,0,7},{1,0,7},{2,0,7},{3,0,7},{4,0,7},{4,0,7},{5,0,7},{6,0,7},{7,0,7},{7,0,6},{7,0,5},{7,0,4},{7,0,4},{7,0,3},{7,0,2},{7,0,1},
};
static const uint8_t bright[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,1},{0,0,0,0,1,1,1,2},{0,0,0,1,1,2,2,3},
    {0,0,1,1,2,2,3,4},{0,0,1,2,2,3,4,5},{0,0,1,2,3,4,5,6},{0,1,2,3,4,5,6,7},
};

// as PICOAMP: the picture stays, its 12 palette colours turn round the colour
// wheel, 25 steps a second, 1..16 steps further each time (louder: more),
// brighter with the bass
static void plasma_colours(void)
{
    uint32_t now = sys_millis();
    if (pl_phase < 0) { pl_phase = 0; pl_last = now; }
    if (now - pl_last > 1000) pl_last = now - 40;
    while (now - pl_last >= 40)
    {
        pl_last += 40;
        pl_phase = (pl_phase + 1 + (((ui_viz.vu[0] + ui_viz.vu[1]) >> 4) & 15)) % 192;
    }
    int b = ui_viz.level[0];
    if (ui_viz.level[1] > b) b = ui_viz.level[1];
    if (ui_viz.level[2] > b) b = ui_viz.level[2];
    int br = 4 + ((b >> 3) & 3);
    if (br > 7) br = 7;
    for (int c = 0; c < 12; c++)
    {
        const uint8_t *w = wheel[(pl_phase / 4 + c * 4) % 48];
        gfx_set_rgb(pl_cols[c], bright[br][w[0]], bright[br][w[1]], bright[br][w[2]]);
    }
}

static void plasma_build(void)
{
    for (int i = 0; i < 256; i++)
    {
        double v = 32.0 + 31.5 * __builtin_sin(2.0 * 3.14159265358979 * i / 256.0);
        sin64[i] = (uint8_t)v;
    }
    for (int y = 0; y < 176; y++)
        for (int c = 0; c < 32; c++)
            pl_v[y][c] = (uint8_t)(sin64[(12 * c) & 255] + sin64[(3 * y) & 255] + sin64[(7 * c + 2 * y) & 255] +
                                   sin64[(5 * y - 9 * c) & 255]);
}

static void draw_plasma(void)
{
    plasma_colours();
    for (int y = 0; y < 176; y++)
        for (int c = 0; c < 32; c++)
        {
            int v = pl_v[y][c] * 3;              // v * 3 / 64: the step (0..11), then 8 dither steps
            int step = (v >> 6) % 12, frac = (v >> 3) & 7;
            uint8_t lo = pl_cols[step], hi = pl_cols[(step + 1) % 12];
            uint8_t pat = dither[y & 1][frac];
            for (int x = 0; x < 8; x++) gfx_fb[8 + y][c * 8 + x] = (pat & (0x80 >> x)) ? hi : lo;
        }
}

static void fs_open(void)
{
    if (!sin64[64]) plasma_build();
    pl_phase = -1;
}

static void fs_set(int m)
{
    fs_mode = (m + FS_N) % FS_N;
    gfx_reset_palette();
    pl_phase = -1;
}

static void fs_close(void) { gfx_reset_palette(); }

static void fs_frame(void)
{
    gfx_clear(NAVY);
    if (fs_mode == FS_PLASMA) draw_plasma();
    else draw_bars(fs_mode == FS_MIRROR);
    gfx_fill(0, 0, 32, COL(WHITE, NAVY));
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    char line[80];
    int e = ui_st.elapsed;
    snprintf(line, sizeof line, "%-11s%d:%02d   </>: next  Z: back", fs_names[fs_mode], e / 60, e % 60);
    gfx_fill(23, 0, 32, COL(WHITE, NAVY));
    gfx_text6(23, 0, 32, line, COL(WHITE, NAVY), 0, 0);
    const char *n = ui_note_text();
    if (n[0]) gfx_text6(22, 0, 32, n, COL(ui_note_error() ? RED : GREEN, NAVY), 0, 0);
}

static void fs_key(int k)
{
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    switch (u)
    {
    case K_LEFT: fs_set(fs_mode - 1); return;
    case K_RIGHT: case 'V': fs_set(fs_mode + 1); return;
    case 'Z': case K_ESC: case K_ENTER: case K_BS: ui_page(PG_PLAYER); return;
    case 'G': return;
    }
    ui_playback_key(k);
}

static void fs_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (b == MOUSE_LEFT) fs_set(fs_mode + 1);
    else if (b == MOUSE_RIGHT) ui_page(PG_PLAYER);
}

const page_t page_full = { fs_open, fs_frame, fs_key, fs_mouse, fs_close, NULL };
