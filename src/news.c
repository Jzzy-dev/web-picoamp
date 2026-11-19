// news.c - T: PICOAMP's news reader (news.asm), in the MSX2's 80-column text
// mode (TEXT 2: 512 x 212, 26 rows of 6 x 8 characters, 16 pixels of border).
//
// The headlines of a feed, one a line, the chosen one's summary under them;
// a story with its title and text, scrolled, and its full article (the
// feed's own text, or the paragraphs of the story's web page); My feeds beside
// the catalogue of feeds by topic. Row 0: what plays, and the time.
//
// Keys: headlines: Up/Down, Left/Right a page, Enter the story, 1-9 a feed of
// My feeds, F My feeds, S the order. A story: Up/Down a line, Left/Right a
// page, Enter the full article / the summary, N/P the next/previous story,
// ESC the headlines. My feeds: Left/Right the side, Enter read (left) / add
// (right), D or DEL remove, U/J move, A a feed by its address. C clock,
// W weather, T or ESC (headlines) the player; the playback keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "newsfeed.h"
#include "config.h"
#include "sys.h"
#include "util.h"

#define COLS     80
#define ROWS     26
#define NL_ROWS  17                 // headlines (rows 2-18)
#define NS_ROWS  18                 // story lines (rows 5-22)
#define NF_ROWS  20                 // My feeds / the catalogue (rows 3-22)
#define C_TEXT   WHITE
#define C_BAR_FG NAVY
#define C_BAR_BG CYAN

enum { V_LIST, V_STORY, V_FEEDS, V_INFO };

static int    view = V_FEEDS;
static bool   opened;
static int    feed_no = -1;         // the feed of My feeds read now
static char   feed_name[200];
static int    sel, top;             // headlines
static int    story, s_top;         // a story
static bool   s_full;               // the full article asked for
static int    f_side;               // My feeds: 0 left, 1 right
static int    f_sel, f_top, c_sel, c_top, c_topic = -1;
static char   msg[160];
static uint32_t msg_until;
static bool   typing;               // A: a feed's address
static char   typed[600];
static char   pending_add[600];     // an address being tried
static bool   adding;
static bool   ext;                  // the headlines are the markets' (ESC or BS: back there)

static void note(const char *m) { u_copy(msg, sizeof msg, m); msg_until = sys_millis() + 3500; }

// ---------------- text mode drawing ----------------
static void tx(int row, int col, const char *s, uint8_t fg, int bg, int max)
{
    if (row < 0 || row >= ROWS) return;
    if (max <= 0) max = COLS - col;
    gfx_text6_at(16 + 6 * col, row * 8, s, fg, bg, max * 6);
}

static void tx_bar(int row, uint8_t bg)
{
    gfx_rect(16, row * 8, COLS * 6, 8, bg);
}

static void tx_hline(int row)
{
    gfx_hline(16, row * 8 + 3, COLS * 6, C_TEXT);
}

static void tx_right(int row, int end_col, const char *s, uint8_t fg, int bg)
{
    int n = gfx_text6_px(s) / 6;
    tx(row, end_col - n + 1, s, fg, bg, n);
}

// a text wrapped into lines of width (paragraphs: '\n'; blank: a line between them)
typedef struct { char **l; int n; } lines_t;

static void lines_free(lines_t *L)
{
    for (int i = 0; i < L->n; i++) free(L->l[i]);
    free(L->l);
    L->l = NULL;
    L->n = 0;
}

static void lines_add(lines_t *L, const char *s, size_t n)
{
    if (L->n % 64 == 0) L->l = realloc(L->l, sizeof *L->l * (size_t)(L->n + 64));
    L->l[L->n] = malloc(n + 1);
    memcpy(L->l[L->n], s, n);
    L->l[L->n][n] = 0;
    L->n++;
}

// characters (not bytes) of UTF-8
static int u8len(const char *s, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++) if ((s[i] & 0xC0) != 0x80) c++;
    return c;
}

