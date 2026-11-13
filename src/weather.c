// weather.c - W: PICOAMP's weather page (weather.asm).
//
// The weather now: a big picture that moves (the sun's rays turn, clouds
// drift, stars twinkle at night, rain and snow fall, a thunderstorm flashes),
// the temperature in big digits, the weather in words, how warm it feels, the
// humidity and the wind, the date and the time. The next days in columns: the
// day, a small picture, the highest and lowest temperature, the chance of rain.
// Open-Meteo's forecast (wxsvc.c). C clock, T news, W/ESC/Enter/BS back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "wxsvc.h"
#include "sys.h"
#include "util.h"

enum { K_SUN, K_PART, K_CLOUD, K_FOG, K_DRIZ, K_RAIN, K_SNOW, K_THUN, K_MOON, K_MOONP };
#define G_THUN GREY_D

static const uint8_t wx_cols[10][3] = {        // back, cloud, rain
    { YELLOW, WHITE, CYAN }, { YELLOW, WHITE, CYAN }, { YELLOW, GREY_L, CYAN }, { YELLOW, WHITE, GREY_L },
    { YELLOW, GREY_L, CYAN }, { YELLOW, GREY, CYAN }, { YELLOW, GREY_L, WHITE }, { YELLOW, G_THUN, CYAN },
    { GREY_L, WHITE, WHITE }, { GREY_L, WHITE, WHITE },
};
static const int wx_sin[60] = { 0,6,12,19,24,30,35,40,45,49,52,55,57,59,60,60,60,59,57,55,52,49,45,40,35,30,24,19,12,6,
                                0,-6,-12,-19,-24,-30,-35,-40,-45,-49,-52,-55,-57,-59,-60,-60,-60,-59,-57,-55,-52,-49,-45,-40,-35,-30,-24,-19,-12,-6 };
