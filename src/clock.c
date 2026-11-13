// clock.c - C: PICOAMP's clock page (clock.asm), and the sleep timer and the
// alarm behind it.
//
// Digital: HH:MM in 7-segment digits 80 pixels high, a blinking colon, a
// seconds bar, the date, the place, the weather now and where the time comes
// from; the sleep timer and the alarm. Analog: a dial with hour strokes and
// minute dots, the hands, a second hand of red dots, the time in big digits
// beside it. The digits, the dial and the small spectrum are four colours that
// turn round a colour wheel while music plays (faster when louder, brighter
// with the bass), or one colour chosen with X.
//
// Keys: Left/Right or V the face, I the time (the place's / the device's), L the
// place, X the colours, B the small spectrum, O the sleep timer, A the alarm,
// W weather, T news, C/Z/ESC/Enter/BS back; the playback keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "config.h"
#include "wxsvc.h"
#include "audio.h"
#include "sys.h"
#include "util.h"

// the glow colours: palette entries the page takes over
#define G1 MAGENTA
#define G2 GREY_D
#define G3 GREY_L
#define G4 GREEN_D
static const int gents[4] = { G1, G2, G3, G4 };
#define A_KEYS COL(GREY, NAVY)

static const uint8_t calmpal[6][8] = {          // 0RRR0BBB, 00000GGG
    { 0x37,7, 0x27,6, 0x16,5, 0x15,4 },           // cyan to blue
    { 0x22,7, 0x11,6, 0x01,5, 0x00,4 },           // green
    { 0x70,6, 0x70,5, 0x60,3, 0x50,2 },           // amber
    { 0x72,2, 0x71,1, 0x60,0, 0x40,0 },           // red
    { 0x67,2, 0x57,1, 0x46,0, 0x35,0 },           // purple
    { 0x77,7, 0x66,6, 0x55,5, 0x44,4 },           // white
};
static const char *const colnames[7] = { "music", "cyan", "green", "amber", "red", "purple", "white" };
static const uint8_t wheel[48][3] = {
    {7,0,0},{7,1,0},{7,2,0},{7,3,0},{7,4,0},{7,4,0},{7,5,0},{7,6,0},{7,7,0},{6,7,0},{5,7,0},{4,7,0},{4,7,0},{3,7,0},{2,7,0},{1,7,0},
    {0,7,0},{0,7,1},{0,7,2},{0,7,3},{0,7,4},{0,7,4},{0,7,5},{0,7,6},{0,7,7},{0,6,7},{0,5,7},{0,4,7},{0,4,7},{0,3,7},{0,2,7},{0,1,7},
    {0,0,7},{1,0,7},{2,0,7},{3,0,7},{4,0,7},{4,0,7},{5,0,7},{6,0,7},{7,0,7},{7,0,6},{7,0,5},{7,0,4},{7,0,4},{7,0,3},{7,0,2},{7,0,1},
};
static const uint8_t bright[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,1},{0,0,0,0,1,1,1,2},{0,0,0,1,1,2,2,3},
    {0,0,1,1,2,2,3,4},{0,0,1,2,2,3,4,5},{0,0,1,2,3,4,5,6},{0,1,2,3,4,5,6,7},
};
static const char *const days[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                        "September", "October", "November", "December" };

static wx_time  t;
static bool     placed;
static wx_info  w;
static int      phase = -1;          // the colour wheel (-1: the calm colours shown)
static uint32_t phase_ms;
static char     msg[120];
static bool     msg_err;
static uint32_t msg_until;
// typing: 'L' the place, 'A' the alarm's time
static int      inp;
static char     inp_text[64];
// the place's search results
static bool     choosing, searching;
static char     results[8][160];
static int      nresults, rsel;

static void note(const char *m, bool err)
{
    u_copy(msg, sizeof msg, m);
    msg_err = err;
    msg_until = sys_millis() + 3000;
}

// ---------------- the colours ----------------
static void calm(void)
{
    const uint8_t *p = calmpal[cfg.clock_colours ? cfg.clock_colours - 1 : 0];
    for (int c = 0; c < 4; c++) gfx_set_rgb(gents[c], (p[2 * c] >> 4) & 7, p[2 * c + 1] & 7, p[2 * c] & 7);
}