static void wrap(lines_t *L, const char *text, int width, bool blank)
{
    const char *p = text;
    bool first = true;
    while (*p)
    {
        const char *e = strchr(p, '\n');
        size_t plen = e ? (size_t)(e - p) : strlen(p);
        if (plen)
        {
            if (!first && blank) lines_add(L, "", 0);
            first = false;
            const char *q = p, *end = p + plen;
            while (q < end)
            {
                while (q < end && *q == ' ') q++;
                if (q >= end) break;
                // as many words as fit
                const char *best = NULL, *r = q;
                int chars = 0;
                while (r < end)
                {
                    const char *w = r;
                    while (w < end && *w != ' ') w++;
                    int wl = u8len(r, (size_t)(w - r));
                    if (chars + wl > width)
                    {
                        if (!best) { // a word longer than the line: cut it
                            int k = 0;
                            const char *c = r;
                            while (c < end && k < width - chars) { c++; while (c < end && (*c & 0xC0) == 0x80) c++; k++; }
                            best = c;
                        }
                        break;
                    }
                    chars += wl;
                    best = w;
                    r = w;
                    if (r < end && *r == ' ') { r++; chars++; }
                }
                if (!best || best == q) best = end;
                lines_add(L, q, (size_t)(best - q));
                q = best;
            }
        }
        p = e ? e + 1 : p + plen;
    }
}

// ---------------- row 0: what plays, the time ----------------
static void row0(void)
{
    static uint32_t t0;
    static char last[800];
    const char *np = ui_now_playing();
    if (strcmp(last, np)) { u_copy(last, sizeof last, np); t0 = sys_millis(); }
    // a note, then the text, moving a character at a time when it is long
    static const uint8_t note8[8] = { 0x18, 0x14, 0x12, 0x10, 0x70, 0xF0, 0x60, 0x00 };
    for (int y = 0; y < 8; y++)
        for (int b = 0; b < 6; b++) if (note8[y] & (0x80 >> b)) gfx_pset(16 + b, y, C_TEXT);
    int room = 60;
    int n = u8len(np, strlen(np));
    char shown[800];
    if (n <= room) u_copy(shown, sizeof shown, np);
    else
    {
        uint32_t el = sys_millis() - t0;
        int off = el < 1000 ? 0 : (int)((el - 1000) / 150) % (n + 3);
        char loop[1700];
        snprintf(loop, sizeof loop, "%s   %s", np, np);
        const char *p = loop;
        for (int k = 0; k < off && *p; k++) { p++; while ((*p & 0xC0) == 0x80) p++; }
        u_copy(shown, sizeof shown, p);
    }
    tx(0, 2, shown, C_TEXT, -1, room);
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    static const char *const wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char t[40];
    snprintf(t, sizeof t, "%s %d %s %02d:%02d", wd[tm.tm_wday], tm.tm_mday, mo[tm.tm_mon], tm.tm_hour, tm.tm_min);
    tx_right(0, 79, t, C_TEXT, -1);
}

static void keylines(const char *k)
{
    if (typing)
    {
        char l[700];
        snprintf(l, sizeof l, " Address of the feed: %s%s", typed, (sys_millis() / 400) & 1 ? "_" : " ");
        int n = u8len(l, strlen(l));
        const char *p = l;
        if (n > 79) { for (int s = n - 79; s > 0 && *p; s--) { p++; while ((*p & 0xC0) == 0x80) p++; } }
        tx(24, 0, p, C_TEXT, -1, 80);
    }
    else if (msg[0] && sys_millis() < msg_until) tx(24, 0, msg, YELLOW, -1, 80);
    else { msg[0] = 0; tx(24, 0, k, C_TEXT, -1, 80); }
    tx(25, 0, " C clock   W weather   T the player   I info", C_TEXT, -1, 80);
}

// ---------------- the headlines ----------------
static void open_feed(int i)
{
    nf_feed *f;
    int n = nf_favs(&f);
    if (i < 0 || i >= n) return;
    feed_no = i;
    ext = false;
    u_copy(feed_name, sizeof feed_name, f[i].name);
    nf_fetch(f[i].url);
    sel = top = 0;
    view = V_LIST;
}

