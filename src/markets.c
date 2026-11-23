// markets.c - $: PICOAMP's market monitor (markets.asm), with 80 columns of
// text in four colours as on the MSX2 (SCREEN 6: 512 x 212, 26 rows of 6 x 8
// characters, 16 pixels of border): risers green, fallers red.
//
// The table: a list of up to 100 instruments, 40 a page in two columns of 20:
// name, price, change and percent against the previous close. A region's lists
// (Europe, Americas, Asia-Pacific): its indices; currencies, commodities and
// rates; the shares of two markets; and My list ([markets] list= in
// picoamp.ini). I on an index shows its members in the list's place. Row 1: the
// region's market headlines as a tape.
//
// The chart: the chosen one's course as a line over 1D, 5D, 1M, 6M, YTD, 1Y, 5Y
// or all years, with a price scale and a time scale, green above the close
// before the range (the dashed line) and red under it; its four latest
// headlines under it, T reads them in the news reader.
//
// Keys: the table: Up/Down choose (past a page's last row: the next page),
// Left/Right the list, Enter the chart, I the index's members (BS back), O the
// order (the list's, the risers first, the fallers first), U update now, 1 2 3
// the region. The chart: Left/Right the range, Up/Down the next one, T its news,
// Enter, BS or ESC the table. C clock, W weather, T news, $ or ESC the player.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "markets.h"
#include "newsfeed.h"
#include "config.h"
#include "sys.h"
#include "util.h"

#define COLS     80
#define ROWS     26
#define ROW0     4                  // the table's first row
#define N_HALF   20                 // rows a column
#define PAGE     40
#define C_TEXT   WHITE
#define C_UP     GREEN
#define C_DOWN   RED
#define CH_X     28                 // the chart: its left (screen column 2),
#define CH_W     396                // its width (66 columns),
#define CH_Y     32                 // its top (row 4),
#define CH_H     120                // its height (to row 19)
#define CH_YCOL  70                 // the price scale's column
#define CH_XROW  19                 // the time scale's row
#define HROW     20                 // the headlines: rows 20-23

enum { V_TABLE, V_CHART };

static int    view;
static int    region, list_no;      // the list: of the region's lists, then My list
static const mk_members *sub;       // an index's members in the list's place
static int    sub_from;             // the index's row
static const mk_item *items;
static int    n_items;
static int    ord[MS_ROWS];
static int    sel, top;
static mk_item my_items[40];
static char   my_syms[40][32];
static int    n_my;
static char   msg[160];
static uint32_t msg_until;
// the chart
static int    ch_row;               // in the list's own order
static unsigned heads_serial;       // the news reader's feed that is the chart's headlines
static char   heads_sym[32];
static uint32_t tape_t0;

static void note(const char *m) { u_copy(msg, sizeof msg, m); msg_until = sys_millis() + 3500; }

// ---------------- text ----------------
static void tx(int row, int col, const char *s, uint8_t fg, int max)
{
    if (row < 0 || row >= ROWS || col >= COLS) return;
    if (max <= 0 || col + max > COLS) max = COLS - col;
    gfx_text6_at(16 + 6 * col, row * 8, s, fg, -1, max * 6);
}

static void tx_right(int row, int end_col, const char *s, uint8_t fg)
{
    int n = gfx_text6_px(s) / 6;
    tx(row, end_col - n + 1, s, fg, n);
}

static void row0(void)
{
    static const uint8_t note8[8] = { 0x18, 0x14, 0x12, 0x10, 0x70, 0xF0, 0x60, 0x00 };
    for (int y = 0; y < 8; y++)
        for (int b = 0; b < 6; b++) if (note8[y] & (0x80 >> b)) gfx_pset(16 + b, y, C_TEXT);
    tx(0, 2, ui_now_playing(), C_TEXT, 58);
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
    tx_right(0, 79, t, C_TEXT);
}

