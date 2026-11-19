// mktsvc.c - the market monitor's service: in a thread of its own it fetches
// the open list's prices (20 symbols a request, again every minute), a chart
// (one instrument over a range) and the region's market headlines (every 15
// minutes), from Yahoo Finance as the PicoVerse Audio Engine's pva_markets.c
// does. A chart asked for goes before the rest of a long list's prices.
// In the browser (__wasm__) there are no threads: ms_poll, each frame, starts one
// request at a time and its answer comes in a callback; Yahoo does not let other
// pages read it, so the requests go through the relay ("relay:", web/picoamp.js).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "markets.h"
#include "json.h"
#include "sys.h"
#include "util.h"
#ifdef __wasm__
#include "web.h"
#else
#include "net.h"
#endif

#define CHUNK      20
#define PTS_MAX    1200
#define TAPE_MAX   4000
#define TAPE_S     900
#define RETRY_S    20
#define AGENT      "User-Agent: Mozilla/5.0 (compatible; PicoAmp for Windows)"

static sys_mutex *mx;
static bool       started;

// the list
static mk_item    items[MS_ROWS];
static char       syms[MS_ROWS][32];
static int        n_items;
static uint32_t   list_gen;
static ms_quote   q[MS_ROWS];
static int        st = MS_IDLE, next_row;   // next_row: the next chunk's first row (0: a new round)
static bool       now_asked;
static uint32_t   due, changes;
static int64_t    updated;
static int        refresh_s = 60;
static char       err[200];

// the chart
static char       ch_sym[32];
static int        ch_range, ch_st = MS_IDLE;
static bool       ch_asked;
static ms_series  ch;

// the tape
static int        tp_region = -1, tp_have_region = -1;
static char       tape[TAPE_MAX];
static uint32_t   tp_due;
static bool       tp_due_set;

static const char *base(void)
{
    const char *e = getenv("PICOAMP_YAHOO");
    return e && *e ? e : "https://query1.finance.yahoo.com";
}

static const char *feeds_base(void)
{
    const char *e = getenv("PICOAMP_YFEEDS");
    return e && *e ? e : "https://feeds.finance.yahoo.com";
}

static void put_sym(char *o, size_t n, const char *s)
{
    size_t k = 0;
    for (; *s && k + 4 < n; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') o[k++] = (char)c;
        else k += (size_t)snprintf(o + k, n - k, "%%%02X", c);
    }
    o[k] = 0;
}

void ms_feed_url(const char *sym, char *out, size_t n)
{
    char e[100];
    put_sym(e, sizeof e, sym);
    snprintf(out, n, "%s/rss/2.0/headline?s=%s&region=US&lang=en-US", feeds_base(), e);
}

#ifndef __wasm__
static char *fetch(const char *url, size_t max, char *e, size_t en)
{
    int status = 0;
    net_conn *c = net_get(url, AGENT, &status, e, en, NULL);
    if (!c) return NULL;
    if (status != 200) { snprintf(e, en, "HTTP %d", status); net_close(c); return NULL; }
    size_t cap = 16384, len = 0;
    char *b = malloc(cap + 1);
    for (;;)
    {
        if (len == cap)
        {
            if (cap >= max) break;
            cap *= 2;
            b = realloc(b, cap + 1);
        }
        long r = net_read(c, b + len, cap - len);
        if (r <= 0) break;
        len += (size_t)r;
    }
    net_close(c);
    b[len] = 0;
    return b;
}
#endif

// ---------------- the replies ----------------
// the path is "SYMBOL.field" (a symbol may have dots of its own)
static bool split(const char *path, char *sym, size_t sn, const char **field)
{
    const char *d = strrchr(path, '.');
    if (!d) return false;
    size_t k = (size_t)(d - path);
    if (k >= sn) return false;
    memcpy(sym, path, k);
    sym[k] = 0;
    *field = d + 1;
    return true;
}