static void draw_list(void)
{
    int n;
    nf_item *it = nf_items(&n);
    int st = nf_state();
    tx_bar(1, C_BAR_BG);
    tx(1, 1, feed_name, C_BAR_FG, -1, 50);
    if (st == NF_BUSY) { tx(10, 2, "Fetching the stories...", C_TEXT, -1, 0); keylines(""); return; }
    if (st == NF_FAILED)
    {
        char e[300];
        snprintf(e, sizeof e, "The feed did not come (%s)", nf_error());
        tx(10, 2, e, C_TEXT, -1, 0);
        keylines(" Enter read  Up/Down choose  Left/Right page  1-9 a feed  F My feeds  S sort");
        return;
    }
    if (!n) { tx(10, 2, "No stories in this feed", C_TEXT, -1, 0); keylines(" 1-9 a feed  F My feeds"); return; }
    if (sel >= n) sel = n - 1;
    if (sel < top) top = sel;
    if (sel >= top + NL_ROWS) top = sel - NL_ROWS + 1;
    char h[60];
    snprintf(h, sizeof h, "Headlines %d-%d of %d", top + 1, top + NL_ROWS < n ? top + NL_ROWS : n, n);
    tx_right(1, 78, h, C_BAR_FG, -1);
    for (int r = 0; r < NL_ROWS && top + r < n; r++)
    {
        int i = top + r;
        bool s = i == sel;
        if (s) tx_bar(2 + r, C_BAR_BG);
        char d[40];
        nf_fmt_date(&it[i], d, sizeof d, false);
        uint8_t fg = s ? C_BAR_FG : C_TEXT;
        tx(2 + r, 1, d, fg, -1, 6);
        tx(2 + r, 8, it[i].title, fg, -1, 71);
    }
    tx_hline(19);
    lines_t L = { 0 };
    wrap(&L, it[sel].text, 78, false);
    for (int k = 0; k < 3 && k < L.n; k++) tx(20 + k, 1, L.l[k], C_TEXT, -1, 78);
    lines_free(&L);
    tx_hline(23);
    keylines(" Enter read  Up/Down choose  Left/Right page  1-9 a feed  F My feeds  S sort");
}

// ---------------- a story ----------------
static lines_t s_lines, s_title;
static int     s_built = -1, s_built_state = -1;
static size_t  s_built_len;

static void story_build(void)
{
    int n;
    nf_item *it = nf_items(&n);
    if (story < 0 || story >= n) return;
    int ast = s_full ? nf_article_state() : NF_IDLE;
    const char *text = s_full && ast == NF_DONE ? nf_article_text() : it[story].text;
    size_t tl = strlen(text);
    if (s_built == story && s_built_state == ast && s_built_len == tl) return;
    lines_free(&s_lines);
    lines_free(&s_title);
    wrap(&s_title, it[story].title, 78, false);
    wrap(&s_lines, text[0] ? text : "(no text)", 78, true);
    if (s_built != story) s_top = 0;
    s_built = story;
    s_built_state = ast;
    s_built_len = tl;
}

