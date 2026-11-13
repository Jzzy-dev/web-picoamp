// ui.c - PICOAMP's main screen, as on the MSX2 (picoamp.asm), cell for cell:
//
//   row 0      PICOAMP   state                                   [x]
//   rows 1-3   big digits (elapsed)   title / artist / format
//   row 4      seek bar
//   row 5      SPECTRUM   EQ FLAT                          LEVEL
//   rows 6-15  24 bars (or the scope)                      L  R
//   row 16     frequency labels                            L  R
//   row 17     a line
//   row 18     the folder's path                           count
//   rows 19-22 the list
//   row 23     |< << > || [] >> >|  SHF REP EQ  VOL  ########
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "gfx.h"
#include "audio.h"
#include "viz.h"
#include "library.h"
#include "config.h"
#include "sys.h"
#include "util.h"
#include "pages.h"
#include "radiodir.h"

#define LIST_ROW 19
#define LIST_N   4
#define GAP      4          // spaces between a ticker's end and its start again
#define HOLD_MS  1000       // a ticker stands still this long first
#define TK_PX_S  45         // and then moves 45 pixels a second

#define A_TITLE COL(WHITE, BLUE)
#define A_NORM  COL(WHITE, NAVY)
#define A_SEL   COL(NAVY, CYAN)
#define A_BAR   COL(GREY_L, BLUE_D)

static const uint8_t pat_track[8]   = { 0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00 };
static const uint8_t pat_knob[8]    = { 0x00,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x00 };
static const uint8_t pat_hline[8]   = { 0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 };
static const uint8_t pat_vol_on[8]  = { 0x00,0x7E,0x7E,0x7E,0x7E,0x7E,0x7E,0x00 };
static const uint8_t pat_vol_off[8] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00 };
static const uint8_t pat_close[8]   = { 0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x00 };
static const uint8_t pat_up[8]      = { 0x00,0x10,0x38,0x7C,0xFE,0x00,0x00,0x00 };
static const uint8_t pat_down[8]    = { 0x00,0x00,0x00,0xFE,0x7C,0x38,0x10,0x00 };
static const uint8_t icons[7][8] = {
    { 0x00,0x46,0x4E,0x5E,0x5E,0x4E,0x46,0x00 },   // |<
    { 0x00,0x12,0x36,0x7E,0x7E,0x36,0x12,0x00 },   // <<
    { 0x00,0x20,0x30,0x38,0x3C,0x38,0x30,0x20 },   // >
    { 0x00,0x6C,0x6C,0x6C,0x6C,0x6C,0x6C,0x00 },   // ||
    { 0x00,0x7C,0x7C,0x7C,0x7C,0x7C,0x7C,0x00 },   // []
    { 0x00,0x48,0x6C,0x7E,0x7E,0x6C,0x48,0x00 },   // >>
    { 0x00,0x62,0x72,0x7A,0x7A,0x72,0x62,0x00 },   // >|
};

// level 0..31 -> lit segments 0..20
static const uint8_t seg_of[32] = { 0,1,1,2,3,3,4,5,5,6,6,7,8,8,9,10,10,11,12,12,13,14,14,15,15,16,17,17,18,19,19,20 };
static const uint8_t label_bars[] = { 0, 6, 12, 17, 21 };

typedef struct
{
    char     text[512];
    int      row, col, w;
    uint8_t  colour;
    bool     act;            // longer than its place
    uint32_t start;          // when its turn (or round) began
    bool     shown;          // drawn this frame
} ticker;

static ticker tk_title, tk_artist, tk_list;
static ticker *const tickers[3] = { &tk_title, &tk_artist, &tk_list };
static ticker *tk_turn;

audio_status ui_st;
viz_data     ui_viz;
#define st  ui_st
#define viz ui_viz
static lib_list     list;
static int          sel, top;
static bool         help_on;
static int          page = PG_PLAYER;
static bool         eq_open;               // G: the equalizer in the picture
static int          eq_sel;                // 0-4 a band, 5 the balance
static int          eq_drag = -1;
static bool         quit_req;
static int          eq_preset;            // EQ_PRESETS: own
static char         msg[256];
static uint8_t      msg_col;
static uint32_t     msg_until;
static unsigned     err_seen;
static int          last_click_row = -1;
static uint32_t     last_click_ms;
static char         playing_place[2048];   // the list the files came from

void ui_message(const char *text, bool error)
{
    u_copy(msg, sizeof msg, text);
    msg_col = error ? COL(RED, NAVY) : COL(GREEN, NAVY);
    msg_until = sys_millis() + 3500;
}

bool ui_quit_requested(void) { return quit_req; }

// ---------------- tickers ----------------
static void ticker_set(ticker *t, const char *text, int row, int col, int w, uint8_t colour)
{
    t->row = row;
    t->col = col;
    t->w = w;
    t->colour = colour;
    t->shown = true;
    if (strcmp(t->text, text))
    {
        u_copy(t->text, sizeof t->text, text);
        t->act = gfx_text6_px(text) > w * 8;
        t->start = sys_millis();
        if (t->act && (!tk_turn || !tk_turn->act || tk_turn == t)) tk_turn = t;
    }
}

static void ticker_give_turn(ticker *t)
{
    tk_turn = t;
    t->start = sys_millis();
}

static void ticker_draw(ticker *t)
{
    int off = 0, cycle = 0;
    if (t->act && t == tk_turn)
    {
        uint32_t el = sys_millis() - t->start;
        int len = gfx_text6_px(t->text) + GAP * 6;
        if (el > HOLD_MS)
        {
            off = (int)((el - HOLD_MS) * TK_PX_S / 1000);
            cycle = len;
            if (off >= len)
            {
                // a round done: the next long text has its turn
                off = 0;
                cycle = 0;
                int i = t == &tk_title ? 0 : t == &tk_artist ? 1 : 2;
                for (int k = 1; k <= 3; k++)
                {
                    ticker *n = tickers[(i + k) % 3];
                    if (n->act && n->shown) { ticker_give_turn(n); break; }
                }
            }
        }
    }
    gfx_text6(t->row, t->col, t->w, t->text, t->colour, off, cycle);
}