static void keylines(const char *k)
{
    if (msg[0] && sys_millis() < msg_until) tx(24, 0, msg, YELLOW, 80);
    else { msg[0] = 0; tx(24, 0, k, C_TEXT, 80); }
    tx(25, 0, " 1 Europe  2 Americas  3 Asia-Pacific   C clock  W weather  T news  $/ESC player", C_TEXT, 80);
}

static struct tm local_tm(int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

// ---------------- numbers ----------------
static void fmt_num(char *o, size_t n, double v, int dec, bool sign)
{
    snprintf(o, n, sign ? "%+.*f" : "%.*f", dec, v);
    if (sign && !strncmp(o, "-0", 2) && atof(o) == 0) o[0] = '+';     // (no "-0.00")
}

static double pct(const ms_quote *q) { return q->prev > 0 ? (q->price - q->prev) * 100.0 / q->prev : 0; }

// ---------------- the lists ----------------
static int region_lists(int r, int *first)
{
    int n = 0, f = -1;
    for (int i = 0; mk_lists[i].name; i++)
        if (mk_lists[i].region == r) { if (f < 0) f = i; n++; }
    if (first) *first = f;
    return n;
}

static void parse_my(void)
{
    n_my = 0;
    char buf[sizeof cfg.mk_list];
    u_copy(buf, sizeof buf, cfg.mk_list);
    char *p = buf, *t;
    while (n_my < 40 && (t = u_token(&p, ", ;\t")) != NULL)
    {
        u_copy(my_syms[n_my], sizeof my_syms[n_my], t);
        my_items[n_my].sym = my_syms[n_my];
        my_items[n_my].name = my_syms[n_my];
        my_items[n_my].dec = 2;
        n_my++;
    }
}

static int lists_here(void) { return region_lists(region, NULL) + (n_my ? 1 : 0); }

static const char *list_name(void)
{
    if (sub) return sub->name;
    int f, n = region_lists(region, &f);
    return list_no < n ? mk_lists[f + list_no].name : "My list";
}

static void resort(void)
{
    for (int i = 0; i < n_items; i++) ord[i] = i;
    if (!cfg.mk_sort) return;
    for (int i = 1; i < n_items; i++)
    {
        int r = ord[i], k = i;
        ms_quote a, b;
        bool ha = ms_quote_get(r, &a) && a.prev > 0;
        while (k)
        {
            bool hb = ms_quote_get(ord[k - 1], &b) && b.prev > 0;
            bool before = ha && (!hb || (cfg.mk_sort == 1 ? pct(&a) > pct(&b) : pct(&a) < pct(&b)));
            if (!before) break;
            ord[k] = ord[k - 1];
            k--;
        }
        ord[k] = r;
    }
}

static void open_list(void)
{
    sub = NULL;
    int f, n = region_lists(region, &f);
    int all = n + (n_my ? 1 : 0);
    if (list_no >= all) list_no = 0;
    if (list_no < 0) list_no = all - 1;
    if (list_no < n) { items = mk_lists[f + list_no].items; n_items = mk_lists[f + list_no].n; }
    else { items = my_items; n_items = n_my; }
    if (n_items > MS_ROWS) n_items = MS_ROWS;
    ms_set_list(items, n_items);
    ms_tape_region(region);
    sel = top = 0;
    resort();
}

static void settop(void) { top = sel / PAGE * PAGE; }

static void members(void)
{
    if (sub || !n_items) return;
    const char *sym = items[ord[sel]].sym;
    for (int i = 0; mk_member_lists[i].sym; i++)
        if (!strcmp(mk_member_lists[i].sym, sym))
        {
            sub_from = sel;
            sub = &mk_member_lists[i];
            items = sub->items;
            n_items = sub->n > MS_ROWS ? MS_ROWS : sub->n;
            ms_set_list(items, n_items);
            sel = top = 0;
            resort();
            return;
        }
    note(" No member list for this one");
}

static void unsub(void)
{
    if (!sub) return;
    int s = sub_from;
    open_list();
    sel = s < n_items ? s : 0;
    settop();
}

// ---------------- the table ----------------
static void draw_status(void)
{
    char l[200];
    size_t o = (size_t)snprintf(l, sizeof l, "%s: %s", mk_region_names[region], list_name());
    if (!sub) o += (size_t)snprintf(l + o, sizeof l - o, "  (%d of %d)", list_no + 1, lists_here());
    if (n_items > PAGE)
        o += (size_t)snprintf(l + o, sizeof l - o, "  %d-%d of %d", top + 1, top + PAGE < n_items ? top + PAGE : n_items, n_items);
    if (cfg.mk_sort) snprintf(l + o, sizeof l - o, "  %s", cfg.mk_sort == 1 ? "risers first" : "fallers first");
    tx(2, 1, l, C_TEXT, 54);
    int64_t up = ms_updated();
    if (up)
    {
        struct tm tm = local_tm(up);
        snprintf(l, sizeof l, "%02d:%02d  Yahoo Finance, delayed", tm.tm_hour, tm.tm_min);
    }
    else u_copy(l, sizeof l, ms_state() == MS_FAILED ? "No prices" : "Fetching the prices...");
    tx_right(2, 78, l, C_TEXT);
}

static void draw_row(int row, int col, int i, bool chosen)
{
    const mk_item *it = &items[i];
    ms_quote q;
    bool have = ms_quote_get(i, &q);
    if (chosen) gfx_text6_at(16 + 6 * col, row * 8, ">", C_TEXT, -1, 6);
    tx(row, col + 1, it->name, C_TEXT, 14);
    if (!have) { tx_right(row, col + 24, "...", C_TEXT); return; }
    char t[40];
    fmt_num(t, sizeof t, q.price, it->dec, false);
    tx_right(row, col + 24, t, C_TEXT);
    if (q.prev <= 0) return;
    double d = q.price - q.prev, p = pct(&q);
    uint8_t c = d > 0 ? C_UP : d < 0 ? C_DOWN : C_TEXT;
    fmt_num(t, sizeof t, d, it->dec, true);
    tx_right(row, col + 32, t, c);
    snprintf(t, sizeof t, "%+.2f%%", p);
    if (!strncmp(t, "-0.00", 5)) t[0] = '+';
    tx_right(row, col + 39, t, c);
}

static void draw_tape(void)
{
    static char tape[4000];
    if (!ms_tape_get(tape, sizeof tape - 8)) { gfx_hline(16, 11, COLS * 6, C_TEXT); return; }
    // a pixel at a time, round and round
    int len = gfx_text6_px(tape) / 6 + 5;
    char loop[8100];
    snprintf(loop, sizeof loop, "%s +++ %s", tape, tape);
    int off = (int)((sys_millis() - tape_t0) / 25) % (len * 6);
    const char *p = loop;
    for (int k = off / 6; k > 0 && *p; k--) { p++; while ((*p & 0xC0) == 0x80) p++; }
    gfx_text6_at(16 - off % 6, 8, p, C_UP, -1, COLS * 6 + 6);
    gfx_rect(0, 8, 16, 8, NAVY);
    gfx_rect(16 + COLS * 6, 8, gfx_w - 16 - COLS * 6, 8, NAVY);
}

static void draw_table(void)
{
    draw_tape();
    draw_status();
    if (!n_items) { tx(10, 2, "No instruments in this list", C_TEXT, 0); keylines(""); return; }
    resort();
    static const char head[] = " Name                Last  Change      %";
    tx(3, 0, head, C_TEXT, 40);
    if (n_items - top > N_HALF) tx(3, 40, head, C_TEXT, 40);
    for (int k = 0; k < PAGE && top + k < n_items; k++)
    {
        int r = top + k;
        draw_row(ROW0 + k % N_HALF, k < N_HALF ? 0 : 40, ord[r], r == sel);
    }
    // (no prices at all: why, where the keys are)
    if (ms_state() == MS_FAILED && !ms_updated() && ms_error()[0])
    {
        char l[120];
        snprintf(l, sizeof l, " No prices: %s", ms_error());
        keylines(l);
        return;
    }
    keylines(" </> list  Up/Down choose  Enter chart  I members (BS back)  O order  U now");
}

// ---------------- the chart ----------------
static const char *const range_names[MS_RANGES] = { "1D", "5D", "1M", "6M", "YTD", "1Y", "5Y", "All" };

static void ask_chart(void)
{
    if (!n_items) return;
    ch_row = ord[sel];
    ms_chart(items[ch_row].sym, cfg.mk_range);
    view = V_CHART;
    if (strcmp(heads_sym, items[ch_row].sym))           // its headlines: the news reader fetches them
    {
        char url[400];
        ms_feed_url(items[ch_row].sym, url, sizeof url);
        news_feed_taken();
        nf_fetch(url);
        heads_serial = nf_serial();
        u_copy(heads_sym, sizeof heads_sym, items[ch_row].sym);
    }
}

static bool heads_ready(void) { return heads_serial && nf_serial() == heads_serial && nf_state() == NF_DONE; }

static void line(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;)
    {
        gfx_pset(x0, y0, c);
        gfx_pset(x0, y0 + 1, c);                    // two dots thick
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static double nice_step(double span)
{
    static const double m[3] = { 1, 2, 5 };
    double b = pow(10, floor(log10(span / 8)));
    for (int i = 0; i < 12; i++)
    {
        double s = b * m[i % 3] * pow(10, i / 3);
        if (span / s <= 8) return s;
    }
    return span;
}

static int dec_for(double step, int dec)
{
    int d = 0;
    while (d < dec && fabs(step * pow(10, d) - floor(step * pow(10, d) + 0.5)) > 1e-6) d++;
    return d;
}

static void draw_chart(void)
{
    const mk_item *it = &items[ch_row];
    // the ranges in the key line, the chosen one green
    char k[120];
    snprintf(k, sizeof k, " Left/Right range ");
    tx(24, 0, k, C_TEXT, 0);
    for (int i = 0, c = 18; i < MS_RANGES; i++, c += 4) tx(24, c, range_names[i], i == cfg.mk_range ? C_UP : C_TEXT, 4);
    tx(24, 50, "Up/Down next  T news  BS list", C_TEXT, 30);
    tx(25, 0, " 1 Europe  2 Americas  3 Asia-Pacific   C clock  W weather  T news  $/ESC player", C_TEXT, 80);
    int st = ms_chart_state();
    ms_series s;
    if (st == MS_BUSY || !ms_chart_get(&s))
    {
        tx(2, 1, it->name, C_TEXT, 30);
        tx(10, 2, st == MS_FAILED ? "No chart for this one over this range" : "Fetching the chart...", C_TEXT, 0);
        goto heads;
    }
    double lo = s.v[0], hi = s.v[0], last = s.price > 0 ? s.price : s.v[s.n - 1];
    for (int i = 1; i < s.n; i++) { if (s.v[i] < lo) lo = s.v[i]; if (s.v[i] > hi) hi = s.v[i]; }
    double dlo = lo, dhi = hi;
    bool have_prev = s.prev > 0;
    if (have_prev) { if (s.prev < lo) lo = s.prev; if (s.prev > hi) hi = s.prev; }
    if (hi - lo < 1e-12) { hi += 1; lo -= 1; }
    // the head: name, price, change; high, low, the close before
    char t[200], a[40], b[40], c[40];
    fmt_num(a, sizeof a, last, it->dec, false);
    tx(2, 1, it->name, C_TEXT, 16);
    tx(2, 18, a, C_TEXT, 14);
    if (have_prev)
    {
        double d = last - s.prev;
        fmt_num(b, sizeof b, d, it->dec, true);
        snprintf(t, sizeof t, "%s  %+.2f%%", b, d * 100 / s.prev);
        tx(2, 32, t, d > 0 ? C_UP : d < 0 ? C_DOWN : C_TEXT, 30);
    }
    fmt_num(a, sizeof a, dhi, it->dec, false);
    fmt_num(b, sizeof b, dlo, it->dec, false);
    if (have_prev) fmt_num(c, sizeof c, s.prev, it->dec, false); else c[0] = 0;
    snprintf(t, sizeof t, "High %s   Low %s   %s %s", a, b, have_prev ? (s.range ? "Close before" : "Previous close") : "", c);
    tx(3, 1, t, C_TEXT, 78);
    #define YOF(v) (CH_Y + CH_H - 1 - (int)(((v) - lo) * (CH_H - 2) / (hi - lo)))
    // the price scale: round prices, dotted lines and their labels at the right
    double step = nice_step(hi - lo);
    int sd = dec_for(step, it->dec);
    for (double v = ceil(lo / step) * step; v <= hi + step * 1e-9; v += step)
    {
        int y = YOF(v);
        for (int x = CH_X; x < CH_X + CH_W; x += 4) gfx_pset(x, y, GREY);
        fmt_num(a, sizeof a, v, sd, false);
        int ly = y - 3;
        if (ly < CH_Y) ly = CH_Y;
        if (ly > CH_Y + CH_H - 8) ly = CH_Y + CH_H - 8;
        gfx_text6_at(16 + 6 * CH_YCOL, ly, a, C_TEXT, -1, 60);
    }
    if (have_prev)                                  // the close before: dashed
    {
        int y = YOF(s.prev);
        for (int x = CH_X; x < CH_X + CH_W; x += 6) gfx_hline(x, y, 3, C_TEXT);
    }
    // where the points are: 1D by their time over the exchange's day, else one after the other
    int64_t t0 = s.t[0], t1 = s.t[s.n - 1];
    bool by_time = s.range == 0;
    if (by_time)
    {
        if (s.start && s.start <= t0) t0 = s.start;
        if (s.end > t1) t1 = s.end;
        if (t1 - t0 > 2 * 86400) { t0 = s.t[0]; t1 = s.t[s.n - 1]; }
    }
    if (t1 <= t0) t1 = t0 + 1;
    #define XOF(i) (CH_X + (by_time ? (int)((s.t[i] - t0) * (CH_W - 1) / (t1 - t0)) : (s.n > 1 ? (int)((int64_t)(i) * (CH_W - 1) / (s.n - 1)) : 0)))
    for (int i = 0; i + 1 < s.n || (s.n == 1 && i == 0); i++)
    {
        int j = s.n > 1 ? i + 1 : i;
        double mid = (s.v[i] + s.v[j]) / 2;
        uint8_t col = !have_prev ? C_TEXT : mid >= s.prev ? C_UP : C_DOWN;
        line(XOF(i), YOF(s.v[i]), XOF(j), YOF(s.v[j]), col);
        if (s.n == 1) break;
    }
    // the time scale
    {
        static const char *const wdn[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        int lastx = -100;
        if (by_time)
        {
            int64_t hours = (t1 - t0) / 3600, stp = hours <= 10 ? 2 : hours <= 16 ? 3 : 6;
            struct tm tm = local_tm(t0);
            int64_t tt = t0 - (tm.tm_hour % stp) * 3600 - tm.tm_min * 60 - tm.tm_sec;
            for (; tt <= t1; tt += stp * 3600)
            {
                if (tt < t0) continue;
                tm = local_tm(tt);
                snprintf(a, sizeof a, "%02d:00", tm.tm_hour);
                int x = CH_X + (int)((tt - t0) * (CH_W - 1) / (t1 - t0));
                if (x - 15 < lastx + 6) continue;
                gfx_text6_at(x - 15, CH_XROW * 8, a, C_TEXT, -1, 30);
                lastx = x + 15;
            }
        }
        else
        {
            struct tm f = local_tm(s.t[0]), l = local_tm(s.t[s.n - 1]);
            int span_y = l.tm_year - f.tm_year, span_m = span_y * 12 + l.tm_mon - f.tm_mon;
            int years = span_y <= 7 ? 1 : span_y <= 14 ? 2 : span_y <= 35 ? 5 : 10;
            int months = span_m > 8 ? 2 : 1;
            long prev_key = -1;
            for (int i = 0; i < s.n; i++)
            {
                struct tm tm = local_tm(s.t[i]);
                long key;
                switch (s.range)
                {
                case 1: key = tm.tm_year * 400L + tm.tm_yday; snprintf(a, sizeof a, "%s", wdn[tm.tm_wday]); break;
                case 2: key = (long)((s.t[i] / 86400 + 3) / 7); snprintf(a, sizeof a, "%d %s", tm.tm_mday, mon[tm.tm_mon]); break;
                case 3: case 4: case 5: key = (tm.tm_year * 12L + tm.tm_mon) / months; snprintf(a, sizeof a, "%s", mon[tm.tm_mon]); break;
                case 6: key = tm.tm_year; snprintf(a, sizeof a, "%d", tm.tm_year + 1900); break;
                default: key = (tm.tm_year + 1900) / years; snprintf(a, sizeof a, "%d", tm.tm_year + 1900); break;
                }
                if (key == prev_key) continue;
                bool first = prev_key < 0;
                prev_key = key;
                if (first && s.range != 1) continue;       // (the first point is in the middle of its unit)
                int x = XOF(i), w = gfx_text6_px(a);
                if (x < lastx + 8) continue;
                gfx_text6_at(x, CH_XROW * 8, a, C_TEXT, -1, w);
                lastx = x + w;
            }
        }
    }
    #undef XOF
    #undef YOF
    ms_series_free(&s);
heads:
    if (heads_ready())
    {
        int n;
        nf_item *h = nf_items(&n);
        for (int i = 0; i < n && i < 4; i++)
        {
            char d[40], l[400];
            nf_fmt_date(&h[i], d, sizeof d, false);
            snprintf(l, sizeof l, "%-6s  %s", d, h[i].title);
            tx(HROW + i, 1, l, C_TEXT, 79);
        }
    }
    else if (heads_serial && nf_serial() == heads_serial && nf_state() == NF_BUSY) tx(HROW, 1, "Fetching the headlines...", C_TEXT, 0);
}

// ---------------- the page ----------------
static bool opened;

static void mk_open(void)
{
    gfx_set_size(512, 212);
    ms_set_refresh(cfg.mk_refresh);
    parse_my();
    if (!opened)
    {
        opened = true;
        region = cfg.mk_region % 3;
        list_no = 0;
        view = V_TABLE;
        tape_t0 = sys_millis();
        open_list();
    }
    else ms_set_list(items, n_items), ms_tape_region(region);      // (fetched again: it was closed)
}

static void mk_close(void)
{
    gfx_set_size(GFX_W, GFX_H);
    ms_set_list(NULL, 0);
    ms_tape_region(-1);
}

static void mk_frame(void)
{
    gfx_set_size(512, 212);
    gfx_clear(NAVY);
    row0();
    if (view == V_CHART) { gfx_hline(16, 11, COLS * 6, C_TEXT); draw_chart(); }
    else draw_table();
}

static void move_sel(int d)
{
    if (!n_items) return;
    sel = (sel + d + n_items) % n_items;
    settop();
}

static void mk_key(int k)
{
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    if (view == V_CHART)
    {
        switch (u)
        {
        case K_LEFT: cfg.mk_range = (cfg.mk_range + MS_RANGES - 1) % MS_RANGES; ask_chart(); return;
        case K_RIGHT: cfg.mk_range = (cfg.mk_range + 1) % MS_RANGES; ask_chart(); return;
        case K_UP: move_sel(-1); ask_chart(); return;
        case K_DOWN: move_sel(1); ask_chart(); return;
        case K_ENTER: case K_BS: case K_ESC: view = V_TABLE; return;
        case 'T':
            if (heads_ready()) { news_open_market(items[ch_row].name); ui_page(PG_NEWS); }
            else note(" No headlines for this one (yet)");
            return;
        }
    }
    else switch (u)
    {
    case K_UP: move_sel(-1); return;
    case K_DOWN: move_sel(1); return;
    case K_PGUP: if (sel >= PAGE) { sel -= PAGE; settop(); } return;
    case K_PGDN: if (sel + PAGE < n_items) { sel += PAGE; settop(); } return;
    case K_HOME: sel = 0; settop(); return;
    case K_END: if (n_items) { sel = n_items - 1; settop(); } return;
    case K_LEFT: list_no--; open_list(); return;
    case K_RIGHT: list_no++; open_list(); return;
    case K_ENTER: ask_chart(); return;
    case 'I': members(); return;
    case K_BS: unsub(); return;
    case 'O': cfg.mk_sort = (cfg.mk_sort + 1) % 3; resort(); return;
    case 'U': ms_refresh_now(); note(" Updating the prices"); return;
    case K_ESC: case '$': ui_page(PG_PLAYER); return;
    case 'T': ui_page(PG_NEWS); return;
    }
    switch (u)
    {
    case '1': case '2': case '3':
        region = cfg.mk_region = u - '1';
        list_no = 0;
        view = V_TABLE;
        open_list();
        return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'W': ui_page(PG_WX); return;
    case '$': case K_ESC: ui_page(PG_PLAYER); return;
    }
    ui_playback_key(k);
}

static void mk_mouse(int x, int y, int b)
{
    if (b == MOUSE_WHEEL_UP) { mk_key(K_UP); return; }
    if (b == MOUSE_WHEEL_DOWN) { mk_key(K_DOWN); return; }
    if (b == MOUSE_RIGHT) { mk_key(view == V_CHART ? K_ESC : (sub ? K_BS : K_ESC)); return; }
    if (b != MOUSE_LEFT || view != V_TABLE) return;
    int row = y / 8 - ROW0, col = (x - 16) / 6;
    if (row < 0 || row >= N_HALF || col < 0 || col >= COLS) return;
    int r = top + row + (col >= 40 ? N_HALF : 0);
    if (r >= n_items) return;
    if (r == sel) ask_chart();
    else sel = r;
}

static void mk_describe(char *out, size_t n)
{
    size_t o = (size_t)snprintf(out, n, "markets %s region=%s list='%s' rows=%d state=%d sort=%d sel=%d top=%d sub=%d",
                                view == V_CHART ? "chart" : "table", mk_region_names[region], list_name(), n_items, ms_state(), cfg.mk_sort, sel, top, sub != NULL);
    if (ms_state() == MS_FAILED) o += (size_t)snprintf(out + o, n - o, " err='%s'", ms_error());
    if (view == V_TABLE)
    {
        char tape[200];
        if (ms_tape_get(tape, sizeof tape)) o += (size_t)snprintf(out + o, n - o, "\n  tape: %.60s", tape);
        for (int k = 0; k < 5 && top + k < n_items && o < n; k++)
        {
            int i = ord[top + k];
            ms_quote q;
            bool h = ms_quote_get(i, &q);
            o += (size_t)snprintf(out + o, n - o, "\n  %s %s %.4f %.2f%%", items[i].sym, items[i].name, h ? q.price : 0, h ? pct(&q) : 0);
        }
    }
    else
    {
        ms_series s;
        bool ok = ms_chart_get(&s);
        o += (size_t)snprintf(out + o, n - o, " sym=%s range=%s chart=%d points=%d", items[ch_row].sym, range_names[cfg.mk_range], ms_chart_state(), ok ? s.n : 0);
        if (ok) o += (size_t)snprintf(out + o, n - o, " first=%.2f last=%.2f prev=%.2f", s.v[0], s.v[s.n - 1], s.prev);
        ms_series_free(&s);
        if (heads_ready())
        {
            int k;
            nf_item *h = nf_items(&k);
            for (int i = 0; i < k && i < 4 && o < n; i++) o += (size_t)snprintf(out + o, n - o, "\n  head: %s", h[i].title);
        }
    }
}

const page_t page_markets = { mk_open, mk_frame, mk_key, mk_mouse, mk_close, mk_describe };