static void draw_story(void)
{
    int n;
    nf_item *it = nf_items(&n);
    if (story >= n) { view = V_LIST; return; }
    story_build();
    tx_bar(1, C_BAR_BG);
    char h[300], d[60];
    snprintf(h, sizeof h, "%.40s  |  story %d of %d", feed_name, story + 1, n);
    tx(1, 1, h, C_BAR_FG, -1, 58);
    nf_fmt_date(&it[story], d, sizeof d, true);
    tx_right(1, 78, d, C_BAR_FG, -1);
    for (int k = 0; k < 2 && k < s_title.n; k++) tx(2 + k, 1, s_title.l[k], C_TEXT, -1, 78);
    tx_hline(4);
    int maxtop = s_lines.n > NS_ROWS ? s_lines.n - NS_ROWS : 0;
    if (s_top > maxtop) s_top = maxtop;
    if (s_top < 0) s_top = 0;
    for (int r = 0; r < NS_ROWS && s_top + r < s_lines.n; r++) tx(5 + r, 1, s_lines.l[s_top + r], C_TEXT, -1, 78);
    tx_bar(23, C_BAR_BG);
    const char *what = " Summary   (Enter: the full article)";
    char buf[100];
    if (s_full)
    {
        int ast = nf_article_state();
        if (ast == NF_DONE) what = " The full article";
        else if (ast == NF_BUSY)
        {
            snprintf(buf, sizeof buf, " Fetching the full article... %u KB", (unsigned)(nf_article_bytes() / 1024));
            what = buf;
        }
        else what = " No full article from the site: the summary";
    }
    tx(23, 0, what, C_BAR_FG, -1, 50);
    snprintf(h, sizeof h, "lines %d-%d of %d", s_lines.n ? s_top + 1 : 0, s_top + NS_ROWS < s_lines.n ? s_top + NS_ROWS : s_lines.n, s_lines.n);
    tx_right(23, 78, h, C_BAR_FG, -1);
    keylines(" Up/Down scroll  Left/Right page  Enter full article/summary  N/P next/prev  ESC");
}

static void open_story(int i)
{
    int n;
    nf_items(&n);
    if (i < 0 || i >= n) return;
    story = i;
    s_full = false;
    s_top = 0;
    s_built = -1;
    view = V_STORY;
}

// ---------------- My feeds and the catalogue ----------------
static int cat_count(int topic)
{
    int k = 0;
    for (int i = 0; nf_catalogue[i].name; i++) if (nf_catalogue[i].topic == topic) k++;
    return k;
}

static const nf_cat *cat_at(int topic, int idx)
{
    for (int i = 0; nf_catalogue[i].name; i++)
        if (nf_catalogue[i].topic == topic && idx-- == 0) return &nf_catalogue[i];
    return NULL;
}

static int topic_count(void) { int n = 0; while (nf_topics[n]) n++; return n; }

static void draw_feeds(void)
{
    nf_feed *f;
    int n = nf_favs(&f);
    tx_bar(1, C_BAR_BG);
    char h[100];
    snprintf(h, sizeof h, "My feeds: %d", n);
    tx(1, 1, h, C_BAR_FG, -1, 38);
    snprintf(h, sizeof h, "Add a feed: %s", c_topic < 0 ? "choose a topic" : nf_topics[c_topic]);
    tx(1, 41, h, C_BAR_FG, -1, 38);
    // left: My feeds
    if (f_sel >= n) f_sel = n - 1;
    if (f_sel < 0) f_sel = 0;
    if (f_sel < f_top) f_top = f_sel;
    if (f_sel >= f_top + NF_ROWS) f_top = f_sel - NF_ROWS + 1;
    for (int r = 0; r < NF_ROWS && f_top + r < n; r++)
    {
        int i = f_top + r;
        bool s = i == f_sel;
        char l[200];
        snprintf(l, sizeof l, "%2d %s", i + 1, f[i].name);
        if (s && f_side == 0) { gfx_rect(16, (3 + r) * 8, 39 * 6, 8, C_BAR_BG); tx(3 + r, 1, l, C_BAR_FG, -1, 37); }
        else { if (s) tx(3 + r, 0, ">", C_TEXT, -1, 1); tx(3 + r, 1, l, C_TEXT, -1, 37); }
    }
    // the line between
    gfx_rect(16 + 39 * 6 + 2, 2 * 8, 1, 21 * 8, C_TEXT);
    // right: the topics, or a topic's feeds
    int cn = c_topic < 0 ? topic_count() : cat_count(c_topic);
    if (c_sel >= cn) c_sel = cn - 1;
    if (c_sel < 0) c_sel = 0;
    if (c_sel < c_top) c_top = c_sel;
    if (c_sel >= c_top + NF_ROWS) c_top = c_sel - NF_ROWS + 1;
    for (int r = 0; r < NF_ROWS && c_top + r < cn; r++)
    {
        int i = c_top + r;
        bool s = i == c_sel && f_side == 1;
        char l[200];
        if (c_topic < 0) snprintf(l, sizeof l, " %s", nf_topics[i]);
        else
        {
            const nf_cat *c = cat_at(c_topic, i);
            snprintf(l, sizeof l, "%c%s", nf_fav_has(c->url) ? '*' : ' ', c->name);
        }
        if (s) gfx_rect(16 + 40 * 6, (3 + r) * 8, 40 * 6, 8, C_BAR_BG);
        tx(3 + r, 40, l, s ? C_BAR_FG : C_TEXT, -1, 39);
    }
    tx_hline(23);
    keylines(" Left/Right side  Enter read/add  D remove  U/J move  A address  BS back  ESC headlines");
}