// ---------------- the list ----------------
static lib_list     loading;               // a list of the directory coming in
static bool         is_loading, loading_more;
static char         loading_select[2048];
static int          loading_sel;           // ("More stations...": stay where we were)
static bool         searching;             // / or J: the search line
static char         search_text[64];

static void take_list(lib_list *nl, const char *select_target)
{
    lib_free(&list);
    list = *nl;
    memset(nl, 0, sizeof *nl);
    sel = top = 0;
    if (select_target)
        for (int i = 0; i < list.count; i++)
            if (!strcmp(list.e[i].target, select_target)) { sel = i; break; }
    if (sel >= LIST_N) top = sel - LIST_N + 1;
    tk_list.text[0] = 0;
}

static void open_place(const char *place, const char *select_target)
{
    lib_list nl = { 0 };
    if (is_loading) { lib_cancel(); lib_free(&loading); is_loading = false; }
    int r = lib_open(&nl, place);
    if (r == LIB_PENDING)
    {
        // the old list stays until the new one is in
        lib_free(&nl);
        is_loading = true;
        loading_more = false;
        u_copy(loading_select, sizeof loading_select, select_target ? select_target : "");
        return;
    }
    if (r == LIB_FAIL)
    {
        lib_free(&nl);
        ui_message("FOLDER NOT FOUND", true);
        if (!list.e && strcmp(place, "")) open_place("", NULL);
        return;
    }
    take_list(&nl, select_target);
}

static void loading_poll(void)
{
    if (!is_loading) return;
    int r = lib_poll(&loading);
    if (r == LIB_PENDING) return;
    is_loading = false;
    if (r == LIB_FAIL)
    {
        lib_free(&loading);
        char m[200];
        snprintf(m, sizeof m, "%s", lib_error()[0] ? lib_error() : "No answer from the station directory");
        ui_message(m, true);
        return;
    }
    if (loading.count <= 1) { lib_free(&loading); ui_message("No stations found", true); return; }
    if (loading_more)
    {
        int s = loading_sel;
        take_list(&loading, NULL);
        sel = s < list.count ? s : list.count - 1;
        top = sel >= LIST_N ? sel - LIST_N + 1 : 0;
    }
    else take_list(&loading, loading_select[0] ? loading_select : NULL);
}

static void move(int d)
{
    if (!list.count) return;
    sel += d;
    if (sel < 0) sel = 0;
    if (sel >= list.count) sel = list.count - 1;
    if (sel < top) top = sel;
    if (sel >= top + LIST_N) top = sel - LIST_N + 1;
    tk_list.text[0] = 0;            // the chosen entry gets the turn
    tk_turn = &tk_list;
}

static void go_parent(void)
{
    char up[2048], from[2048];
    if (!list.place[0]) return;
    u_copy(from, sizeof from, list.place);
    lib_parent(list.place, up, sizeof up);
    open_place(up, from);
}

static void go_parent(void);

static void activate(void)
{
    if (!list.count) return;
    lib_entry *e = &list.e[sel];
    if (e->type == E_PARENT) { go_parent(); return; }
    if (e->type == E_MORE)
    {
        if (lib_more(&list) == LIB_PENDING) { is_loading = loading_more = true; loading_sel = sel; }
        return;
    }
    if (e->type == E_FOLDER) { open_place(e->target, NULL); return; }
    if (e->type == E_STATION)
    {
        // the list's stations are the play list, as on the MSX (N / P)
        const char **u = malloc(sizeof(char *) * (size_t)list.count), **nm = malloc(sizeof(char *) * (size_t)list.count);
        int n = 0, start = 0;
        for (int i = 0; i < list.count; i++)
            if (list.e[i].type == E_STATION)
            {
                if (i == sel) start = n;
                u[n] = list.e[i].target;
                nm[n++] = list.e[i].name;
            }
        audio_play_stations(u, nm, n, start);
        free(u);
        free(nm);
        rd_click(e->uuid);
        return;
    }
    // a file: the folder's music files are the play list
    int n = 0, start = 0;
    const char **files = malloc(sizeof(char *) * (size_t)list.count);
    for (int i = 0; i < list.count; i++)
        if (list.e[i].type == E_FILE)
        {
            if (i == sel) start = n;
            files[n++] = list.e[i].target;
        }
    audio_play_files(files, n, start);
    free(files);
    u_copy(playing_place, sizeof playing_place, list.place);
}

// the station that is chosen, or the one that plays
static bool chosen_station(char *name, size_t nl, char *url, size_t ul)
{
    if (list.count && list.e[sel].type == E_STATION)
    {
        u_copy(name, nl, list.e[sel].name);
        u_copy(url, ul, list.e[sel].target);
        return true;
    }
    if (st.stream && st.url[0])
    {
        u_copy(name, nl, st.album[0] ? st.album : st.url);
        u_copy(url, ul, st.url);
        return true;
    }
    return false;
}

static void fav_add(void)
{
    char name[256], url[2048];
    if (!chosen_station(name, sizeof name, url, sizeof url)) { ui_message("Choose (or play) a station first", true); return; }
    int r = lib_fav_add(name, url);
    if (r > 0)
    {
        ui_message("Added to the favourites", false);
        if (lib_is_favourites(list.place)) { int s = sel; open_place(list.place, NULL); move(s); }
    }
    else if (r == 0) ui_message("Already a favourite", true);
    else ui_message("Cannot write Favourites.m3u", true);
}