typedef struct { int first, count; ms_quote got[CHUNK]; } quote_job;

static void on_quotes(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    quote_job *qj = j->user;
    char s[40];
    const char *f;
    if (ev != JSON_VALUE || !split(path, s, sizeof s, &f) || !strcmp(v, "null")) return;
    for (int i = 0; i < qj->count; i++)
    {
        if (strcmp(syms[qj->first + i], s)) continue;
        ms_quote *g = &qj->got[i];
        double x = atof(v);
        if (!strcmp(f, "fulldayPrice") && x > 0) { g->price = x; g->have = true; }
        else if (!strcmp(f, "close") && x > 0 && !g->have) g->price = x;      // (the last of them, when no fulldayPrice)
        else if (!strcmp(f, "chartPreviousClose") && x > 0) g->prev = x;
        else if (!strcmp(f, "previousClose") && x > 0 && g->prev <= 0) g->prev = x;
        return;
    }
    (void)idx;
}

typedef struct { ms_series s; int cap; } chart_job;

static void on_chart(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    chart_job *cj = j->user;
    char s[40];
    const char *f;
    if (ev != JSON_VALUE || !split(path, s, sizeof s, &f)) return;
    bool null = !strcmp(v, "null");
    if (!strcmp(f, "timestamp") && idx < cj->cap)
    {
        cj->s.t[idx] = strtoll(v, NULL, 10);
        cj->s.v[idx] = NAN;
        if (idx >= cj->s.n) cj->s.n = idx + 1;
    }
    else if (!strcmp(f, "close") && idx < cj->cap) { if (!null) cj->s.v[idx] = atof(v); }
    else if (null) return;
    else if (!strcmp(f, "chartPreviousClose")) cj->s.prev = atof(v);
    else if (!strcmp(f, "fulldayPrice")) cj->s.price = atof(v);
    else if (!strcmp(f, "start")) cj->s.start = strtoll(v, NULL, 10);
    else if (!strcmp(f, "end")) cj->s.end = strtoll(v, NULL, 10);
}

static bool parse(const char *body, json_cb cb, void *user)
{
    json_t *j = calloc(1, sizeof *j);
    j->cb = cb;
    j->user = user;
    json_reset(j);
    json_feed(j, body, strlen(body));
    bool ok = j->seen;
    free(j);
    return ok;
}

// the headlines' titles of an RSS feed, " +++ " between them
static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static void entity(const char **pp, char *o, size_t *k, size_t n)
{
    const char *p = *pp;
    const char *e = strchr(p, ';');
    if (!e || e - p > 10) { if (*k + 1 < n) o[(*k)++] = '&'; *pp = p + 1; return; }
    char nm[12];
    memcpy(nm, p + 1, (size_t)(e - p - 1));
    nm[e - p - 1] = 0;
    unsigned long cp = !strcmp(nm, "amp") ? '&' : !strcmp(nm, "lt") ? '<' : !strcmp(nm, "gt") ? '>' : !strcmp(nm, "quot") ? '"' :
                       !strcmp(nm, "apos") ? '\'' : nm[0] == '#' ? (nm[1] == 'x' ? strtoul(nm + 2, NULL, 16) : strtoul(nm + 1, NULL, 10)) : ' ';
    char u[5];
    int ul = utf8_put(u, (uint32_t)cp);
    for (int i = 0; i < ul && *k + 1 < n; i++) o[(*k)++] = u[i];
    *pp = e + 1;
}