static void colours_tick(void)
{
    if (cfg.clock_colours || ui_st.state != ST_PLAYING)
    {
        if (phase != -1) { phase = -1; }
        calm();
        return;
    }
    // 25 steps a second: faster round the wheel when it is loud
    uint32_t now = sys_millis();
    if (phase < 0) { phase = 0; phase_ms = now; }
    while (now - phase_ms >= 40)
    {
        phase_ms += 40;
        phase = (phase + 1 + ((ui_viz.vu[0] + ui_viz.vu[1]) >> 4 & 15)) % 192;
    }
    int lv = ui_viz.level[0] > ui_viz.level[1] ? ui_viz.level[0] : ui_viz.level[1];
    int br = 5 + (lv >> 3 & 3);
    if (br > 7) br = 7;
    for (int c = 0; c < 4; c++)
    {
        const uint8_t *rgb = wheel[(phase / 4 + 6 * c) % 48];
        gfx_set_rgb(gents[c], bright[br][rgb[0]], bright[br][rgb[1]], bright[br][rgb[2]]);
    }
}

// ---------------- the digital face ----------------
static uint8_t px[128][192];        // the picture: digits (192 x 80) or the dial (128 x 128)

static void hfill(int x0, int x1, int y)
{
    if (y < 0 || y >= 128) return;
    for (int x = x0; x <= x1; x++) if (x >= 0 && x < 192) px[y][x] = 1;
}

static void hseg(int x, int y, int len)
{
    static const int taper[8] = { 3, 2, 1, 0, 0, 1, 2, 3 };
    for (int r = 0; r < 8; r++) hfill(x + taper[r], x + len - 1 - taper[r], y + r);
}

static void vseg(int x, int y, int len)
{
    for (int r = 0; r < len; r++)
    {
        int cut = r < 4 ? 3 - r : (r >= len - 4 ? r - (len - 4) : 0);
        hfill(x + cut, x + 7 - cut, y + r);
    }
}

static void digit7(int d, int x)
{
    static const uint8_t segs[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    static const int list[7][4] = { {2,0,32,0}, {28,2,37,1}, {28,41,37,1}, {2,72,32,0}, {0,41,37,1}, {0,2,37,1}, {2,36,32,0} };
    for (int s = 0; s < 7; s++)
        if (segs[d] & (1 << s))
        {
            if (list[s][3]) vseg(x + list[s][0], list[s][1], list[s][2]);
            else hseg(x + list[s][0], list[s][1], list[s][2]);
        }
}

static uint8_t dgrad(int y) { return (uint8_t)gents[y < 20 ? 0 : y < 40 ? 1 : y < 60 ? 2 : 3]; }

static void draw_digital(void)
{
    memset(px, 0, sizeof px);
    digit7(t.hour / 10, 4);
    digit7(t.hour % 10, 48);
    digit7(t.min / 10, 108);
    digit7(t.min % 10, 152);
    if (!(t.sec & 1))
        for (int k = 0; k < 8; k++) { hfill(92, 99, 22 + k); hfill(92, 99, 50 + k); }
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 192; x++)
            if (px[y][x]) gfx_fb[16 + y][32 + x] = dgrad(y);
    // the seconds: a bar in row 13 (lines 1-5) on its track (line 6)
    int lit = (t.sec + 1) * 16 / 5;
    static const int line_col[5] = { G1, G1, G2, G2, G3 };
    for (int l = 0; l < 5; l++) gfx_hline(32, 105 + l, lit, (uint8_t)line_col[l]);
    gfx_hline(32, 110, 192, BLUE);
}

// ---------------- the analog face ----------------
static const int ck_sin[60] = { 0,6,12,19,24,30,35,40,45,49,52,55,57,59,60,60,60,59,57,55,52,49,45,40,35,30,24,19,12,6,
                                0,-6,-12,-19,-24,-30,-35,-40,-45,-49,-52,-55,-57,-59,-60,-60,-60,-59,-57,-55,-52,-49,-45,-40,-35,-30,-24,-19,-12,-6 };