static void fav_del(void)
{
    if (!lib_is_favourites(list.place) || !list.count || list.e[sel].type != E_STATION)
    {
        ui_message("D removes a station in Internet radio/Favourites", true);
        return;
    }
    int r = lib_fav_del(list.e[sel].target);
    if (r > 0)
    {
        int s = sel;
        open_place(list.place, NULL);
        move(s >= list.count ? list.count - 1 : s);
        ui_message("Removed from the favourites", false);
    }
    else if (r == 0) ui_message("Not in Favourites.m3u", true);
    else ui_message("Cannot write Favourites.m3u", true);
}

// ---------------- drawing ----------------
static void draw_static(void)
{
    gfx_clear(NAVY);
    gfx_fill(0, 0, 32, A_TITLE);
    gfx_str(0, 1, "PICOAMP", 9, A_TITLE);
    gfx_cell(0, 31, pat_close, COL(WHITE, RED));
    for (int c = 1; c <= 30; c++) gfx_cell(4, c, pat_track, COL(GREY, NAVY));
    gfx_str(5, 25, "LEVEL", 5, COL(MAGENTA, NAVY));
    gfx_char(16, 26, 'L', COL(CYAN, NAVY));
    gfx_char(16, 29, 'R', COL(CYAN, NAVY));
    for (int c = 0; c < 32; c++) gfx_cell(17, c, pat_hline, COL(BLUE, NAVY));
    gfx_cell(19, 31, pat_up, COL(GREY, NAVY));
    gfx_cell(22, 31, pat_down, COL(GREY, NAVY));
    gfx_fill(23, 0, 32, A_BAR);
    for (int i = 0; i < 7; i++) gfx_cell(23, 1 + i, icons[i], COL(WHITE, BLUE_D));
    gfx_str(23, 9, "SHF REP EQ", 11, A_BAR);
    gfx_str(23, 20, "VOL", 3, A_BAR);
}

static void fmt_format(char *out, size_t n)
{
    if (st.buffering) { if (st.buf_pct) snprintf(out, n, "BUFFERING %d%%", st.buf_pct); else snprintf(out, n, "BUFFERING"); return; }
    out[0] = 0;
    if (!st.bitrate && !st.rate && !st.stream) return;
    size_t o = 0;
    if (st.bitrate) o += (size_t)snprintf(out + o, n - o, "%dK ", st.bitrate);
    if (st.rate && o < n)
    {
        int r10 = st.rate / 10;
        o += (size_t)snprintf(out + o, n - o, "%d.%dKHZ %s ", r10 / 100, (r10 % 100) / 10, st.channels == 1 ? "MO" : "ST");
    }
    if (st.stream && o < n) o += (size_t)snprintf(out + o, n - o, "%s%s ", st.bitrate || st.rate ? "" : "LIVE ", st.tls ? "HTTPS" : "HTTP");
    if (o && o <= n) out[o - 1] = 0;
}

static void draw_info(void)
{
    int sec = st.elapsed, m = sec / 60, s = sec % 60;
    if (m > 99) m = 99;
    uint8_t dc = COL(RED, NAVY);
    gfx_digit(1, 1, m / 10, dc);
    gfx_digit(1, 3, m % 10, dc);
    gfx_colon(1, 5, dc);
    gfx_digit(1, 6, s / 10, dc);
    gfx_digit(1, 8, s % 10, dc);
    // a station without song titles (a browser cannot read them): its name, then what it is
    bool bare = st.stream && !st.title[0] && st.album[0];
    const char *artist = bare ? "Internet radio" : st.artist[0] ? st.artist : st.album;
    ticker_set(&tk_title, bare ? st.album : st.title[0] ? st.title : "NO TRACK", 1, 11, 21, COL(YELLOW, NAVY));
    ticker_set(&tk_artist, artist, 2, 11, 21, COL(CYAN, NAVY));
    char f[64];
    fmt_format(f, sizeof f);
    gfx_text6(3, 11, 21, f, COL(GREEN, NAVY), 0, 0);
    // seek bar: elapsed / total, a stream: how full its buffer is
    int pos = 0;
    if (st.stream) pos = st.buf_pct * 29 / 100;
    else if (st.total) pos = st.elapsed * 29 / st.total;
    if (pos > 29) pos = 29;
    gfx_cell(4, 1 + pos, pat_knob, COL(WHITE, NAVY));
    // the state: for a station its name 3 of every 4 steps of about a second
    char line[300];
    bool name = st.state == ST_PLAYING && st.stream && st.album[0] && ((sys_millis() / 1067) & 3) != 3;
    if (name) u_copy(line, sizeof line, st.album);
    else if (st.state == ST_PLAYING) snprintf(line, sizeof line, "PLAYING %d/%d", st.pl_pos + 1, st.pl_count);
    else u_copy(line, sizeof line, st.state == ST_PAUSED ? "PAUSED" : "STOPPED");
    gfx_str(0, 11, line, 20, A_TITLE);
    // SHF REP: green when on
    gfx_str(23, 9, "SHF", 3, (st.mode & MODE_SHUFFLE) ? COL(GREEN, BLUE_D) : COL(GREY, BLUE_D));
    gfx_str(23, 13, "REP", 3, (st.mode & MODE_REPEAT) ? COL(GREEN, BLUE_D) : COL(GREY, BLUE_D));
    // volume: cell b on when the volume is above 12 * b (muted: red)
    for (int b = 0; b < 8; b++)
    {
        bool on = st.volume > 12 * b;
        gfx_cell(23, 24 + b, on ? pat_vol_on : pat_vol_off,
                 on ? (st.muted ? COL(RED, BLUE_D) : COL(GREEN, BLUE_D)) : COL(GREY, BLUE_D));
    }
}