// ---------------- info ----------------
static void draw_info(void)
{
    tx_bar(1, C_BAR_BG);
    tx(1, 1, "The news", C_BAR_FG, -1, 0);
    int n;
    nf_items(&n);
    static const char *const sn[] = { "idle", "fetching", "done", "failed" };
    char l[400];
    snprintf(l, sizeof l, "Feed: %s", feed_name[0] ? feed_name : "-");
    tx(3, 1, l, C_TEXT, -1, 78);
    snprintf(l, sizeof l, "State: %s, %d stories, error: %s", sn[nf_state()], n, nf_error()[0] ? nf_error() : "-");
    tx(4, 1, l, C_TEXT, -1, 78);
    snprintf(l, sizeof l, "Title in the feed: %s", nf_feed_title());
    tx(5, 1, l, C_TEXT, -1, 78);
    snprintf(l, sizeof l, "Order: %s", cfg.news_by_feed ? "as the feed has them" : "newest first");
    tx(6, 1, l, C_TEXT, -1, 78);
    snprintf(l, sizeof l, "My feeds: %s%cMyFeeds.opml", sys_config_dir(),
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
    );
    tx(8, 1, l, C_TEXT, -1, 78);
    tx(9, 1, "(an OPML file: other feed readers import and export it; drop one on the window to add its feeds)", C_TEXT, -1, 78);
    keylines(" ESC, Enter or BS: back");
}

// ---------------- the page ----------------
static int last_view = V_FEEDS;

static void nw_open(void)
{
    gfx_set_size(512, 212);
    if (!opened) { opened = true; nf_sort(!cfg.news_by_feed); view = V_FEEDS; }
}

static void nw_close(void) { gfx_set_size(GFX_W, GFX_H); typing = false; }

void news_open_market(const char *name)
{
    if (!opened) { opened = true; nf_sort(!cfg.news_by_feed); }
    ext = true;
    feed_no = -2;
    snprintf(feed_name, sizeof feed_name, "Market news: %s", name);
    sel = top = 0;
    view = V_LIST;
}

void news_feed_taken(void)
{
    if (feed_no == -1) return;
    feed_no = -1;
    ext = false;
    if (view == V_LIST || view == V_STORY) view = V_FEEDS;
}

static void try_add_poll(void)
{
    if (!adding) return;
    int st = nf_state();
    if (st == NF_BUSY) return;
    adding = false;
    int n;
    nf_items(&n);
    if (st == NF_DONE && n)
    {
        const char *t = nf_feed_title();
        if (nf_fav_add(t[0] ? t : pending_add, pending_add)) note(" Added to My feeds");
        else note(" That one is in My feeds already");
    }
    else note(" No feed at that address");
    view = V_FEEDS;
}

static void nw_frame(void)
{
    gfx_set_size(512, 212);
    gfx_clear(NAVY);
    try_add_poll();
    row0();
    switch (view)
    {
    case V_LIST: draw_list(); break;
    case V_STORY: draw_story(); break;
    case V_FEEDS:
        if (adding) { tx_bar(1, C_BAR_BG); tx(1, 1, "Trying the address...", C_BAR_FG, -1, 0); keylines(""); }
        else draw_feeds();
        break;
    case V_INFO: draw_info(); break;
    }
}