// a length: 0 all, 1 7/8, 2 3/4, 3 5/8, 4 1/2, 5 1/4 (as the Z80's shifts: floor)
static int scale(int b, int mode)
{
    switch (mode)
    {
    case 1: return b - (b >> 3);
    case 2: return (b >> 1) + (b >> 2);
    case 3: return (b >> 1) + (b >> 3);
    case 4: return b >> 1;
    case 5: return b >> 2;
    }
    return b;
}

static void pt(int ang, int mode, int *x, int *y)
{
    *x = 64 + scale(ck_sin[ang % 60], mode);
    *y = 64 - scale(ck_sin[(ang + 15) % 60], mode);
}

static void apx(int x, int y) { if (x >= 0 && x < 128 && y >= 0 && y < 128) px[y][x] = 1; }

static void line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;)
    {
        apx(x0, y0);
        if (x0 == x1 && y0 == y1) return;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void hand(int ang, int mode, int thick)
{
    static const int offs[4][2] = { {0,0}, {1,0}, {0,1}, {1,1} };
    int hx, hy;
    pt(ang, mode, &hx, &hy);
    for (int i = 0; i < thick; i++) line(64 + offs[i][0], 64 + offs[i][1], hx + offs[i][0], hy + offs[i][1]);
}

static void draw_analog(void)
{
    memset(px, 0, sizeof px);
    for (int m = 0; m < 60; m++)
    {
        int x, y;
        pt(m, 0, &x, &y);
        apx(x, y);
        apx(x + 1, y);
        if (m % 5 == 0)
        {
            int x1, y1;
            pt(m, 1, &x1, &y1);
            line(x1, y1, x, y);
            line(x1 + 1, y1, x + 1, y);
        }
    }
    hand((t.hour % 12) * 5 + t.min / 12, 3, 4);
    hand(t.min, 1, 3);
    apx(63, 63); apx(64, 63); apx(63, 64); apx(64, 64);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++)
            if (px[y][x]) gfx_fb[16 + y][8 + x] = (uint8_t)gents[(y / 32) & 3];
    // the second hand: red dots at 1/4, 1/2, 3/4 and 7/8
    static const int hp[4] = { 5, 4, 2, 1 };
    for (int i = 0; i < 4; i++)
    {
        int x, y;
        pt(t.sec, hp[i], &x, &y);
        gfx_rect(8 + x - 1, 16 + y - 1, 3, 3, RED);
    }
    uint8_t dc = COL(RED, NAVY);
    gfx_digit(3, 18, t.hour / 10, dc);
    gfx_digit(3, 20, t.hour % 10, dc);
    gfx_colon(3, 22, dc);
    gfx_digit(3, 23, t.min / 10, dc);
    gfx_digit(3, 25, t.min % 10, dc);
    char s[8];
    snprintf(s, sizeof s, ":%02d", t.sec);
    gfx_text6(5, 27, 4, s, dc, 0, 0);
}

// ---------------- the text ----------------
static void ctext(int row, const char *s, uint8_t colour)
{
    int n = gfx_text6_px(s) / 6;
    int pad = 128 - 3 * n;
    if (pad < 0) pad = 0;
    gfx_text6(row, 0, 32, s, colour, -pad, 0);
}

static void wxstr(char *out, size_t n)
{
    out[0] = 0;
    if (!w.have_wx) return;
    int a = w.t10 < 0 ? -w.t10 : w.t10;
    snprintf(out, n, "%s%d.%d C %s", w.t10 < 0 ? "-" : "", a / 10, a % 10, wx_words(w.code));
}

static const char *srcstr(void)
{
    if (cfg.clock_pc_time) return "device clock";
    return placed ? "internet time" : "device clock (waiting)";
}

// "Sleep 25 min  Alarm 07:30 Mo-Fr !"
static int      sleep_min;           // the sleep timer: its length (0 off)
static uint32_t sleep_end;
static bool     ringing;
static void salstr(char *out, size_t n)
{
    out[0] = 0;
    size_t o = 0;
    if (sleep_min)
    {
        int left = (int)((sleep_end - sys_millis()) / 60000u) + 1;
        o += (size_t)snprintf(out + o, n - o, "Sleep %d min", left);
    }
    if (cfg.alarm_on)
        o += (size_t)snprintf(out + o, n - o, "%sAlarm %02d:%02d%s%s", o ? "  " : "", cfg.alarm_h, cfg.alarm_m,
                              cfg.alarm_weekdays ? " Mo-Fr" : "", ringing ? " !" : "");
}