// one bar of 10 cells, 2 LED segments a cell
static void draw_bar(int col, int level, int pk_level)
{
    int lit = seg_of[level & 31];
    int pk = -1;
    if (pk_level)
    {
        pk = seg_of[pk_level & 31];
        if (pk < lit) pk = -1;
        else if (pk > 19) pk = 19;
    }
    for (int cell = 0; cell < 10; cell++)
    {
        int lo = cell * 2, hi = lo + 1;
        uint8_t pat[8] = { 0 }, cols[8];
        bool lo_on = lit > lo || pk == lo, hi_on = lit > hi || pk == hi;
        if (hi_on) pat[0] = pat[1] = pat[2] = 0x7E;
        if (lo_on) pat[4] = pat[5] = pat[6] = 0x7E;
        uint8_t chi = hi <= 10 ? COL(GREEN, NAVY) : hi <= 15 ? COL(YELLOW, NAVY) : COL(RED, NAVY);
        uint8_t clo = lo <= 10 ? COL(GREEN, NAVY) : lo <= 15 ? COL(YELLOW, NAVY) : COL(RED, NAVY);
        if (pk == hi) chi = COL(WHITE, NAVY);
        if (pk == lo) clo = COL(WHITE, NAVY);
        for (int i = 0; i < 4; i++) cols[i] = chi;
        for (int i = 4; i < 8; i++) cols[i] = clo;
        gfx_cell2(15 - cell, col, pat, cols);
    }
}

static void draw_scope(void)
{
    // a new trace every 80 ms (the MSX draws one at most every 40 ms; on a PC
    // screen that is restless)
    static int8_t snap[VIZ_WAVE];
    static uint32_t snap_ms;
    uint32_t now = sys_millis();
    if (now - snap_ms >= 80) { snap_ms = now; memcpy(snap, viz.scope, sizeof snap); }
    // 192 x 80 at (8, 48): 64 points, 3 pixels apart, joined by vertical lines
    static uint8_t px[80][192];
    memset(px, 0, sizeof px);
    int prev = -1;
    for (int i = 0; i < VIZ_WAVE; i++)
    {
        int y = 40 - (snap[i] * 5) / 8;
        if (snap[i] < 0) y = 40 - (int)((snap[i] * 5 - 7) / 8);      // (the Z80's floor)
        if (y < 0) y = 0;
        if (y > 79) y = 79;
        if (prev < 0) prev = y;
        int x = i * 3, a = prev < y ? prev : y, b = prev < y ? y : prev;
        for (int k = a; k <= b; k++) px[k][x] = 1;
        if (x + 1 < 192) px[y][x + 1] = 1;
        if (x + 2 < 192) px[y][x + 2] = 1;
        prev = y;
    }
    for (int y = 0; y < 80; y++)
    {
        int d2 = 2 * y - 79;                    // |y - 39.5| * 2
        if (d2 < 0) d2 = -d2;
        uint8_t fg = d2 < 44 ? GREEN : d2 < 66 ? YELLOW : RED;
        uint8_t bg = y == 40 ? BLUE_D : NAVY;
        for (int x = 0; x < 192; x++) gfx_fb[48 + y][8 + x] = px[y][x] ? fg : bg;
    }
}

static void freq_label(int bar)
{
    char b[8];
    unsigned hz = viz.freq[bar];
    if (hz < 1000) snprintf(b, sizeof b, "%u", hz);
    else snprintf(b, sizeof b, "%uK", (hz + 500) / 1000);
    gfx_str(16, bar + 1, b, 3, COL(CYAN, NAVY));
}

// ---------------- the equalizer (G) ----------------
static const int8_t band_cell[24] = { -1,-1,-1,-1,0,0,0,-1,1,1,1,-1,2,2,2,-1,3,3,3,-1,4,4,4,-1 };
static const uint8_t knob_v[7] = { 0x3C,0x7E,0xFF,0xFF,0xFF,0x7E,0x3C };
static const uint8_t knob_h[8] = { 0x3C,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x3C };
static const char *const eq_lab[5] = { " 60", "250", " 1K", " 4K", "12K" };
static const char *const eq_hz[5] = { "60 HZ", "250 HZ", "1 KHZ", "4 KHZ", "12 KHZ" };

// the colour of a cell of the picture, in one of its three parts (y 0, 58, 66)
static uint8_t eq_colour(int c, int part_y)
{
    if (part_y >= 66) return c < 3 ? CYAN : eq_sel == 5 ? YELLOW : WHITE;
    if (c < 3) return CYAN;
    int b = band_cell[c];
    if (b < 0) return GREY;
    if (b == eq_sel) return YELLOW;
    if (part_y >= 58) return CYAN;
    return c == 5 + 4 * b ? WHITE : GREY;
}

static void draw_eq(void)
{
    static uint8_t px[80][192];
    memset(px, 0, sizeof px);
    #define VB_TEXT(cell, y, txt) do { uint8_t m_[8]; int n_ = gfx_to_msx(txt, m_, sizeof m_); \
        for (int i_ = 0; i_ < n_; i_++) { const uint8_t *g_ = gfx_glyph(m_[i_]); \
            for (int r_ = 0; r_ < 8; r_++) for (int b_ = 0; b_ < 8; b_++) \
                if (g_[r_] & (0x80 >> b_) && (y) + r_ < 80 && ((cell) + i_) * 8 + b_ < 192) px[(y) + r_][((cell) + i_) * 8 + b_] = 1; } } while (0)
    VB_TEXT(0, 3, "+12");
    VB_TEXT(1, 27, "0");
    VB_TEXT(0, 51, "-12");
    for (int x = 24; x < 184; x++) if ((x & 3) == 0) px[30][x] = 1;          // 0 dB: 0x88 a cell
    VB_TEXT(0, 70, "BAL");
    for (int x = 24; x < 184; x++) px[73][x] = px[74][x] = 1;
    for (int b = 0; b < EQ_BANDS; b++)
    {
        int cc = 5 + 4 * b;
        for (int y = 2; y < 59; y++) px[y][cc * 8 + 3] = px[y][cc * 8 + 4] = 1;
        int top = 27 - 2 * cfg.eq[b];
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 8; k++)
                if (knob_v[r] & (0x80 >> k)) px[top + r][cc * 8 + k] = 1;
        VB_TEXT(cc - 1, 60, eq_lab[b]);
    }
    int bx = 28 + cfg.balance * 148 / 100;
    for (int r = 0; r < 8; r++)
        for (int k = 0; k < 8; k++)
            if (knob_h[r] & (0x80 >> k) && bx + k < 192) px[70 + r][bx + k] = 1;
    #undef VB_TEXT
    for (int y = 0; y < 80; y++)
    {
        int part = y < 58 ? 0 : y < 66 ? 58 : 66;
        for (int x = 0; x < 192; x++)
            gfx_fb[48 + y][8 + x] = px[y][x] ? eq_colour(x / 8, part) : NAVY;
    }
    // row 16: the chosen band and its gain, or the balance
    char line[40];
    if (eq_sel == 5)
    {
        if (cfg.balance == 50) snprintf(line, sizeof line, "BALANCE CENTRE");
        else snprintf(line, sizeof line, "BALANCE %d", cfg.balance);
    }
    else snprintf(line, sizeof line, "%s %s%d DB", eq_hz[eq_sel], cfg.eq[eq_sel] > 0 ? "+" : "", cfg.eq[eq_sel]);
    gfx_str(16, 1, line, 16, COL(WHITE, NAVY));
    gfx_str(16, 17, "G:CLOSE", 8, COL(GREY, NAVY));
}