static void titles(const char *x, char *o, size_t n)
{
    size_t k = 0;
    const char *p = x;
    o[0] = 0;
    while ((p = strstr(p, "<item")) != NULL)
    {
        const char *t = strstr(p, "<title>"), *end = strstr(p, "</item>");
        if (!t || (end && t > end)) { p += 5; continue; }
        t += 7;
        const char *te = strstr(t, "</title>");
        if (!te) break;
        if (!strncmp(t, "<![CDATA[", 9)) { t += 9; const char *c = strstr(t, "]]>"); if (c && c < te) te = c; }
        if (k + 80 > n) break;
        if (k) { memcpy(o + k, " +++ ", 5); k += 5; }
        for (const char *c = t; c < te && k + 8 < n; )
        {
            if (*c == '&') { entity(&c, o, &k, n); continue; }
            char ch = *c++;
            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            if (ch == ' ' && k && o[k - 1] == ' ') continue;
            o[k++] = ch;
        }
        o[k] = 0;
        p = te;
    }
}

// ---------------- the thread ----------------
static int64_t now_s(void) { return sys_unix_time(); }

// ---------------- the requests: their addresses, their answers ----------------
typedef struct { char sym[32]; int range; } chart_ask;

static void chart_url(chart_ask *a, char *url, size_t un)
{
    char e[52];
    sys_lock(mx);
    u_copy(a->sym, sizeof a->sym, ch_sym);
    a->range = ch_range;
    ch_asked = false;
    sys_unlock(mx);
    static const char *const rg[MS_RANGES] = { "range=1d&interval=15m", "range=5d&interval=60m", "range=1mo&interval=1d", "range=6mo&interval=1d",
                                               "range=ytd&interval=1d", "range=1y&interval=1d", "range=5y&interval=1wk", "range=max&interval=1mo" };
    put_sym(e, sizeof e, a->sym);
    snprintf(url, un, "%s/v8/finance/spark?symbols=%s&%s", base(), e, rg[a->range]);
}

static void chart_done(const chart_ask *a, char *b)
{
    const char *sym = a->sym;
    int range = a->range;
    chart_job *cj = calloc(1, sizeof *cj);
    cj->cap = PTS_MAX;
    cj->s.t = calloc(PTS_MAX, sizeof *cj->s.t);
    cj->s.v = calloc(PTS_MAX, sizeof *cj->s.v);
    cj->s.range = range;
    bool ok = b && parse(b, on_chart, cj);
    free(b);
    // the points with a price, in order
    int k = 0;
    for (int i = 0; i < cj->s.n; i++)
        if (!isnan(cj->s.v[i]) && cj->s.v[i] > 0) { cj->s.t[k] = cj->s.t[i]; cj->s.v[k] = cj->s.v[i]; k++; }
    cj->s.n = k;
    sys_lock(mx);
    if (!ch_asked && !strcmp(sym, ch_sym) && range == ch_range)      // (not one asked for meanwhile)
    {
        ms_series_free(&ch);
        ch = cj->s;
        ch_st = ok && k ? MS_DONE : MS_FAILED;
        cj->s.t = NULL;
        cj->s.v = NULL;
    }
    sys_unlock(mx);
    free(cj->s.t);
    free(cj->s.v);
    free(cj);
}

static quote_job *quotes_url(char *url, size_t un, uint32_t *g)
{
    quote_job *qj = calloc(1, sizeof *qj);
    sys_lock(mx);
    *g = list_gen;
    qj->first = next_row;
    qj->count = n_items - next_row < CHUNK ? n_items - next_row : CHUNK;
    size_t o = (size_t)snprintf(url, un, "%s/v8/finance/spark?symbols=", base());
    for (int i = 0; i < qj->count; i++)
    {
        char e[52];
        put_sym(e, sizeof e, syms[qj->first + i]);
        o += (size_t)snprintf(url + o, un - o, "%s%s", i ? "," : "", e);
    }
    sys_unlock(mx);
    snprintf(url + o, un - o, "&range=1d&interval=1d");
    return qj;
}

