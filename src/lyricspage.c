// lyricspage.c - L: the words of what plays (lyrics.c, from LRCLIB).
//
//   row 0      what plays (a ticker)
//   row 1      LYRICS and how they came (synced / not synced)
//   rows 2-21  the words; synced: the line sung now in yellow on row 8, the
//              ones sung grey, the ones to come white; not synced: they scroll
//              through the song (when its length is known), or with the keys
//   row 23     the keys, or a message
//
// A file and another program (Spotify ...) know where the song is; a
// station's song is timed from when its title came, as heard: [ and ] move
// the words' clock half a second back or on (stations send their titles a
// little early or late), 0 puts it right again. A station tuned in during a
// song does not know how far it is: ENTER: SYNC, no line lit; the keys move
// a line onto the band in the middle and Enter makes it the one sung now
// (any time the timing is off). R looks for the words again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "sys.h"
#include "util.h"
#include "lyrics.h"

#define TOP      2
#define ROWS     20
#define NOW_ROW  8              // the line sung now: this row of the page
#define WRAP_PX  240
#define MANUAL_MS 8000          // the keys' scrolling holds this long

typedef struct { int line, off, len; } wrow;

static wrow    *rows;
static int      nrows, rows_cap;
static int      top;            // the first row shown
static int      manual_top = -1 - NOW_ROW;   // (-1 - NOW_ROW: not moved by hand)
static uint32_t manual_until;
static int      offset_ms;      // [ / ]: the words earlier / later
static bool     by_hand;        // synced with Enter (a station's song joined in it is timed then)
static char     song_key[600];  // the song the offset and by_hand are for

// the lines in rows of the page (words wrapped)
static void wrap(void)
{
    nrows = 0;
    int n = lyrics_count();
    for (int i = 0; i < n; i++)
    {
        const char *t = lyrics_line(i, NULL);
        int len = (int)strlen(t), off = 0;
        do
        {
            // as much as fits, cut at a space when it is too long
            char buf[600];
            int take = len - off;
            for (;;)
            {
                if (take > (int)sizeof buf - 1) take = (int)sizeof buf - 1;
                memcpy(buf, t + off, (size_t)take);
                buf[take] = 0;
                if (gfx_text6_px(buf) <= WRAP_PX || take <= 1) break;
                int sp = take - 1;
                while (sp > 0 && buf[sp] != ' ') sp--;
                if (sp > 0) take = sp;
                else { take--; while (take > 1 && (t[off + take] & 0xC0) == 0x80) take--; }   // (not in a UTF-8 character)
            }
            if (nrows == rows_cap)
            {
                int c = rows_cap ? rows_cap * 2 : 128;
                wrow *nr = realloc(rows, sizeof *nr * (size_t)c);
                if (!nr) return;
                rows = nr;
                rows_cap = c;
            }
            rows[nrows].line = i;
            rows[nrows].off = off;
            rows[nrows].len = take;
            nrows++;
            off += take;
            while (off < len && t[off] == ' ') off++;
        } while (off < len);
    }
}

// what plays now: asked for (the album only of a file or a program: a station's "album" is its name)
static void follow(void)
{
    const audio_status *s = &ui_st;
    if (s->state == ST_STOPPED && !s->title[0]) { lyrics_want("", "", "", 0); return; }
    lyrics_want(s->artist, s->title, s->stream ? "" : s->album, s->stream ? 0 : s->total);
}

static int now_ms(void) { return ui_st.elapsed_ms + offset_ms; }

// a new song: its own timing
static void song_check(void)
{
    char k[sizeof song_key];
    snprintf(k, sizeof k, "%s\x1f%s", ui_st.artist, ui_st.title);
    if (strcmp(k, song_key)) { u_copy(song_key, sizeof song_key, k); offset_ms = 0; by_hand = false; manual_top = -1 - NOW_ROW; }
}

// not timed: a station's song joined in it, not synced with Enter yet
static bool untimed(void) { return ui_st.untimed && !by_hand; }