static void eq_apply(void)
{
    audio_set_eq(cfg.eq);
    eq_preset = eq_find_preset(cfg.eq);
}

static void eq_step(int d)
{
    if (eq_sel == 5)
    {
        int b = cfg.balance + 5 * d;
        cfg.balance = b < 0 ? 0 : b > 100 ? 100 : b;
        audio_set_balance(cfg.balance);
        return;
    }
    int g = cfg.eq[eq_sel] + d;
    if (g > EQ_MAX_DB || g < -EQ_MAX_DB) return;
    cfg.eq[eq_sel] = (int8_t)g;
    eq_apply();
}

// a click or a drag in the picture: x, y in the picture (0..191, 0..79)
static void eq_point(int ax, int ay, bool press)
{
    if (press)
    {
        eq_drag = -1;
        if (ay >= 66 && ax >= 16) eq_drag = 5;
        else if (ay < 58 && ax >= 0 && ax < 192 && band_cell[ax / 8] >= 0) eq_drag = band_cell[ax / 8];
        if (eq_drag < 0) return;
        eq_sel = eq_drag;
    }
    if (eq_drag == 5)
    {
        int b = (ax - 32) * 100 / 148;
        cfg.balance = b < 0 ? 0 : b > 100 ? 100 : b;
        if (cfg.balance > 46 && cfg.balance < 54) cfg.balance = 50;
        audio_set_balance(cfg.balance);
    }
    else if (eq_drag >= 0)
    {
        int g = (30 - ay) / 2;
        if (g > EQ_MAX_DB) g = EQ_MAX_DB;
        if (g < -EQ_MAX_DB) g = -EQ_MAX_DB;
        cfg.eq[eq_drag] = (int8_t)g;
        eq_apply();
    }
}

static void draw_view(void)
{
    gfx_str(5, 1, eq_open ? "EQUALIZER" : cfg.view ? "SCOPE" : "SPECTRUM", 9, COL(MAGENTA, NAVY));
    gfx_str(5, 11, eq_names[eq_preset], 9, eq_open ? COL(YELLOW, NAVY) : COL(GREY, NAVY));
    if (eq_open) draw_eq();
    else if (cfg.view) draw_scope();
    else
    {
        for (int b = 0; b < VIZ_BANDS; b++) draw_bar(1 + b, viz.level[b], viz.peak[b]);
        for (unsigned i = 0; i < sizeof label_bars; i++) freq_label(label_bars[i]);
    }
    draw_bar(26, viz.vu[0], viz.vu[0]);
    draw_bar(29, viz.vu[1], viz.vu[1]);
}

static void draw_list(void)
{
    if (searching)
    {
        char t[100];
        snprintf(t, sizeof t, "SEARCH: %s%s", search_text, (sys_millis() / 400) & 1 ? "_" : " ");
        gfx_fill(18, 0, 32, A_NORM);
        gfx_text6(18, 1, 30, t, COL(WHITE, NAVY), 0, 0);
    }
    else if (is_loading) gfx_text6(18, 1, 30, "LOADING THE LIST...", COL(GREEN, NAVY), 0, 0);
    else if (msg[0] && sys_millis() < msg_until) gfx_text6(18, 1, 30, msg, msg_col, 0, 0);
    else
    {
        msg[0] = 0;
        // a long path: its end, which says where you are
        const char *t = list.title;
        char tail[600];
        int chars = 0;
        for (const char *p = t; *p; p++) if ((*p & 0xC0) != 0x80) chars++;
        if (chars > 31)
        {
            const char *p = t;
            for (int skip = chars - 29; *p && skip; ) { p++; if ((*p & 0xC0) != 0x80) skip--; }
            snprintf(tail, sizeof tail, "..%s", p);
            t = tail;
        }
        gfx_text6(18, 1, 24, t, COL(YELLOW, NAVY), 0, 0);
        char n[16];
        snprintf(n, sizeof n, "%d", list.count);
        gfx_str(18, 25, n, 6, COL(GREY, NAVY));
    }
    tk_list.shown = false;
    for (int r = 0; r < LIST_N; r++)
    {
        int i = top + r, row = LIST_ROW + r;
        if (i >= list.count) { gfx_fill(row, 1, 30, A_NORM); continue; }
        lib_entry *e = &list.e[i];
        char t[300];
        if (e->type == E_FOLDER || e->type == E_PARENT) snprintf(t, sizeof t, "[%s]", e->name);
        else u_copy(t, sizeof t, e->name);
        int w = e->type == E_STATION ? 25 : 30;
        uint8_t c = i == sel ? A_SEL : A_NORM;
        if (i == sel) { ticker_set(&tk_list, t, row, 1, w, c); ticker_draw(&tk_list); }
        else gfx_text6(row, 1, w, t, c, 0, 0);
        if (e->type == E_STATION) gfx_text6(row, 26, 5, e->tls ? " https" : "  http", c, 0, 0);
    }
}