static void feeds_key(int k, int u)
{
    nf_feed *f;
    int n = nf_favs(&f);
    switch (u)
    {
    case K_LEFT: case K_RIGHT: case '\t': f_side ^= 1; return;
    case K_UP: if (f_side) c_sel--; else f_sel--; return;
    case K_DOWN: if (f_side) c_sel++; else f_sel++; return;
    case K_PGUP: if (f_side) c_sel -= NF_ROWS; else f_sel -= NF_ROWS; return;
    case K_PGDN: if (f_side) c_sel += NF_ROWS; else f_sel += NF_ROWS; return;
    case K_HOME: if (f_side) c_sel = 0; else f_sel = 0; return;
    case K_END: if (f_side) c_sel = 1 << 20; else f_sel = n - 1; return;
    case K_ENTER:
        if (!f_side) { if (n) open_feed(f_sel); return; }
        if (c_topic < 0) { c_topic = c_sel; c_sel = c_top = 0; return; }
        {
            const nf_cat *c = cat_at(c_topic, c_sel);
            if (c) note(nf_fav_add(c->name, c->url) ? " Added to My feeds" : " That one is in My feeds already");
        }
        return;
    case K_BS: if (f_side && c_topic >= 0) { c_sel = c_topic; c_topic = -1; } else if (feed_no >= 0) view = V_LIST; return;
    case K_ESC: if (feed_no >= 0) view = V_LIST; else ui_page(PG_PLAYER); return;
    case 'D': case K_DEL:
        if (!f_side && n) { nf_fav_del(f_sel); note(" Removed from My feeds"); if (feed_no == f_sel) feed_no = -1; }
        return;
    case 'U': if (!f_side && f_sel > 0) { nf_fav_move(f_sel, -1); f_sel--; } return;
    case 'J': if (!f_side && f_sel + 1 < n) { nf_fav_move(f_sel, 1); f_sel++; } return;
    case 'A': typing = true; typed[0] = 0; return;
    }
    (void)k;
}