static void ly_open(void) { manual_top = -1 - NOW_ROW; }

static void ly_frame(void)
{
    follow();
    song_check();
    int state = lyrics_state();
    bool unt = state == LY_SYNCED && untimed();
    gfx_clear(NAVY);
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    gfx_fill(1, 0, 32, COL(WHITE, BLUE_D));
    gfx_text6(1, 0, 10, " LYRICS", COL(YELLOW, BLUE_D), 0, 0);
    const char *how = state == LY_SYNCED ? "SYNCED" : state == LY_PLAIN ? "NOT SYNCED" : "";
    char right[64];
    if (unt) u_copy(right, sizeof right, "ENTER: SYNC");
    else if (state == LY_SYNCED && offset_ms) snprintf(right, sizeof right, "%s  %+d.%dS", how, offset_ms / 1000, abs(offset_ms) % 1000 / 100);
    else u_copy(right, sizeof right, how);
    gfx_text6(1, 10, 22, right, COL(CYAN, BLUE_D), 0, 0);

    const char *msg = NULL;
    switch (state)
    {
    case LY_NONE:         msg = "Nothing plays"; break;
    case LY_LOADING:      msg = "Looking for the words..."; break;
    case LY_INSTRUMENTAL: msg = "Instrumental: no words"; break;
    case LY_NOT_FOUND:    msg = "LRCLIB has no words for this song"; break;
    case LY_FAILED:       msg = lyrics_error(); break;
    }
    const char *note = ui_note_text();
    const char *keys = unt ? "Up/Down: the line sung, Enter: sync  R again"
                     : state == LY_SYNCED ? "Enter sync  [ ] -/+  Up/Down  R again"
                     : state == LY_PLAIN ? "Up/Down scroll  R again  L back" : "R look again  L or ESC back";
    gfx_text6(23, 0, 32, note[0] ? note : keys, note[0] && ui_note_error() ? COL(RED, NAVY) : COL(GREY, NAVY), 0, 0);
    gfx_text6(22, 0, 32, "Words: LRCLIB.net", COL(GREY_D, NAVY), 0, 0);
    if (msg)
    {
        int w = gfx_text6_px(msg) / 6;
        gfx_text6(10, w < 42 ? (42 - w) * 6 / 16 : 0, 32, msg, COL(WHITE, NAVY), 0, 0);
        return;
    }

    wrap();
    int sung = state == LY_SYNCED && !unt ? lyrics_at(now_ms()) : -1;
    uint32_t t = sys_millis();
    if (unt && manual_top < -NOW_ROW) { manual_top = -NOW_ROW; manual_until = t + 0x7FFFFFFFu; }
    bool manual = manual_top >= -NOW_ROW && (unt || (int32_t)(t - manual_until) < 0);
    if (manual) top = manual_top;
    else
    {
        manual_top = -1 - NOW_ROW;
        if (state == LY_SYNCED)
        {
            // the line sung now on NOW_ROW (before the first: the first there)
            int first = 0;
            for (int r = 0; r < nrows; r++) if (rows[r].line == (sung < 0 ? 0 : sung)) { first = r; break; }
            top = first - NOW_ROW;
        }
        else if (ui_st.total > 0)
            top = (int)((long long)(nrows - ROWS / 2) * ui_st.elapsed_ms / (ui_st.total * 1000LL)) - ROWS / 4;
        else top = top < 0 ? 0 : top;
    }
    if (top > nrows - 1) top = nrows - 1;
    if (state != LY_SYNCED && top > nrows - ROWS) top = nrows - ROWS;
    if (top < -NOW_ROW) top = -NOW_ROW;
    if (state != LY_SYNCED && top < 0) top = 0;
    for (int r = 0; r < ROWS; r++)
    {
        int i = top + r;
        if (i < 0 || i >= nrows) continue;
        char buf[600];
        const char *l = lyrics_line(rows[i].line, NULL);
        int len = rows[i].len < (int)sizeof buf - 1 ? rows[i].len : (int)sizeof buf - 1;
        memcpy(buf, l + rows[i].off, (size_t)len);
        buf[len] = 0;
        // centred: the line sung now yellow on a band, the ones sung grey
        uint8_t fg = WHITE, bg = NAVY;
        if (state == LY_SYNCED)
        {
            if (rows[i].line == sung) { fg = YELLOW; bg = BLUE_D; }
            else if (rows[i].line < sung) fg = GREY;
            if (manual && r == NOW_ROW) bg = BLUE_D;        // (the line Enter syncs to)
        }
        if (bg != NAVY) gfx_fill(TOP + r, 0, 32, COL(fg, bg));
        if (!buf[0] && state == LY_SYNCED && rows[i].line == sung) u_copy(buf, sizeof buf, "...");
        int px = gfx_text6_px(buf);
        gfx_text6_at(px < GFX_W ? (GFX_W - px) / 2 : 0, (TOP + r) * 8, buf, fg, bg, GFX_W);
    }
}