static void draw_help(void)
{
    static const struct { uint8_t c; const char *t; } lines[] = {
        { COL(WHITE, BLUE),  "  PICOAMP - THE KEYS" },
        { A_NORM, "Up/Down    choose     Enter  play/open" },
        { A_NORM, "Left/Right page       BS     parent" },
        { A_NORM, "Space      pause      S      stop" },
        { A_NORM, "N / P      next/prev  , / .  -10s/+10s" },
        { A_NORM, "+ = / - M  vol/mute   H / R  shuffle/rep" },
        { A_NORM, "V          scope      E      EQ preset" },
        { A_NORM, "G          equalizer  Z      full screen" },
        { A_NORM, "C          clock      W      weather" },
        { A_NORM, "T          news       / J    search" },
        { A_NORM, "F          favourite+ D, DEL favourite-" },
        { A_NORM, "Ctrl+Up/Down  move a favourite" },
        { A_NORM, "O          edit the station lists" },
        { COL(CYAN, NAVY), "Page: F11, Alt+Enter or a double click" },
        { COL(CYAN, NAVY), "  outside the picture: full screen," },
        { COL(CYAN, NAVY), "  F2 scaling: fit / whole / stretch," },
        { COL(CYAN, NAVY), "  F3 scan lines; media keys work" },
        { COL(CYAN, NAVY), "Mouse: click, again to play; the right" },
        { COL(CYAN, NAVY), "  button goes back, the wheel scrolls" },
        { COL(CYAN, NAVY), "Drop music or .m3u/.pls files on the" },
        { COL(CYAN, NAVY), "  page: play them, add them" },
        { A_NORM, "" },
        { A_NORM, "" },
        { COL(GREY, NAVY), "press a key" },
    };
    gfx_clear(NAVY);
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++)
    {
        if (i == 0) gfx_fill(0, 0, 32, lines[0].c);
        gfx_text6((int)i, i == 0 ? 0 : 1, i == 0 ? 32 : 31, lines[i].t, lines[i].c, 0, 0);
    }
    // the credits change colour on the beat of the music
    static const uint8_t beat[] = { COL(WHITE, RED), COL(NAVY, ORANGE), COL(NAVY, YELLOW), COL(NAVY, GREEN),
                                    COL(NAVY, CYAN), COL(WHITE, BLUE), COL(WHITE, MAGENTA) };
    static const struct { int col; const char *w; } words[] = { { 11, "by" }, { 14, "JZZY" }, { 19, "&" }, { 21, "CLAUDE" }, { 28, "2026" } };
    // a step round the colours on a beat: the bass jumps up (at most 3 a second)
    static int last_bass, step;
    static uint32_t last_beat;
    int bass = (viz.level[0] + viz.level[1] + viz.level[2] + viz.level[3]) / 4;
    uint32_t now = sys_millis();
    if (bass >= last_bass + 4 && bass > 12 && now - last_beat >= 330) { step++; last_beat = now; }
    last_bass = bass;
    for (unsigned i = 0; i < sizeof words / sizeof words[0]; i++)
        gfx_str(22, words[i].col, words[i].w, (int)strlen(words[i].w), beat[(i + (unsigned)step) % 7]);
}

// ---------------- pages ----------------
static const page_t *const pages[PG_COUNT] = { NULL, &page_full, &page_clock, &page_wx, &page_news };

int ui_page_now(void) { return page; }

void ui_page(int pg)
{
    if (pg == page) return;
    if (page != PG_PLAYER && pages[page]->close) pages[page]->close();
    page = pg;
    gfx_set_size(GFX_W, GFX_H);
    tk_title.text[0] = tk_artist.text[0] = tk_list.text[0] = 0;
    tk_turn = NULL;
    if (page != PG_PLAYER && pages[page]->open) pages[page]->open();
}

const char *ui_now_playing(void)
{
    static char t[800];
    if (st.stream && !st.title[0] && st.album[0]) { u_copy(t, sizeof t, st.album); return t; }
    snprintf(t, sizeof t, "%s%s%s%s%s", st.title[0] ? st.title : "NO TRACK", st.artist[0] ? " - " : "", st.artist,
             st.stream && st.album[0] ? "  . " : "", st.stream ? st.album : "");
    return t;
}

void ui_row0(int row, int col, int width, uint8_t colour)
{
    tk_title.shown = false;
    ticker_set(&tk_title, ui_now_playing(), row, col, width, colour);
    if (!tk_turn || !tk_turn->act) tk_turn = &tk_title;
    ticker_draw(&tk_title);
}

void ui_note(const char *text, bool error) { ui_message(text, error); }
const char *ui_note_text(void) { return (msg[0] && sys_millis() < msg_until) ? msg : ""; }
bool ui_note_error(void) { return msg_col == COL(RED, NAVY); }

void ui_frame(void)
{
    audio_status_get(&st);
    viz_poll(st.state == ST_PLAYING && !st.buffering, &viz);
    clock_service_tick();
    loading_poll();
    if (st.err_gen != err_seen)
    {
        err_seen = st.err_gen;
        if (st.error[0]) ui_message(st.error, true);
    }
    if (help_on) { gfx_set_size(GFX_W, GFX_H); draw_help(); return; }
    if (page != PG_PLAYER) { pages[page]->frame(); return; }
    for (int i = 0; i < 3; i++) tickers[i]->shown = false;
    draw_static();
    draw_info();
    draw_view();
    draw_list();
    ticker_draw(&tk_title);
    ticker_draw(&tk_artist);
    if (tk_turn && !tk_turn->shown) tk_turn = NULL;
    if (!tk_turn || !tk_turn->act)
        for (int i = 0; i < 3; i++) if (tickers[i]->act && tickers[i]->shown) { ticker_give_turn(tickers[i]); break; }
}

