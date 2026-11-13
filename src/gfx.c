// gfx.c - the MSX2 SCREEN 4 look (see gfx.h)
#include <string.h>
#include "gfx.h"
#include "assets.h"

uint8_t  gfx_fb[GFX_MAXH][GFX_MAXW];
uint32_t gfx_pal[16];
int      gfx_w = GFX_W, gfx_h = GFX_H;

void gfx_set_size(int w, int h)
{
    if (w < 1 || w > GFX_MAXW) w = GFX_W;
    if (h < 1 || h > GFX_MAXH) h = GFX_H;
    gfx_w = w;
    gfx_h = h;
}

const uint8_t *gfx_glyph(unsigned ch) { return &font8[(ch & 255) * 8]; }

// PICOAMP's palette: 16 x (0RRR0BBB, 00000GGG)
static const uint8_t palette[32] = {
    0x00,0x00, 0x00,0x00, 0x13,0x01, 0x26,0x02, 0x37,0x06, 0x77,0x07, 0x45,0x04, 0x01,0x04,
    0x22,0x07, 0x71,0x07, 0x70,0x04, 0x71,0x01, 0x66,0x02, 0x23,0x02, 0x02,0x00, 0x56,0x05
};

void gfx_set_rgb(int i, int r, int g, int b)
{
    if (i < 0 || i > 15) return;
    r &= 7; g &= 7; b &= 7;
    gfx_pal[i] = ((unsigned)(r * 255 / 7) << 16) | ((unsigned)(g * 255 / 7) << 8) | (unsigned)(b * 255 / 7);
}

void gfx_reset_palette(void)
{
    for (int i = 0; i < 16; i++)
        gfx_set_rgb(i, (palette[2 * i] >> 4) & 7, palette[2 * i + 1] & 7, palette[2 * i] & 7);
}

void gfx_init(void)
{
    gfx_reset_palette();
    gfx_clear(NAVY);
}

void gfx_clear(uint8_t colour)
{
    memset(gfx_fb, colour & 15, sizeof gfx_fb);
}

static inline bool cell_ok(int row, int col)
{
    return row >= 0 && row < 24 && col >= 0 && col < 32;
}

void gfx_cell2(int row, int col, const uint8_t pat[8], const uint8_t colours[8])
{
    if (!cell_ok(row, col)) return;
    for (int y = 0; y < 8; y++)
    {
        uint8_t *p = &gfx_fb[row * 8 + y][col * 8];
        uint8_t fg = colours[y] >> 4, bg = colours[y] & 15, bits = pat[y];
        for (int x = 0; x < 8; x++) p[x] = (bits & (0x80 >> x)) ? fg : bg;
    }
}

void gfx_cell(int row, int col, const uint8_t pat[8], uint8_t colour)
{
    uint8_t c[8];
    memset(c, colour, 8);
    gfx_cell2(row, col, pat, c);
}

void gfx_char(int row, int col, unsigned ch, uint8_t colour)
{
    gfx_cell(row, col, &font8[(ch & 255) * 8], colour);
}

void gfx_str(int row, int col, const char *text, int width, uint8_t colour)
{
    uint8_t buf[256];
    int n = gfx_to_msx(text ? text : "", buf, sizeof buf);
    for (int i = 0; i < width; i++) gfx_char(row, col + i, i < n ? buf[i] : ' ', colour);
}

void gfx_fill(int row, int col, int cells, uint8_t colour)
{
    static const uint8_t blank[8];
    for (int i = 0; i < cells; i++) gfx_cell(row, col + i, blank, colour);
}

// the strip of a 6-pixel text: (width + 2) cells, a guard cell on each side,
// as PICOAMP's text6 builds it
static void text6_pass(uint8_t *strip, int strip_w, const uint8_t *s, int n, int x)
{
    for (int i = 0; i < n; i++, x += 6)
    {
        if (x + 8 <= 0) continue;
        if (x >= strip_w) break;
        const uint8_t *g = &font8[s[i] * 8];
        for (int y = 0; y < 8; y++)
            for (int b = 0; b < 8; b++)
                if (g[y] & (0x80 >> b))
                {
                    int px = x + b;
                    if (px >= 0 && px < strip_w) strip[y * strip_w + px] = 1;
                }
    }
}

int gfx_text6_px(const char *text)
{
    uint8_t buf[512];
    return 6 * gfx_to_msx(text ? text : "", buf, sizeof buf);
}

void gfx_text6(int row, int col, int width, const char *text, uint8_t colour, int off, int cycle)
{
    uint8_t s[512];
    int n = gfx_to_msx(text ? text : "", s, sizeof s);
    if (width > 32) width = 32;
    if (width < 1) return;
    int sw = (width + 2) * 8;
    uint8_t strip[8 * 36 * 8];
    memset(strip, 0, (size_t)sw * 8);
    int x = 8 - off;
    text6_pass(strip, sw, s, n, x);
    if (cycle > 0) text6_pass(strip, sw, s, n, x + cycle);
    for (int c = 0; c < width; c++)
    {
        uint8_t pat[8];
        for (int y = 0; y < 8; y++)
        {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++)
                if (strip[y * sw + (c + 1) * 8 + k]) b |= 0x80 >> k;
            pat[y] = b;
        }
        gfx_cell(row, col + c, pat, colour);
    }
}