static void scroll(int d)
{
    if (nrows <= 0) return;
    int base = manual_top >= -NOW_ROW ? manual_top : top;
    base += d;
    int lo = lyrics_state() == LY_SYNCED ? -NOW_ROW : 0;
    if (base > nrows - 1) base = nrows - 1;
    if (base < lo) base = lo;
    manual_top = base;
    manual_until = sys_millis() + (untimed() ? 0x7FFFFFFFu : MANUAL_MS);
}

// Enter: the line on the band in the middle is sung now
static bool sync_here(void)
{
    if (lyrics_state() != LY_SYNCED || nrows <= 0) return false;
    int r = top + NOW_ROW;
    if (r < 0) r = 0;
    if (r >= nrows) r = nrows - 1;
    int ms = -1;
    lyrics_line(rows[r].line, &ms);
    if (ms < 0) return false;
    offset_ms = ms - ui_st.elapsed_ms;
    by_hand = true;
    manual_top = -1 - NOW_ROW;
    return true;
}

static void ly_key(int k)
{
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    switch (u)
    {
    case K_ENTER: if (sync_here()) return; ui_page(PG_PLAYER); return;
    case K_ESC: case K_BS: case 'L': ui_page(PG_PLAYER); return;
    case 'R': lyrics_again(); manual_top = -1 - NOW_ROW; return;
    case 'Z': ui_page(PG_FULL); return;
    case K_UP: scroll(-1); return;
    case K_DOWN: scroll(1); return;
    case K_PGUP: case K_LEFT: scroll(-ROWS / 2); return;
    case K_PGDN: case K_RIGHT: scroll(ROWS / 2); return;
    case K_HOME: manual_top = -1 - NOW_ROW; return;
    case '[': offset_ms -= 500; return;
    case ']': offset_ms += 500; return;
    case '0': offset_ms = 0; return;
    case 'G': case 'V': return;
    }
    ui_playback_key(k);
}

static void ly_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (b == MOUSE_RIGHT) ui_page(PG_PLAYER);
    else if (b == MOUSE_WHEEL_UP) scroll(-2);
    else if (b == MOUSE_WHEEL_DOWN) scroll(2);
}

static void ly_describe(char *out, size_t n)
{
    follow();
    int st = lyrics_state(), at = lyrics_at(now_ms());
    snprintf(out, n, "lyrics state=%s lines=%d at=%d line='%s'",
             st == LY_SYNCED ? "synced" : st == LY_PLAIN ? "plain" : st == LY_LOADING ? "loading" : st == LY_INSTRUMENTAL ? "instrumental"
             : st == LY_NOT_FOUND ? "none" : st == LY_FAILED ? "failed" : "nothing",
             lyrics_count(), at, at >= 0 ? lyrics_line(at, NULL) : "");
}

const page_t page_lyrics = { ly_open, ly_frame, ly_key, ly_mouse, NULL, ly_describe };