static void quotes_done(quote_job *qj, uint32_t g, char *b, const char *m)
{
    bool ok = b && parse(b, on_quotes, qj);
    free(b);
    sys_lock(mx);
    if (g == list_gen)
    {
        if (ok)
        {
            for (int i = 0; i < qj->count; i++)
            {
                ms_quote *d = &q[qj->first + i];
                if (qj->got[i].price > 0) { d->price = qj->got[i].price; d->have = true; }
                if (qj->got[i].prev > 0) d->prev = qj->got[i].prev;
            }
            next_row += qj->count;
            changes++;
            if (next_row >= n_items)
            {
                next_row = 0;
                st = MS_DONE;
                err[0] = 0;
                updated = now_s();
                due = sys_millis() + (uint32_t)refresh_s * 1000u;
            }
        }
        else
        {
            next_row = 0;
            if (st != MS_DONE) st = MS_FAILED;
            u_copy(err, sizeof err, m[0] ? m : "no prices in the reply");
            due = sys_millis() + RETRY_S * 1000u;
        }
    }
    sys_unlock(mx);
    free(qj);
}

static void tape_url(int region, char *url, size_t un)
{
    static const char *const s[3] = { "%5EAEX,%5EGDAXI,%5EFTSE,%5EFCHI", "%5EGSPC,%5EDJI,%5EIXIC", "%5EN225,%5EHSI,000001.SS" };
    snprintf(url, un, "%s/rss/2.0/headline?s=%s&region=US&lang=en-US", feeds_base(), s[region % 3]);
}

static void tape_done(int region, char *b)
{
    char *t = malloc(TAPE_MAX);
    t[0] = 0;
    if (b) titles(b, t, TAPE_MAX);
    free(b);
    sys_lock(mx);
    tp_due_set = true;
    if (strlen(t) > 20 && region == tp_region)
    {
        u_copy(tape, sizeof tape, t);
        tp_have_region = region;
        tp_due = sys_millis() + TAPE_S * 1000u;
    }
    else tp_due = sys_millis() + 300000u;
    sys_unlock(mx);
    free(t);
}

// what is next: 1 a chart, 2 prices, 3 headlines (*region), 0 nothing
static int next_job(int *region_out)
{
    uint32_t t = sys_millis();
    sys_lock(mx);
    bool chart = ch_asked;
    bool quotes = n_items && (next_row || now_asked || (int32_t)(t - due) >= 0);
    if (quotes && !next_row) { now_asked = false; if (st != MS_DONE) st = MS_BUSY; }
    int region = tp_region;
    bool tp = region >= 0 && (tp_have_region != region || !tp_due_set || (int32_t)(t - tp_due) >= 0);
    sys_unlock(mx);
    *region_out = region;
    return chart ? 1 : quotes ? 2 : tp ? 3 : 0;
}

#ifndef __wasm__
static int run(void *arg)
{
    (void)arg;
    for (;;)
    {
        sys_sleep_ms(40);
        int region, k = next_job(&region);
        char url[1400], m[200] = "";
        if (k == 1)
        {
            chart_ask a;
            chart_url(&a, url, sizeof url);
            chart_done(&a, fetch(url, 1 << 20, m, sizeof m));
        }
        else if (k == 2)
        {
            uint32_t g;
            quote_job *qj = quotes_url(url, sizeof url, &g);
            quotes_done(qj, g, fetch(url, 1 << 18, m, sizeof m), m);
        }
        else if (k == 3)
        {
            tape_url(region, url, sizeof url);
            tape_done(region, fetch(url, 1 << 19, m, sizeof m));
        }
    }
    return 0;
}
#else
// one request at a time; its answer (0-terminated, a copy) to the job's *_done
typedef struct { int kind, region; uint32_t g; chart_ask a; quote_job *qj; } web_job;
static bool web_busy;

static void web_got(void *user, int status, const char *data, size_t len, const char *e)
{
    web_job *w = user;
    char m[200] = "";
    char *b = NULL;
    if (status == 200 && data) { b = malloc(len + 1); memcpy(b, data, len); b[len] = 0; }
    else if (status) snprintf(m, sizeof m, "HTTP %d", status);
    else u_copy(m, sizeof m, e && *e ? e : "no answer");
    if (w->kind == 1) chart_done(&w->a, b);
    else if (w->kind == 2) quotes_done(w->qj, w->g, b, m);
    else tape_done(w->region, b);
    free(w);
    web_busy = false;
}