// ---------------- input ----------------
static void set_eq_preset(int p)
{
    eq_preset = p % EQ_PRESETS;
    memcpy(cfg.eq, eq_presets[eq_preset], EQ_BANDS);
    audio_set_eq(cfg.eq);
}

void ui_eq_next(void) { set_eq_preset(eq_preset >= EQ_PRESETS ? 0 : eq_preset + 1); }

static void set_mode(int m)
{
    cfg.mode = m;
    audio_set_mode(m);
}

// the keys that work on every page
bool ui_playback_key(int k)
{
    if (k >= 'a' && k <= 'z') k -= 32;
    if (k == ' ' || k == 'S' || k == 'N' || k == 'P' || (k >= K_MEDIA_PLAY && k <= K_MEDIA_PREV)) clock_alarm_ack();
    switch (k)
    {
    case ' ': case K_MEDIA_PLAY: audio_toggle_pause(); return true;
    case 'S': case K_MEDIA_STOP: audio_stop(); return true;
    case 'N': case K_MEDIA_NEXT: audio_next(); return true;
    case 'P': case K_MEDIA_PREV: audio_prev(); return true;
    case '+': case '=': case K_VOL_UP:
        cfg.volume = cfg.volume + 5 > 100 ? 100 : cfg.volume + 5;
        audio_set_volume(cfg.volume);
        return true;
    case '-': case K_VOL_DOWN:
        cfg.volume = cfg.volume < 5 ? 0 : cfg.volume - 5;
        audio_set_volume(cfg.volume);
        return true;
    case 'M': case K_VOL_MUTE:
        cfg.muted = !cfg.muted;
        audio_set_mute(cfg.muted);
        return true;
    case ',': case '<': audio_seek_rel(-10); return true;
    case '.': case '>': audio_seek_rel(10); return true;
    case 'H': set_mode(cfg.mode ^ MODE_SHUFFLE); return true;
    case 'R': set_mode(cfg.mode ^ MODE_REPEAT); return true;
    case 'E': ui_eq_next(); return true;
    case 'Q': quit_req = true; plat_quit(); return true;
    }
    return false;
}

void ui_key(int k)
{
    if (help_on) { help_on = false; return; }
    if (page != PG_PLAYER) { pages[page]->key(k); return; }
    if (searching)
    {
        size_t n = strlen(search_text);
        if (k == K_ESC) searching = false;
        else if (k == K_BS) { if (n) search_text[n - 1] = 0; }
        else if (k == K_ENTER)
        {
            searching = false;
            char t[80];
            u_copy(t, sizeof t, search_text);
            u_trim(t);
            if (t[0])
            {
                char pl[100];
                snprintf(pl, sizeof pl, "rb:search=%s", t);
                open_place(pl, NULL);
            }
        }
        else if (k >= 32 && k < 127 && n < 40) { search_text[n] = (char)k; search_text[n + 1] = 0; }
        return;
    }
    if (k >= 'a' && k <= 'z') k -= 32;
    if (eq_open)
    {
        // the equalizer takes the arrows and 0
        switch (k)
        {
        case K_LEFT:  eq_sel = eq_sel ? eq_sel - 1 : 5; return;
        case K_RIGHT: eq_sel = eq_sel < 5 ? eq_sel + 1 : 0; return;
        case K_UP:    eq_step(1); return;
        case K_DOWN:  eq_step(-1); return;
        case '0':
            if (eq_sel == 5) { cfg.balance = 50; audio_set_balance(50); }
            else { cfg.eq[eq_sel] = 0; eq_apply(); }
            return;
        case 'G': case K_ESC: eq_open = false; return;
        }
    }
    switch (k)
    {
    case K_UP:    move(-1); return;
    case K_DOWN:  move(1); return;
    case K_LEFT: case K_PGUP:  move(-LIST_N); return;
    case K_RIGHT: case K_PGDN: move(LIST_N); return;
    case K_HOME:  move(-list.count); return;
    case K_END:   move(list.count); return;
    case K_ENTER: activate(); return;
    case K_BS:    go_parent(); return;
    case 'V': eq_open = false; cfg.view ^= 1; return;
    case 'G': eq_open = !eq_open; eq_drag = -1; return;
    case 'Z': ui_page(PG_FULL); return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'W': ui_page(PG_WX); return;
    case 'T': ui_page(PG_NEWS); return;
    case 'F': fav_add(); return;
    case 'D': case K_DEL: fav_del(); return;
    case K_CTRL_UP: case K_CTRL_DOWN:
        if (lib_is_favourites(list.place) && list.count && list.e[sel].type == E_STATION)
        {
            char url[2048];
            u_copy(url, sizeof url, list.e[sel].target);
            if (lib_fav_move(url, k == K_CTRL_UP ? -1 : 1)) open_place(list.place, url);
        }
        else ui_message("Ctrl+Up/Down moves a station in Internet radio/Favourites", true);
        return;
    case 'O': plat_open_folder(config_radio_dir()); return;
    case '?': help_on = true; return;
    case '/': case 'J': searching = true; search_text[0] = 0; return;
    case K_ESC:
        if (is_loading) { lib_cancel(); is_loading = false; }
        else plat_minimize();
        return;
    }
    ui_playback_key(k);
}