static void nw_key(int k)
{
    if (typing)
    {
        size_t n = strlen(typed);
        if (k == K_ESC) typing = false;
        else if (k == K_BS) { if (n) typed[n - 1] = 0; }
        else if (k == K_ENTER)
        {
            typing = false;
            char u[600];
            u_copy(u, sizeof u, typed);
            u_trim(u);
            if (!u[0]) return;
            if (!u_is_url(u)) { char t[700]; snprintf(t, sizeof t, "https://%.590s", u); u_copy(u, sizeof u, t); }
            u_copy(pending_add, sizeof pending_add, u);
            adding = true;
            feed_no = -1;
            feed_name[0] = 0;
            nf_fetch(u);
        }
        else if (k >= 32 && k < 127 && n < sizeof typed - 1) { typed[n] = (char)k; typed[n + 1] = 0; }
        return;
    }
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    // the page's own first, then the common ones
    if (view == V_INFO) { if (u == K_ESC || u == K_ENTER || u == K_BS || u == 'I') view = last_view; return; }
    if (u == 'C') { ui_page(PG_CLOCK); return; }
    if (u == 'W') { ui_page(PG_WX); return; }
    if (u == 'T') { ui_page(PG_PLAYER); return; }
    if (u == 'I') { last_view = view; view = V_INFO; return; }
    if (view == V_FEEDS)
    {
        if (u == K_LEFT || u == K_RIGHT || u == '\t' || u == K_UP || u == K_DOWN || u == K_PGUP || u == K_PGDN || u == K_ENTER ||
            u == K_HOME || u == K_END ||
            u == K_BS || u == K_ESC || u == 'D' || u == K_DEL || u == 'U' || u == 'J' || u == 'A')
        { feeds_key(k, u); return; }
        ui_playback_key(k);
        return;
    }
    int n;
    nf_items(&n);
    if (view == V_LIST)
    {
        switch (u)
        {
        case K_UP: if (sel > 0) sel--; return;
        case K_DOWN: if (sel + 1 < n) sel++; return;
        case K_LEFT: case K_PGUP: sel = sel > NL_ROWS ? sel - NL_ROWS : 0; top = top > NL_ROWS ? top - NL_ROWS : 0; return;
        case K_RIGHT: case K_PGDN: sel = sel + NL_ROWS < n ? sel + NL_ROWS : n - 1; top += NL_ROWS; if (top > sel) top = sel; return;
        case K_HOME: sel = 0; return;
        case K_END: sel = n - 1; return;
        case K_ENTER: open_story(sel); return;
        case K_BS: if (ext) { ui_page(PG_MARKETS); return; } view = V_FEEDS; return;
        case 'F': view = V_FEEDS; return;
        case K_ESC: ui_page(ext ? PG_MARKETS : PG_PLAYER); return;
        case 'S':
            cfg.news_by_feed = !cfg.news_by_feed;
            nf_sort(!cfg.news_by_feed);
            note(cfg.news_by_feed ? " Sorted: as the feed has them" : " Sorted: newest first");
            return;
        }
        if (u >= '1' && u <= '9') { open_feed(u - '1'); return; }
        ui_playback_key(k);
        return;
    }
    // a story
    switch (u)
    {
    case K_UP: s_top--; return;
    case K_DOWN: s_top++; return;
    case K_LEFT: case K_PGUP: s_top -= NS_ROWS; return;
    case K_RIGHT: case K_PGDN: s_top += NS_ROWS; return;
    case K_HOME: s_top = 0; return;
    case K_END: s_top = 1 << 20; return;
    case K_ENTER:
        s_full = !s_full;
        if (s_full) nf_article(story);
        s_top = 0;
        return;
    case 'N': if (story + 1 < n) { open_story(story + 1); sel = story; } return;
    case 'P': if (story > 0) { open_story(story - 1); sel = story; } return;
    case K_ESC: case K_BS: view = V_LIST; return;
    }
    ui_playback_key(k);
}

static void nw_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (b == MOUSE_WHEEL_UP) nw_key(K_UP);
    else if (b == MOUSE_WHEEL_DOWN) nw_key(K_DOWN);
    else if (b == MOUSE_RIGHT) nw_key(K_ESC);
}

static void nw_describe(char *out, size_t n)
{
    int k;
    nf_item *it = nf_items(&k);
    static const char *const vn[] = { "headlines", "story", "feeds", "info" };
    size_t o = (size_t)snprintf(out, n, "news %s feed='%s' state=%d stories=%d err='%s'", vn[view], feed_name, nf_state(), k, nf_error());
    if (view == V_LIST)
        for (int i = 0; i < k && i < 5 && o < n; i++)
        {
            char d[40];
            nf_fmt_date(&it[i], d, sizeof d, false);
            o += (size_t)snprintf(out + o, n - o, "\n  %s  %s", d, it[i].title);
        }
    if (view == V_STORY)
    {
        story_build();
        o += (size_t)snprintf(out + o, n - o, " article=%d lines=%d", s_full ? nf_article_state() : -1, s_lines.n);
        for (int i = 0; i < s_lines.n && i < 6 && o < n; i++) o += (size_t)snprintf(out + o, n - o, "\n  | %s", s_lines.l[i]);
    }
    if (view == V_FEEDS)
    {
        nf_feed *f;
        int fn = nf_favs(&f);
        for (int i = 0; i < fn && o < n; i++) o += (size_t)snprintf(out + o, n - o, "\n  %d %s = %s", i + 1, f[i].name, f[i].url);
    }
}

const page_t page_news = { nw_open, nw_frame, nw_key, nw_mouse, nw_close, nw_describe };

// a dropped .opml file: its feeds into My feeds
int news_import(const char *path) { return nf_import_opml(path); }