void ms_poll(void)
{
    if (!started || web_busy) return;
    int region, k = next_job(&region);
    if (!k) return;
    web_job *w = calloc(1, sizeof *w);
    char url[1400] = "relay:";
    w->kind = k;
    w->region = region;
    if (k == 1) chart_url(&w->a, url + 6, sizeof url - 6);
    else if (k == 2) w->qj = quotes_url(url + 6, sizeof url - 6, &w->g);
    else tape_url(region, url + 6, sizeof url - 6);
    web_busy = true;
    net_fetch(url, web_got, w);
}
#endif

// ---------------- the main thread ----------------
void ms_init(void)
{
    if (started) return;
    started = true;
    mx = sys_mutex_new();
#ifndef __wasm__
    sys_thread_detach(sys_thread_start(run, NULL));
#endif
}

void ms_set_list(const mk_item *it, int n)
{
    ms_init();
    if (n > MS_ROWS) n = MS_ROWS;
    sys_lock(mx);
    list_gen++;
    n_items = it ? n : 0;
    for (int i = 0; i < n_items; i++) { items[i] = it[i]; u_copy(syms[i], sizeof syms[i], it[i].sym); }
    memset(q, 0, sizeof q);
    next_row = 0;
    now_asked = n_items > 0;
    st = n_items ? MS_BUSY : MS_IDLE;
    err[0] = 0;
    changes++;
    sys_unlock(mx);
}

void ms_refresh_now(void) { ms_init(); sys_lock(mx); now_asked = true; sys_unlock(mx); }
void ms_set_refresh(int s) { refresh_s = s < 20 ? 20 : s; }
int ms_state(void) { return st; }
const char *ms_error(void) { return err; }
uint32_t ms_changes(void) { return changes; }
int64_t ms_updated(void) { return updated; }

bool ms_quote_get(int r, ms_quote *out)
{
    if (!started || r < 0 || r >= MS_ROWS) return false;
    sys_lock(mx);
    *out = q[r];
    sys_unlock(mx);
    return out->have;
}

void ms_chart(const char *sym, int range)
{
    ms_init();
    sys_lock(mx);
    u_copy(ch_sym, sizeof ch_sym, sym);
    ch_range = range < 0 || range >= MS_RANGES ? 0 : range;
    ch_asked = true;
    ch_st = MS_BUSY;
    sys_unlock(mx);
}

int ms_chart_state(void) { return ch_st; }

bool ms_chart_get(ms_series *s)
{
    memset(s, 0, sizeof *s);
    if (!started) return false;
    sys_lock(mx);
    bool ok = ch_st == MS_DONE && ch.n > 0;
    if (ok)
    {
        *s = ch;
        s->t = malloc(sizeof *s->t * (size_t)ch.n);
        s->v = malloc(sizeof *s->v * (size_t)ch.n);
        memcpy(s->t, ch.t, sizeof *s->t * (size_t)ch.n);
        memcpy(s->v, ch.v, sizeof *s->v * (size_t)ch.n);
    }
    sys_unlock(mx);
    return ok;
}

void ms_series_free(ms_series *s)
{
    free(s->t);
    free(s->v);
    s->t = NULL;
    s->v = NULL;
    s->n = 0;
}

void ms_tape_region(int region) { ms_init(); sys_lock(mx); tp_region = region; sys_unlock(mx); }

bool ms_tape_get(char *out, size_t n)
{
    if (!started) return false;
    sys_lock(mx);
    bool ok = tp_have_region == tp_region && tape[0];
    if (ok) u_copy(out, n, tape);
    sys_unlock(mx);
    return ok;
}