static void lines(void)
{
    char date[80], wx[60], sal[80], l[200];
    snprintf(date, sizeof date, "%s %d %s %d", days[t.wday % 7], t.day, months[(t.month + 11) % 12], t.year);
    wxstr(wx, sizeof wx);
    salstr(sal, sizeof sal);
    const char *place = w.have_place ? w.place : "";
    if (!cfg.clock_analog)
    {
        ctext(14, date, COL(YELLOW, NAVY));
        ctext(15, place, COL(WHITE, NAVY));
        snprintf(l, sizeof l, "%s%s%s", wx, wx[0] ? "  " : "", srcstr());
        ctext(16, l, COL(CYAN, NAVY));
        ctext(17, sal, COL(YELLOW, NAVY));
        return;
    }
    // beside the dial: the day, the date, the city, the rest of the place
    gfx_text6(7, 18, 14, days[t.wday % 7], COL(YELLOW, NAVY), 0, 0);
    gfx_text6(8, 18, 14, strchr(date, ' ') + 1, COL(WHITE, NAVY), 0, 0);
    char city[160];
    u_copy(city, sizeof city, place);
    char *comma = strchr(city, ',');
    const char *rest = "";
    if (comma)
    {
        *comma = 0;
        rest = place + (comma - city) + 1;
        while (*rest == ' ') rest++;
    }
    gfx_text6(10, 18, 14, city, COL(WHITE, NAVY), 0, 0);
    gfx_text6(11, 18, 14, rest, COL(WHITE, NAVY), 0, 0);
    gfx_text6(12, 18, 14, sal, COL(YELLOW, NAVY), 0, 0);
    gfx_text6(14, 18, 14, wx, COL(WHITE, NAVY), 0, 0);
    gfx_text6(16, 17, 15, srcstr(), A_KEYS, 0, 0);
}

// the small spectrum: 24 bars of 4 cells (8 steps), rows 19-22
static void small_spectrum(void)
{
    static const uint8_t segof[32] = { 0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8 };
    static const int segcol[8] = { G1, G1, G2, G2, G3, G3, G4, RED };
    for (int b = 0; b < 24; b++)
    {
        int lit = segof[ui_viz.level[b] & 31], pk = -1;
        if (ui_viz.peak[b])
        {
            pk = segof[ui_viz.peak[b] & 31];
            if (pk < lit) pk = -1;
            else if (pk > 7) pk = 7;
        }
        for (int s = 0; s < 8; s++)
        {
            if (s >= lit && s != pk) continue;
            int cell = s / 2, y0 = (22 - cell) * 8 + ((s & 1) ? 0 : 4);
            gfx_rect((4 + b) * 8 + 1, y0, 6, 3, s >= lit ? WHITE : (uint8_t)segcol[s]);
        }
    }
}

// ---------------- the page ----------------
static void row23(void)
{
    gfx_fill(23, 0, 32, COL(GREY, NAVY));
    if (inp)
    {
        char l[120];
        snprintf(l, sizeof l, "%s%s%s", inp == 'L' ? "Place (Enter alone: by IP): " : "Alarm at (e.g. 7:30, w: Mo-Fr): ",
                 inp_text, (sys_millis() / 400) & 1 ? "_" : " ");
        int px_w = gfx_text6_px(l), off = px_w > 252 ? px_w - 252 : 0;
        gfx_text6(23, 0, 32, l, COL(WHITE, NAVY), off, 0);
        return;
    }
    if (msg[0] && sys_millis() < msg_until) { gfx_text6(23, 0, 32, msg, COL(msg_err ? RED : GREEN, NAVY), 0, 0); return; }
    msg[0] = 0;
    gfx_text6(23, 0, 32, choosing ? "Up/Down, Enter: this place   ESC: back" : "</> face I time L place O sleep A alarm",
              A_KEYS, 0, 0);
}