void ui_mouse(int x, int y, int button)
{
    if (help_on) { if (button == MOUSE_LEFT || button == MOUSE_RIGHT) help_on = false; return; }
    if (page != PG_PLAYER) { if (pages[page]->mouse) pages[page]->mouse(x, y, button); return; }
    if (button == MOUSE_UP) { eq_drag = -1; return; }
    if (button == MOUSE_DRAG)
    {
        if (eq_open && eq_drag >= 0) eq_point(x - 8, y - 48, false);
        return;
    }
    if (button == MOUSE_WHEEL_UP) { move(-1); return; }
    if (button == MOUSE_WHEEL_DOWN) { move(1); return; }
    if (button == MOUSE_RIGHT) { if (eq_open) eq_open = false; else go_parent(); return; }
    if (button != MOUSE_LEFT || x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) return;
    int row = y / 8, col = x / 8;
    if (row == 0 && col == 31) { quit_req = true; plat_quit(); return; }
    if (searching) { searching = false; return; }
    if (eq_open && row >= 6 && row <= 15 && col >= 1 && col <= 24) { eq_point(x - 8, y - 48, true); return; }
    if (eq_open && row == 16 && col >= 17 && col <= 24) { eq_open = false; return; }
    if (row == 4 && col >= 1 && col <= 30)
    {
        if (st.total && st.seekable) audio_seek_to((x - 8) * st.total / 240);
        return;
    }
    if (row == 5 && col >= 1 && col <= 9) { if (eq_open) eq_open = false; else cfg.view ^= 1; return; }
    if (row == 5 && col >= 11 && col <= 19) { ui_eq_next(); return; }
    if (!eq_open && row >= 6 && row <= 16 && col >= 1 && col <= 24) { cfg.view ^= 1; return; }
    if (row >= LIST_ROW && row < LIST_ROW + LIST_N)
    {
        if (col == 31) { move(row == LIST_ROW ? -LIST_N : row == LIST_ROW + LIST_N - 1 ? LIST_N : 0); return; }
        int i = top + row - LIST_ROW;
        if (i >= list.count) return;
        uint32_t now = sys_millis();
        if (i == sel && (last_click_row == i || now - last_click_ms < 500)) activate();
        else move(i - sel);
        last_click_row = i;
        last_click_ms = now;
        return;
    }
    if (row == 23)
    {
        switch (col)
        {
        case 1: audio_prev(); return;
        case 2: audio_seek_rel(-10); return;
        case 3: if (st.state != ST_PLAYING) audio_toggle_pause(); return;
        case 4: if (st.state == ST_PLAYING) audio_toggle_pause(); return;
        case 5: audio_stop(); return;
        case 6: audio_seek_rel(10); return;
        case 7: audio_next(); return;
        case 9: case 10: case 11: set_mode(cfg.mode ^ MODE_SHUFFLE); return;
        case 13: case 14: case 15: set_mode(cfg.mode ^ MODE_REPEAT); return;
        case 17: case 18: eq_open = !eq_open; return;
        case 20: case 21: case 22: ui_playback_key('M'); return;
        default:
            if (col >= 24)
            {
                // a cell of the volume: up to it (the first cell again: 0)
                int v = (col - 24) * 12 + 12;
                if (col == 24 && cfg.volume <= 12) v = 0;
                cfg.volume = v > 100 ? 100 : v;
                if (col == 31) cfg.volume = 100;
                audio_set_volume(cfg.volume);
            }
        }
    }
}

void ui_drop(const char *const *paths, int n)
{
    const char **music = malloc(sizeof(char *) * (size_t)(n + 1));
    int m = 0;
    char note[256] = "";
    for (int i = 0; i < n; i++)
    {
        if (lib_is_music(paths[i])) music[m++] = paths[i];
        else if (u_ends_with_ci(paths[i], ".m3u") || u_ends_with_ci(paths[i], ".pls") || u_ends_with_ci(paths[i], ".m3u8"))
        {
            bool ok = lib_import_list(paths[i], note, sizeof note);
            ui_message(note, !ok);
            if (ok) open_place("radio:", NULL);
        }
        else if (u_ends_with_ci(paths[i], ".opml") || u_ends_with_ci(paths[i], ".xml"))
        {
            ui_message("The news is not in the web version yet", true);
        }
        else if (sys_is_dir(paths[i])) open_place(paths[i], NULL);
    }
    if (m) audio_play_files(music, m, 0);
    free(music);
}

char *ui_describe(bool the_list)
{
    size_t n = 65536, o = 0;
    char *b = malloc(n);
    b[0] = 0;
    if (the_list)
    {
        o += (size_t)snprintf(b + o, n - o, "list %s (%d)%s\n", list.title, list.count, is_loading ? " loading" : "");
        for (int i = 0; i < list.count && o < n - 600; i++)
            o += (size_t)snprintf(b + o, n - o, "  %c%d %s%s%s\n", i == sel ? '>' : ' ', list.e[i].type,
                                  list.e[i].name, list.e[i].type == E_STATION ? " = " : "",
                                  list.e[i].type == E_STATION ? list.e[i].target : "");
    }
    else
    {
        o += (size_t)snprintf(b + o, n - o, "page %d", page);
        if (page != PG_PLAYER && pages[page]->describe) { b[o++] = '\n'; pages[page]->describe(b + o, n - o); }
    }
    return b;
}

// the lists changed outside (the page's editor): the place again, the same entry chosen
void ui_reload(void)
{
    char place[2048], target[2048] = "";
    u_copy(place, sizeof place, list.place);
    if (list.count) u_copy(target, sizeof target, list.e[sel].target);
    open_place(place, target[0] ? target : NULL);
}

void ui_save_state(void)
{
    u_copy(cfg.start_path, sizeof cfg.start_path, list.place);
}

void ui_init(void)
{
    lib_init();
    eq_preset = eq_find_preset(cfg.eq);
    audio_set_volume(cfg.volume);
    audio_set_mute(cfg.muted);
    audio_set_mode(cfg.mode);
    audio_set_eq(cfg.eq);
    audio_set_balance(cfg.balance);
    open_place(cfg.start_path, NULL);
    if (!list.ok) open_place("", NULL);
    (void)playing_place;
}