static const char *const wd[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *const days_l[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                        "September", "October", "November", "December" };

static int kind_of(int code, bool day)
{
    static const struct { int max, k; } t[] = {
        { 0, K_SUN }, { 2, K_PART }, { 3, K_CLOUD }, { 48, K_FOG }, { 57, K_DRIZ }, { 67, K_RAIN }, { 77, K_SNOW },
        { 82, K_RAIN }, { 86, K_SNOW }, { 99, K_THUN }, { 255, K_CLOUD },
    };
    int k = K_CLOUD;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (code <= t[i].max) { k = t[i].k; break; }
    if (!day) { if (k == K_SUN) k = K_MOON; else if (k == K_PART) k = K_MOONP; }
    return k;
}

// ---------------- the three layers: back (sun, moon, bolt), cloud, rain ----------------
static uint8_t L[3][64][64];
static bool    small;          // 32 x 32 (coordinates halved)
static int     phase, drift;
static bool    flash, erase;

static int sc(int v) { return small ? v >> 1 : v; }
static int lim(void) { return small ? 32 : 64; }

static void span(int layer, int x0, int x1, int y)
{
    if (y < 0 || y >= lim()) return;
    for (int x = x0; x <= x1; x++) if (x >= 0 && x < lim()) L[layer][y][x] = erase ? 0 : 1;
}

static void disk(int layer, int x, int y, int r)
{
    x = sc(x); y = sc(y); r = sc(r);
    for (int dy = -r; dy <= r; dy++)
    {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
        span(layer, x - w, x + w, y + dy);
    }
}

static void hline(int layer, int x0, int x1, int y) { span(layer, sc(x0), sc(x1), sc(y)); }

static void line(int layer, int x0, int y0, int x1, int y1)
{
    x0 = sc(x0); y0 = sc(y0); x1 = sc(x1); y1 = sc(y1);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;)
    {
        if (x0 >= 0 && x0 < lim() && y0 >= 0 && y0 < lim()) L[layer][y0][x0] = 1;
        if (x0 == x1 && y0 == y1) return;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void star(int x, int y)
{
    x = sc(x); y = sc(y);
    span(2, x - 1, x + 1, y);
    span(2, x, x, y - 1);
    span(2, x, x, y + 1);
}

static void cloud(int x, int y)
{
    disk(1, x - 12, y + 2, 9);
    disk(1, x, y - 4, 12);
    disk(1, x + 12, y + 2, 9);
    for (int r = 0; r < 10; r++) hline(1, x - 12, x + 12, y + 2 + r);
}

static void moon(int x, int y)
{
    disk(0, x, y, 13);
    erase = true;
    disk(0, x + 7, y - 6, 11);
    erase = false;
}

static void pt(int cx, int cy, int ang, int r, int *x, int *y)
{
    // (sin * 60) * r / 64, as the Z80 does (the high byte of * 4)
    *x = cx + ((wx_sin[ang % 60] * r * 4) >> 8);
    *y = cy - ((wx_sin[(ang + 15) % 60] * r * 4) >> 8);
}

static void rays(int cx, int cy, int r0, int r1)
{
    for (int i = 0, a = phase % 5; i < 12; i++, a += 5)
    {
        int x0, y0, x1, y1;
        pt(cx, cy, a, r0, &x0, &y0);
        pt(cx, cy, a, r1, &x1, &y1);
        line(0, x0, y0, x1, y1);
    }
}

static void marks(int what)
{
    static const int xy[3][2] = { { 18, 46 }, { 32, 50 }, { 46, 46 } };
    if (!small) return;                     // (big: they fall)
    for (int i = 0; i < 3; i++)
    {
        int x = xy[i][0], y = xy[i][1];
        if (what == 1) disk(2, x, y, 2);
        else if (what == 2) line(2, x, y, x - 4, y + 10);
        else star(x, y);
    }
}

static void draw_kind(int k)
{
    memset(L, 0, sizeof L);
    switch (k)
    {
    case K_SUN: disk(0, 32, 32, 13); rays(32, 32, 17, 25); break;
    case K_MOON:
        moon(30, 32);
        if (!small)
        {
            static const int stars[5][2] = { { 8, 10 }, { 54, 8 }, { 56, 44 }, { 10, 50 }, { 46, 56 } };
            for (int i = 0; i < 5; i++) if ((phase + 5 - i) & 3) star(stars[i][0], stars[i][1]);
        }
        break;
    case K_PART: disk(0, 22, 22, 11); rays(22, 22, 14, 20); cloud(38, 40); break;
    case K_MOONP: moon(22, 22); cloud(38, 40); break;
    case K_CLOUD: cloud(32 + (small ? 0 : drift), 32); break;
    case K_FOG:
        for (int i = 0, c = 18; i < 4; i++, c += 10)
        {
            int a = (phase + c) & 7;
            if (a >= 4) a = 8 - a;
            int x = a + 6;
            hline(2, x, x + 44, c);
        }
        break;
    case K_DRIZ: cloud(32, 24); marks(1); break;
    case K_RAIN: cloud(32, 24); marks(2); break;
    case K_SNOW: cloud(32, 24); marks(3); break;
    case K_THUN:
        cloud(32, 24);
        if (flash || small)
        {
            static const int bolt[3][4] = { { 34, 38, 28, 48 }, { 28, 48, 36, 48 }, { 36, 48, 30, 60 } };
            for (int i = 0; i < 3; i++)
            {
                line(0, bolt[i][0], bolt[i][1], bolt[i][2], bolt[i][3]);
                line(0, bolt[i][0] + 1, bolt[i][1], bolt[i][2] + 1, bolt[i][3]);
            }
        }
        break;
    }
}

// the layers onto the screen at (x0, y0): the cloud over the rain over the back
static void out(int k, int x0, int y0)
{
    int n = lim();
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
        {
            uint8_t c = L[1][y][x] ? wx_cols[k][1] : L[2][y][x] ? wx_cols[k][2] : L[0][y][x] ? wx_cols[k][0] : 0;
            if (c) gfx_pset(x0 + x, y0 + y, c);
        }
}

// ---------------- rain and snow (the MSX's sprites) ----------------
static int     drops[6][2];
static uint8_t seed = 0x5A;
static uint8_t rnd(void)
{
    uint8_t c = seed;
    seed = (uint8_t)(seed << 1) ^ ((seed & 0x80) ? 0x1D : 0);
    return seed ^ c;
}

static void drops_init(void)
{
    for (int i = 0; i < 6; i++) { drops[i][0] = (rnd() & 31) + 16; drops[i][1] = (rnd() & 31) + 30; }
}

static void drops_step(int k)
{
    int v = k == K_DRIZ ? 2 : k == K_SNOW ? 1 : 3;
    for (int i = 0; i < 6; i++)
    {
        drops[i][1] += v;
        if (drops[i][1] >= 62) { drops[i][0] = (rnd() & 31) + 16; drops[i][1] = 34; }
    }
}

static void drops_draw(int k)
{
    for (int i = 0; i < 6; i++)
    {
        int x = 8 + drops[i][0], y = 16 + drops[i][1];
        if (k == K_SNOW)
        {
            x += ((phase + 6 - i) & 4) >> 2;
            gfx_pset(x + 1, y, WHITE);
            gfx_hline(x, y + 1, 3, WHITE);
            gfx_pset(x + 1, y + 2, WHITE);
        }
        else for (int j = 0; j < 4; j++) gfx_pset(x, y + j, CYAN);
    }
}

// ---------------- the page ----------------
static wx_info  w;
static uint32_t step_ms;
static int      big_kind = -1;
static int      flash_left;

static void ctext(int row, int col, int cells, const char *s, uint8_t colour)
{
    int pad = (8 * cells - gfx_text6_px(s)) / 2;
    if (pad < 0) pad = 0;
    gfx_text6(row, col, cells, s, colour, -pad, 0);
}

static void deg(char *out, size_t n, int t10)         // rounded degrees ("-0" is 0)
{
    int a = t10 < 0 ? -t10 : t10, d = (a + 5) / 10;
    snprintf(out, n, "%s%d", t10 < 0 && d ? "-" : "", d);
}

static void wx_frame(void)
{
    wx_get(&w);
    gfx_clear(NAVY);
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    char l[220];
    snprintf(l, sizeof l, "Weather in %s", w.have_place ? w.place : "?");
    gfx_text6(1, 0, 32, l, COL(YELLOW, NAVY), 0, 0);
    for (int c = 0; c < 32; c++) gfx_hline(c * 8, 11 * 8 + 3, 8, BLUE);
    ctext(21, 0, 32, "Weather: Open-Meteo.com", COL(GREY, NAVY));
    const char *n = ui_note_text();
    gfx_text6(23, 0, 32, n[0] ? n : "C clock  T news  W or ESC back", n[0] ? COL(RED, NAVY) : COL(GREY, NAVY), 0, 0);
    if (!w.have_wx)
    {
        ctext(5, 0, 32, w.error[0] ? w.error : "Waiting for the weather (it needs the internet)", COL(WHITE, NAVY));
        return;
    }
    // the big picture: a step every 200 ms
    int bk = kind_of(w.code, w.is_day);
    if (bk != big_kind)
    {
        big_kind = bk;
        drops_init();
        flash = false;
        flash_left = 0;
        gfx_set_rgb(G_THUN, 3, 3, 4);
    }
    uint32_t now = sys_millis();
    if (now - step_ms >= 200)
    {
        step_ms = now;
        phase++;
        if (bk == K_CLOUD)
        {
            int a = (phase >> 2) & 7;
            if (a >= 4) a = 8 - a;
            drift = a - 2;
        }
        if (bk == K_THUN)
        {
            if (flash_left) { if (!--flash_left) { flash = false; gfx_set_rgb(G_THUN, 3, 3, 4); } }
            else if (rnd() < 20) { flash = true; flash_left = 2; gfx_set_rgb(G_THUN, 7, 7, 7); }
        }
        if (bk >= K_DRIZ && bk <= K_THUN) drops_step(bk == K_THUN ? K_RAIN : bk);
    }
    small = false;
    draw_kind(bk);
    out(bk, 8, 16);
    if (bk >= K_DRIZ && bk <= K_THUN) drops_draw(bk == K_THUN ? K_RAIN : bk);
    // the temperature in big digits, then the words
    int a = w.t10 < 0 ? -w.t10 : w.t10, d = (a + 5) / 10;
    int col = 10;
    if (w.t10 < 0 && d)
    {
        static const uint8_t minus[8] = { 0, 0, 0, 0x7E, 0x7E, 0, 0, 0 };
        gfx_cell(4, 10, minus, COL(WHITE, NAVY));
        col = 11;
    }
    if (d > 99) d = 99;
    if (d >= 10) { gfx_digit(3, col, d / 10, COL(WHITE, NAVY)); col += 2; }
    gfx_digit(3, col, d % 10, COL(WHITE, NAVY));
    gfx_text6(3, col + 2, 1, "C", COL(WHITE, NAVY), 0, 0);
    gfx_text6(3, 17, 15, wx_words(w.code), COL(YELLOW, NAVY), 0, 0);
    int f = w.feel10 < 0 ? -w.feel10 : w.feel10;
    snprintf(l, sizeof l, "Feels %s%d.%d C", w.feel10 < 0 ? "-" : "", f / 10, f % 10);
    gfx_text6(4, 17, 15, l, COL(WHITE, NAVY), 0, 0);
    snprintf(l, sizeof l, "Humidity %d%%", w.hum);
    gfx_text6(5, 17, 15, l, COL(WHITE, NAVY), 0, 0);
    snprintf(l, sizeof l, "Wind %d km/h", w.wind);
    gfx_text6(6, 17, 15, l, COL(WHITE, NAVY), 0, 0);
    wx_time t;
    wx_now(&t, true);
    snprintf(l, sizeof l, "%s %d %s  %02d:%02d", days_l[t.wday % 7], t.day, months[(t.month + 11) % 12], t.hour, t.min);
    gfx_text6(8, 10, 22, l, COL(CYAN, NAVY), 0, 0);
    // the next days
    int nd = w.ndays > 6 ? 6 : w.ndays;
    int col0 = (33 - 5 * nd) / 2;
    for (int i = 0; i < nd; i++)
    {
        wx_day *dy = &w.days[i];
        int c = col0 + 5 * i;
        ctext(12, c, 4, i ? wd[dy->wday % 7] : "Today", COL(YELLOW, NAVY));
        int k = kind_of(dy->code, true);
        small = true;
        draw_kind(k);
        out(k, c * 8, 13 * 8);
        char tb[16];
        deg(tb, sizeof tb, dy->tmax10);
        ctext(17, c, 4, tb, COL(ORANGE, NAVY));
        deg(tb, sizeof tb, dy->tmin10);
        ctext(18, c, 4, tb, COL(CYAN, NAVY));
        if (dy->pp >= 0) { snprintf(tb, sizeof tb, "%d%%", dy->pp); ctext(19, c, 4, tb, COL(GREY_L, NAVY)); }
    }
}

static void wx_open(void) { big_kind = -1; wx_refresh(); }
static void wx_close(void) { gfx_reset_palette(); }

static void wx_key(int k)
{
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    switch (u)
    {
    case K_ESC: case K_ENTER: case K_BS: case 'W': case 'Z': ui_page(PG_PLAYER); return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'T': ui_page(PG_NEWS); return;
    case 'G': case 'V': return;
    }
    ui_playback_key(k);
}

static void wx_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (b == MOUSE_RIGHT) ui_page(PG_PLAYER);
}

static void wx_describe(char *out, size_t n)
{
    size_t o = (size_t)snprintf(out, n, "weather place='%s' now=%d.%d code=%d (%s) feels=%d hum=%d wind=%d day=%d days=%d err='%s'",
                                w.place, w.t10 / 10, abs(w.t10 % 10), w.code, wx_words(w.code), w.feel10, w.hum, w.wind,
                                w.is_day, w.ndays, w.error);
    for (int i = 0; i < w.ndays && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "\n  %04d-%02d-%02d %s code=%d max=%d min=%d rain=%d%%", w.days[i].year,
                              w.days[i].month, w.days[i].day, wd[w.days[i].wday % 7], w.days[i].code, w.days[i].tmax10,
                              w.days[i].tmin10, w.days[i].pp);
}

const page_t page_wx = { wx_open, wx_frame, wx_key, wx_mouse, wx_close, wx_describe };