static void draw_choose(void)
{
    gfx_text6(3, 1, 30, searching ? "Looking for the place..." : "Which place?", COL(YELLOW, NAVY), 0, 0);
    for (int i = 0; i < nresults; i++)
        gfx_text6(5 + i, 1, 30, results[i], i == rsel ? COL(NAVY, CYAN) : COL(WHITE, NAVY), 0, 0);
}

static void ck_frame(void)
{
    placed = wx_now(&t, !cfg.clock_pc_time);
    wx_get(&w);
    if (searching)
    {
        int s = wx_search_state();
        if (s == WXS_DONE)
        {
            searching = false;
            nresults = wx_search_results(results, 8);
            rsel = 0;
            if (!nresults) { choosing = false; note("No place of that name found", true); }
        }
        else if (s == WXS_FAILED) { searching = choosing = false; note("Place search failed (no internet?)", true); }
    }
    colours_tick();
    gfx_clear(NAVY);
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    if (choosing) draw_choose();
    else
    {
        if (cfg.clock_analog) draw_analog();
        else draw_digital();
        lines();
        if (cfg.clock_spectrum) small_spectrum();
    }
    row23();
}

static void ck_open(void) { phase = -1; calm(); }
static void ck_close(void) { gfx_reset_palette(); inp = 0; choosing = searching = false; }

static void sleep_next(void)
{
    static const int steps[] = { 15, 30, 45, 60, 90, 0 };
    int i = 0;
    while (i < 5 && steps[i] != sleep_min) i++;
    if (steps[i] != sleep_min) i = -1;
    sleep_min = steps[(i + 1) % 6];
    if (sleep_min)
    {
        sleep_end = sys_millis() + (uint32_t)sleep_min * 60000u;
        note("Sleep timer on: the music fades out", false);
    }
    else { audio_set_fade(1000); note("Sleep timer off", false); }
}

static void alarm_set(const char *s)
{
    char b[64];
    u_copy(b, sizeof b, s);
    char *p = u_trim(b);
    if (!*p) { cfg.alarm_on = false; ringing = false; note("Alarm off", false); return; }
    bool wk = false;
    size_t l = strlen(p);
    if (l && (p[l - 1] == 'w' || p[l - 1] == 'W')) { wk = true; p[--l] = 0; u_trim(p); }
    int h = -1, m = -1;
    if (strchr(p, ':') || strchr(p, '.')) sscanf(p, "%d%*[:.]%d", &h, &m);
    else if (strlen(p) >= 3) { int v = atoi(p); h = v / 100; m = v % 100; }
    else h = atoi(p), m = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) { note("The alarm's time: HH:MM", true); return; }
    if (ui_st.state != ST_PLAYING || !ui_st.url[0]) { note("Play the station for the alarm first", true); return; }
    cfg.alarm_on = true;
    cfg.alarm_h = h;
    cfg.alarm_m = m;
    cfg.alarm_weekdays = wk;
    u_copy(cfg.alarm_play, sizeof cfg.alarm_play, ui_st.url);
    u_copy(cfg.alarm_name, sizeof cfg.alarm_name, ui_st.stream ? ui_st.album : ui_st.title);
    note("Alarm set: it plays what plays now", false);
}