void gfx_digit(int row, int col, int d, uint8_t colour)
{
    if (d < 0 || d > 9) d = 0;
    const uint8_t *p = &digits7[d * 48];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 2; c++, p += 8) gfx_cell(row + r, col + c, p, colour);
}

void gfx_colon(int row, int col, uint8_t colour)
{
    for (int r = 0; r < 3; r++) gfx_cell(row + r, col, &colon7[r * 8], colour);
}

void gfx_pset(int x, int y, uint8_t index)
{
    if (x >= 0 && x < gfx_w && y >= 0 && y < gfx_h) gfx_fb[y][x] = index & 15;
}

void gfx_text_at(int x, int y, const char *text, uint8_t fg, int bg)
{
    uint8_t s[512];
    int n = gfx_to_msx(text ? text : "", s, sizeof s);
    for (int i = 0; i < n; i++, x += 8)
    {
        const uint8_t *g = &font8[s[i] * 8];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
            {
                if (g[r] & (0x80 >> b)) gfx_pset(x + b, y + r, fg);
                else if (bg >= 0) gfx_pset(x + b, y + r, (uint8_t)bg);
            }
    }
}

int gfx_text6_at(int x, int y, const char *text, uint8_t fg, int bg, int max_px)
{
    uint8_t s[1024];
    int n = gfx_to_msx(text ? text : "", s, sizeof s);
    int x0 = x;
    for (int i = 0; i < n; i++, x += 6)
    {
        if (max_px > 0 && x + 6 > x0 + max_px) break;
        const uint8_t *g = &font8[s[i] * 8];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 6; b++)
            {
                if (g[r] & (0x80 >> b)) gfx_pset(x + b, y + r, fg);
                else if (bg >= 0) gfx_pset(x + b, y + r, (uint8_t)bg);
            }
    }
    return x;
}

void gfx_hline(int x, int y, int w, uint8_t index)
{
    for (int i = 0; i < w; i++) gfx_pset(x + i, y, index);
}

void gfx_rect(int x, int y, int w, int h, uint8_t index)
{
    for (int j = 0; j < h; j++) gfx_hline(x, y + j, w, index);
}

// Unicode -> MSX character codes 128..175 (the MSX international set, as CP437)
static const uint16_t msx_hi[48] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB
};

static unsigned fold(uint32_t u)
{
    if (u >= 32 && u < 127) return u;
    for (int i = 0; i < 48; i++) if (msx_hi[i] == u) return 128 + i;
    // letters without a glyph of their own: the plain letter
    static const struct { uint16_t u; char c; } f[] = {
        {0xC0,'A'},{0xC1,'A'},{0xC2,'A'},{0xC3,'A'},{0xE3,'a'},{0xC8,'E'},{0xCA,'E'},{0xCB,'E'},
        {0xCC,'I'},{0xCD,'I'},{0xCE,'I'},{0xCF,'I'},{0xD2,'O'},{0xD3,'O'},{0xD4,'O'},{0xD5,'O'},
        {0xF5,'o'},{0xD8,'O'},{0xF8,'o'},{0xD9,'U'},{0xDA,'U'},{0xDB,'U'},{0xDD,'Y'},{0xFD,'y'},
        {0xDF,'s'},{0x2018,'\''},{0x2019,'\''},{0x201C,'"'},{0x201D,'"'},{0x2013,'-'},{0x2014,'-'},
        {0x2026,'.'},{0xA0,' '},{0x160,'S'},{0x161,'s'},{0x17D,'Z'},{0x17E,'z'},{0x152,'O'},{0x153,'o'},
        {0x141,'L'},{0x142,'l'},{0x15A,'S'},{0x15B,'s'},{0x106,'C'},{0x107,'c'},{0x10C,'C'},{0x10D,'c'},
        {0x158,'R'},{0x159,'r'},{0x11A,'E'},{0x11B,'e'},{0x143,'N'},{0x144,'n'},{0x147,'N'},{0x148,'n'},
    };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) if (f[i].u == u) return (unsigned char)f[i].c;
    if (u == '\t') return ' ';
    return '?';
}

int gfx_to_msx(const char *s, uint8_t *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max - 1)
    {
        uint32_t u = *p++;
        if (u >= 0xC0)
        {
            int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
            u &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) u = (u << 6) | (*p++ & 0x3F);
        }
        else if (u >= 0x80) u = '?';            // a stray continuation byte
        if (u < 32) continue;
        out[n++] = (uint8_t)fold(u);
    }
    out[n] = 0;
    return n;
}

void gfx_to_rgb(uint32_t *out)
{
    for (int y = 0; y < gfx_h; y++)
        for (int x = 0; x < gfx_w; x++) *out++ = gfx_pal[gfx_fb[y][x]];
}