static void ck_key(int k)
{
    if (inp)
    {
        size_t n = strlen(inp_text);
        if (k == K_ESC) inp = 0;
        else if (k == K_BS) { if (n) inp_text[n - 1] = 0; }
        else if (k == K_ENTER)
        {
            int what = inp;
            inp = 0;
            if (what == 'A') alarm_set(inp_text);
            else
            {
                char q[64];
                u_copy(q, sizeof q, inp_text);
                u_trim(q);
                if (!q[0]) { wx_use_ip(); note("Place: where the internet connection is", false); }
                else { wx_search(q); searching = choosing = true; nresults = 0; }
            }
        }
        else if (k >= 32 && k < 127 && n < 40) { inp_text[n] = (char)k; inp_text[n + 1] = 0; }
        return;
    }
    if (choosing)
    {
        if (k == K_UP && rsel > 0) rsel--;
        else if (k == K_DOWN && rsel + 1 < nresults) rsel++;
        else if (k == K_ENTER && nresults) { wx_choose(rsel); choosing = false; }
        else if (k == K_ESC || k == K_BS) choosing = searching = false;
        return;
    }
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    switch (u)
    {
    case K_LEFT: case K_RIGHT: case 'V': cfg.clock_analog = !cfg.clock_analog; return;
    case K_ESC: case K_ENTER: case K_BS: case 'C': case 'Z': ui_page(PG_PLAYER); return;
    case 'I':
        cfg.clock_pc_time = !cfg.clock_pc_time;
        note(cfg.clock_pc_time ? "Time: this device's clock and zone" : "Time: the place's zone (internet)", false);
        return;
    case 'T': ui_page(PG_NEWS); return;
    case 'W': ui_page(PG_WX); return;
    case 'O': sleep_next(); return;
    case 'A': inp = 'A'; inp_text[0] = 0; return;
    case 'L': inp = 'L'; inp_text[0] = 0; return;
    case 'X':
    {
        cfg.clock_colours = (cfg.clock_colours + 1) % 7;
        char m[40];
        snprintf(m, sizeof m, "Colours: %s", colnames[cfg.clock_colours]);
        note(m, false);
        phase = -1;
        return;
    }
    case 'B': cfg.clock_spectrum = !cfg.clock_spectrum; return;
    case 'G': return;
    }
    ui_playback_key(k);
}

static void ck_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (inp || choosing) return;
    if (b == MOUSE_LEFT) cfg.clock_analog = !cfg.clock_analog;
    else if (b == MOUSE_RIGHT) ui_page(PG_PLAYER);
}

static void ck_describe(char *out, size_t n)
{
    char wx[60], sal[80];
    wxstr(wx, sizeof wx);
    salstr(sal, sizeof sal);
    snprintf(out, n, "clock %s %02d:%02d:%02d %d-%02d-%02d (%s) place='%s' weather='%s' %s err='%s'",
             cfg.clock_analog ? "analog" : "digital", t.hour, t.min, t.sec, t.year, t.month, t.day, srcstr(),
             w.place, wx, sal, w.error);
}

const page_t page_clock = { ck_open, ck_frame, ck_key, ck_mouse, ck_close, ck_describe };

// ---------------- the sleep timer and the alarm (every frame, any page) ----------------
static int  last_alarm_min = -1;
static uint32_t ring_start;

void clock_alarm_ack(void)
{
    if (!ringing) return;
    ringing = false;
    audio_set_fade(1000);
}

void clock_service_tick(void)
{
    static bool started;
    if (!started) { started = true; wx_start(); }
    uint32_t now = sys_millis();
    if (sleep_min)
    {
        int32_t left = (int32_t)(sleep_end - now);
        if (left <= 0)
        {
            sleep_min = 0;
            audio_stop();
            audio_set_fade(1000);
        }
        else if (left < 30000) audio_set_fade((int)(left / 30));       // the last 30 seconds: fade out
    }
    if (ringing)
    {
        // fade in over a minute
        uint32_t el = now - ring_start;
        if (el < 60000) audio_set_fade(80 + (int)(el * 920 / 60000));
        else audio_set_fade(1000);
    }
    if (!cfg.alarm_on || !cfg.alarm_play[0]) return;
    wx_time tt;
    wx_now(&tt, !cfg.clock_pc_time);
    int m = tt.hour * 60 + tt.min;
    if (m == last_alarm_min) return;
    if (last_alarm_min < 0) { last_alarm_min = m; return; }      // (not at once when PicoAmp starts)
    last_alarm_min = m;
    if (tt.hour != cfg.alarm_h || tt.min != cfg.alarm_m) return;
    if (cfg.alarm_weekdays && tt.wday >= 5) return;
    ringing = true;
    ring_start = now;
    audio_set_fade(80);
    if (u_is_url(cfg.alarm_play)) audio_play_url(cfg.alarm_play, cfg.alarm_name);
    else { const char *f[1] = { cfg.alarm_play }; audio_play_files(f, 1, 0); }
}
