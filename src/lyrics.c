// lyrics.c - the words of what plays, from LRCLIB (see lyrics.h).
//
//   /api/get?artist_name=&track_name=&album_name=&duration=   the one song (a file, another program)
//   /api/search?track_name=&artist_name=                      else: the best of what it finds
//                                                             (synced first, the length closest)
// The answer's "syncedLyrics" is LRC: "[mm:ss.xx] words" lines.
// In the browser (__wasm__) there are no threads: the two questions one after the other, each
// answer in a callback; LRCLIB straight, else through the relay ("cors:", web/picoamp.js). What
// was found is kept in memory there (the page's storage is small), in <config>/Lyrics here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lyrics.h"
#ifdef __wasm__
#include "web.h"
#else
#include "net.h"
#endif
#include "radiodir.h"
#include "sys.h"
#include "util.h"

#define REPLY_MAX   (1u << 20)
#define NONE_DAYS   7

typedef struct { int ms; char *text; } ly_line;

typedef struct
{
    int      state;
    ly_line *l;
    int      n;
    char     error[120];
} ly_set;

static sys_mutex *lock;
static unsigned   want_gen;            // the lookup asked for last
static char       want_key[1100];
static ly_set     cur;                 // the screen's
static ly_set     came;                // (lock) the thread's result for want_gen
static bool       came_have;

typedef struct { unsigned gen; char artist[256], title[256], album[256]; int seconds; char key[1100]; } job_t;

static void set_free(ly_set *s)
{
    for (int i = 0; i < s->n; i++) free(s->l[i].text);
    free(s->l);
    memset(s, 0, sizeof *s);
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

// ---------------- what was found, kept (a "none" only for a week) ----------------
#ifndef __wasm__
static void cache_path(char *out, size_t n, const char *key)
{
    char d[1024], f[32];
    u_join(d, sizeof d, sys_config_dir(), "Lyrics");
    sys_mkdir(d);
    snprintf(f, sizeof f, "%08X.lrc", (unsigned)fnv(key));
    u_join(out, n, d, f);
}

static char *kept_get(const char *key)
{
    char path[1200];
    cache_path(path, sizeof path, key);
    char *t = sys_read_file(path, NULL);
    if (t && !strncmp(t, "none", 4) && sys_unix_time() - sys_file_time(path) > NONE_DAYS * 86400) { free(t); t = NULL; }
    return t;
}

static void kept_put(const char *key, const char *t)
{
    char path[1200];
    cache_path(path, sizeof path, key);
    sys_write_file(path, t, strlen(t));
}

static void kept_forget(const char *key)
{
    char path[1200];
    cache_path(path, sizeof path, key);
    sys_delete(path);
}
#else
// the last KEPT_N songs, in memory (the oldest goes)
#define KEPT_N 64
static struct { uint32_t h; char *t; } kept[KEPT_N];
static int kept_next;

static char *kept_get(const char *key)
{
    uint32_t h = fnv(key);
    for (int i = 0; i < KEPT_N; i++) if (kept[i].t && kept[i].h == h) return strdup(kept[i].t);
    return NULL;
}

static void kept_forget(const char *key)
{
    uint32_t h = fnv(key);
    for (int i = 0; i < KEPT_N; i++) if (kept[i].t && kept[i].h == h) { free(kept[i].t); kept[i].t = NULL; }
}

static void kept_put(const char *key, const char *t)
{
    kept_forget(key);
    free(kept[kept_next].t);
    kept[kept_next].h = fnv(key);
    kept[kept_next].t = strdup(t);
    kept_next = (kept_next + 1) % KEPT_N;
}
#endif

// ---------------- LRC and plain text into lines ----------------
static void add(ly_set *s, int ms, const char *t, size_t len)
{
    while (len && (t[len - 1] == '\r' || t[len - 1] == ' ')) len--;
    while (len && *t == ' ') { t++; len--; }
    ly_line *nl = realloc(s->l, sizeof *nl * (size_t)(s->n + 1));
    if (!nl) return;
    s->l = nl;
    s->l[s->n].ms = ms;
    s->l[s->n].text = malloc(len + 1);
    if (!s->l[s->n].text) return;
    memcpy(s->l[s->n].text, t, len);
    s->l[s->n].text[len] = 0;
    s->n++;
}

static int by_time(const void *a, const void *b)
{
    const ly_line *x = a, *y = b;
    return x->ms < y->ms ? -1 : x->ms > y->ms;
}

// "[mm:ss.xx]" at p: its time (ms) and the length of it; false: not a time ([ar:...])
static bool lrc_time(const char *p, int *ms, int *len)
{
    int m = 0, s = 0, f = 0, fd = 0, i = 1;
    if (p[0] != '[' || p[1] < '0' || p[1] > '9') return false;
    while (p[i] >= '0' && p[i] <= '9') m = m * 10 + (p[i++] - '0');
    if (p[i++] != ':') return false;
    if (p[i] < '0' || p[i] > '9') return false;
    while (p[i] >= '0' && p[i] <= '9') s = s * 10 + (p[i++] - '0');
    if (p[i] == '.' || p[i] == ':') for (i++; p[i] >= '0' && p[i] <= '9'; i++) if (fd < 3) { f = f * 10 + (p[i] - '0'); fd++; }
    if (p[i] != ']') return false;
    while (fd < 3) { f *= 10; fd++; }
    *ms = (m * 60 + s) * 1000 + f;
    *len = i + 1;
    return true;
}

static void parse_text(ly_set *s, const char *t, bool synced)
{
    for (const char *p = t; *p;)
    {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (synced)
        {
            // one or more times, then the words (a line of words for each time)
            int times[16], nt = 0, ms, tl;
            const char *q = p;
            while (q < p + len && lrc_time(q, &ms, &tl)) { if (nt < 16) times[nt++] = ms; q += tl; }
            for (int i = 0; i < nt; i++) add(s, times[i], q, len - (size_t)(q - p));
        }
        else add(s, -1, p, len);
        p += len + (e ? 1 : 0);
    }
    if (synced)
    {
        qsort(s->l, (size_t)s->n, sizeof *s->l, by_time);
        // (empty lines at the very start say nothing)
        while (s->n && !s->l[0].text[0]) { free(s->l[0].text); memmove(s->l, s->l + 1, sizeof *s->l * (size_t)--s->n); }
    }
    else
    {
        while (s->n && !s->l[s->n - 1].text[0]) free(s->l[--s->n].text);
    }
}

// a cache file / what is kept: "synced\n" + LRC, "plain\n" + words, "instrumental", "none"
static bool from_kept(ly_set *s, const char *t)
{
    memset(s, 0, sizeof *s);
    if (!strncmp(t, "synced\n", 7)) { s->state = LY_SYNCED; parse_text(s, t + 7, true); }
    else if (!strncmp(t, "plain\n", 6)) { s->state = LY_PLAIN; parse_text(s, t + 6, false); }
    else if (!strncmp(t, "instrumental", 12)) s->state = LY_INSTRUMENTAL;
    else if (!strncmp(t, "none", 4)) s->state = LY_NOT_FOUND;
    else return false;
    if ((s->state == LY_SYNCED || s->state == LY_PLAIN) && !s->n) s->state = LY_NOT_FOUND;
    return true;
}

// ---------------- the answer (JSON) ----------------
static void put_utf8(char *o, size_t *n, uint32_t c)
{
    if (c < 0x80) o[(*n)++] = (char)c;
    else if (c < 0x800) { o[(*n)++] = (char)(0xC0 | c >> 6); o[(*n)++] = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { o[(*n)++] = (char)(0xE0 | c >> 12); o[(*n)++] = (char)(0x80 | (c >> 6 & 63)); o[(*n)++] = (char)(0x80 | (c & 63)); }
    else { o[(*n)++] = (char)(0xF0 | c >> 18); o[(*n)++] = (char)(0x80 | (c >> 12 & 63)); o[(*n)++] = (char)(0x80 | (c >> 6 & 63)); o[(*n)++] = (char)(0x80 | (c & 63)); }
}

static uint32_t hex4(const char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        char c = p[i];
        v = v * 16 + (uint32_t)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

// the end of a JSON string that starts at p ('"'): after its closing quote
static const char *str_end(const char *p, const char *end)
{
    for (p++; p < end && *p != '"'; p++) if (*p == '\\' && p + 1 < end) p++;
    return p < end ? p + 1 : end;
}

// the end of the value at p (a string, a number, an object, an array, true ...)
static const char *value_end(const char *p, const char *end)
{
    if (*p == '"') return str_end(p, end);
    if (*p == '{' || *p == '[')
    {
        int d = 0;
        for (; p < end; p++)
        {
            if (*p == '"') { p = str_end(p, end) - 1; continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
        }
        return end;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

// the value of key in the object [o, end) (its own keys only): where it starts, or NULL
static const char *field(const char *o, const char *end, const char *key)
{
    size_t kl = strlen(key);
    const char *p = o + 1;
    while (p < end)
    {
        while (p < end && *p != '"' && *p != '}') p++;
        if (p >= end || *p == '}') return NULL;
        const char *ks = p + 1, *ke = str_end(p, end) - 1;
        p = ke + 1;
        while (p < end && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
        if ((size_t)(ke - ks) == kl && !memcmp(ks, key, kl)) return p;
        p = value_end(p, end);
    }
    return NULL;
}

// a string field: malloc'd UTF-8 (NULL: not there, null)
static char *field_str(const char *o, const char *end, const char *key)
{
    const char *p = field(o, end, key);
    if (!p || *p != '"') return NULL;
    const char *e = str_end(p, end) - 1;
    char *r = malloc((size_t)(e - p) * 2 + 8);
    if (!r) return NULL;
    size_t n = 0;
    for (p++; p < e; p++)
    {
        if (*p != '\\') { r[n++] = *p; continue; }
        p++;
        switch (*p)
        {
        case 'n': r[n++] = '\n'; break;
        case 'r': break;
        case 't': r[n++] = ' '; break;
        case 'u':
        {
            uint32_t c = hex4(p + 1);
            p += 4;
            if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u')
            {
                uint32_t lo = hex4(p + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            put_utf8(r, &n, c);
            break;
        }
        default: r[n++] = *p;
        }
    }
    r[n] = 0;
    if (!n) { free(r); return NULL; }
    return r;
}

static double field_num(const char *o, const char *end, const char *key)
{
    const char *p = field(o, end, key);
    return p ? atof(p) : -1;
}

static bool field_true(const char *o, const char *end, const char *key)
{
    const char *p = field(o, end, key);
    return p && !strncmp(p, "true", 4);
}

// the best object of an answer (an object, or an array of them): what is kept ("synced\n..." ...), or NULL
static char *best_of(const char *r, int seconds)
{
    const char *end = r + strlen(r), *p = r;
    while (p < end && *p != '{' && *p != '[') p++;
    if (p >= end) return NULL;
    const char *objs[64], *ends[64];
    int n = 0;
    if (*p == '{') { objs[0] = p; ends[0] = value_end(p, end); n = 1; }
    else
        for (p++; p < end && n < 64;)
        {
            while (p < end && *p != '{' && *p != ']') p++;
            if (p >= end || *p == ']') break;
            objs[n] = p;
            ends[n] = value_end(p, end);
            p = ends[n++];
        }
    // the score: synced, then plain, then instrumental; the closer its length, the better
    int best = -1;
    double best_score = -1;
    for (int i = 0; i < n; i++)
    {
        if (field(objs[i], ends[i], "code") && !field(objs[i], ends[i], "id")) continue;    // {"code":404,...}
        const char *s = field(objs[i], ends[i], "syncedLyrics"), *pl = field(objs[i], ends[i], "plainLyrics");
        double score = (s && *s == '"') ? 3 : (pl && *pl == '"') ? 2 : field_true(objs[i], ends[i], "instrumental") ? 1 : 0;
        if (score <= 0) continue;
        double d = field_num(objs[i], ends[i], "duration");
        if (seconds > 0 && d > 0)
        {
            double diff = d > seconds ? d - seconds : seconds - d;
            if (diff > 10) score -= 2.5;           // another version of the song
            else score += 0.5 - diff / 20;
        }
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) return NULL;
    char *t;
    const char *head;
    if ((t = field_str(objs[best], ends[best], "syncedLyrics"))) head = "synced\n";
    else if ((t = field_str(objs[best], ends[best], "plainLyrics"))) head = "plain\n";
    else return strdup("instrumental");
    char *k = malloc(strlen(head) + strlen(t) + 1);
    if (k) { strcpy(k, head); strcat(k, t); }
    free(t);
    return k;
}

// ---------------- the lookup ----------------
static void lookup_url(char *u, size_t n, const char *path)
{
    const char *base = getenv("PICOAMP_LRCLIB");
    snprintf(u, n, "%s%s", base && *base ? base : "https://lrclib.net", path);
}

// the questions: 1 the one song (by its artist, title, album and length: LRCLIB's own match),
// 2 a search (the best of what it finds); 0 none (left)
static int question(const job_t *j, int after, char *q, size_t qn)
{
    char a[800], t[800], al[800];
    rd_urlenc(a, sizeof a, j->artist);
    rd_urlenc(t, sizeof t, j->title);
    rd_urlenc(al, sizeof al, j->album);
    if (after < 1 && j->seconds > 0 && j->artist[0])
    {
        snprintf(q, qn, "/api/get?artist_name=%s&track_name=%s&album_name=%s&duration=%d", a, t, al, j->seconds);
        return 1;
    }
    if (after < 2)
    {
        if (j->artist[0]) snprintf(q, qn, "/api/search?track_name=%s&artist_name=%s", t, a);
        else snprintf(q, qn, "/api/search?q=%s", t);
        return 2;
    }
    return 0;
}

// the answer: kept (or that nothing was found), and for the screen when it is still the song asked for
static void finish(job_t *j, char *found, bool answered, const char *err)
{
    ly_set s;
    memset(&s, 0, sizeof s);
    if (found || answered)
    {
        const char *k = found ? found : "none";
        kept_put(j->key, k);
        from_kept(&s, k);
    }
    else
    {
        s.state = LY_FAILED;
        snprintf(s.error, sizeof s.error, "No lyrics: %s", err && err[0] ? err : "LRCLIB did not answer");
    }
    free(found);
    sys_lock(lock);
    if (j->gen == want_gen) { set_free(&came); came = s; came_have = true; memset(&s, 0, sizeof s); }
    sys_unlock(lock);
    set_free(&s);
    free(j);
}

#ifndef __wasm__
static char *ask(const char *path, bool *answered, char *err, size_t errlen)
{
    char u[3200];
    lookup_url(u, sizeof u, path);
    int st = 0;
    // (LRCLIB asks for a User-Agent that names the program)
    net_conn *c = net_get(u, "User-Agent: PicoAmp/" PICOAMP_VERSION " (https://github.com/Jzzy-dev/win-picoamp)", &st, err, errlen, NULL);
    if (!c) { if (st >= 400 && st < 500) *answered = true; return NULL; }     // (404: LRCLIB has nothing)
    *answered = true;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap + 1);
    for (;;)
    {
        if (n == cap)
        {
            if (cap >= REPLY_MAX) break;
            char *nb = realloc(b, cap * 2 + 1);
            if (!nb) break;
            b = nb;
            cap *= 2;
        }
        long r = net_read(c, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    net_close(c);
    b[n] = 0;
    if (st != 200) { free(b); return NULL; }     // (404: not there)
    return b;
}

static int run(void *arg)
{
    job_t *j = arg;
    char q[3000], err[120] = "";
    bool answered = false;
    char *found = NULL;
    for (int k = 0; !found && (k = question(j, k, q, sizeof q)) && j->gen == want_gen; )
    {
        char *r = ask(q, &answered, err, sizeof err);
        if (r) { found = best_of(r, j->seconds); free(r); }
    }
    finish(j, found, answered, err);
    return 0;
}

static bool start(job_t *j)
{
    sys_thread *th = sys_thread_start(run, j);
    if (th) sys_thread_detach(th);
    return th != NULL;
}
#else
// one question, its answer in the callback; then the next or the end
typedef struct { job_t *j; int k; bool answered; } web_ask;
static void web_next(web_ask *w);

static void web_got(void *user, int status, const char *data, size_t len, const char *e)
{
    web_ask *w = user;
    char *found = NULL;
    if (status >= 400 && status < 500) w->answered = true;           // (404: LRCLIB has nothing)
    else if (status == 200 && data)
    {
        w->answered = true;
        char *b = malloc(len + 1);
        if (b) { memcpy(b, data, len); b[len] = 0; found = best_of(b, w->j->seconds); free(b); }
    }
    char err[120] = "";
    if (!status) u_copy(err, sizeof err, e && *e ? e : "no answer");
    else if (status != 200 && status != 404) snprintf(err, sizeof err, "HTTP %d", status);
    if (found || w->j->gen != want_gen) { finish(w->j, found, w->answered, err); free(w); return; }
    if (!w->answered && !status) { finish(w->j, NULL, false, err); free(w); return; }   // (not reached: the second would not be either)
    web_next(w);
}

static void web_next(web_ask *w)
{
    char q[3000], u[3300];
    if (!(w->k = question(w->j, w->k, q, sizeof q))) { finish(w->j, NULL, w->answered, ""); free(w); return; }
    char url[3200];
    lookup_url(url, sizeof url, q);
    snprintf(u, sizeof u, "cors:%s", url);
    net_fetch(u, web_got, w);
}

static bool start(job_t *j)
{
    web_ask *w = calloc(1, sizeof *w);
    if (!w) return false;
    w->j = j;
    web_next(w);
    return true;
}
#endif

void lyrics_want(const char *artist, const char *title, const char *album, int seconds)
{
    if (!lock) lock = sys_mutex_new();
    char key[sizeof want_key];
    // (the key: what the song is; its length to a few seconds, so a station's song and a file's meet)
    snprintf(key, sizeof key, "%s\x1f%s\x1f%s\x1f%d", artist, title, album, seconds > 0 ? (seconds + 2) / 5 : 0);
    for (char *p = key; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (!strcmp(key, want_key)) return;
    u_copy(want_key, sizeof want_key, key);
    sys_lock(lock);
    want_gen++;
    set_free(&came);
    came_have = false;
    sys_unlock(lock);
    set_free(&cur);
    if (!title[0]) { cur.state = LY_NONE; return; }
    // kept already (a "none" only for a week)
    char *t = kept_get(key);
    if (t)
    {
        bool ok = from_kept(&cur, t);
        free(t);
        if (ok) return;
    }
    cur.state = LY_LOADING;
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->gen = want_gen;
    u_copy(j->artist, sizeof j->artist, artist);
    u_copy(j->title, sizeof j->title, title);
    u_copy(j->album, sizeof j->album, album);
    j->seconds = seconds;
    u_copy(j->key, sizeof j->key, key);
    if (!start(j)) { free(j); cur.state = LY_FAILED; u_copy(cur.error, sizeof cur.error, "No lyrics: no thread"); }
}

void lyrics_again(void)
{
    if (!want_key[0]) return;
    kept_forget(want_key);
    want_key[0] = 0;                // (asked again by the next lyrics_want)
}

int lyrics_state(void)
{
    if (lock && cur.state == LY_LOADING)
    {
        sys_lock(lock);
        if (came_have) { set_free(&cur); cur = came; memset(&came, 0, sizeof came); came_have = false; }
        sys_unlock(lock);
    }
    return cur.state;
}

int lyrics_count(void) { return cur.n; }

const char *lyrics_line(int i, int *ms)
{
    if (i < 0 || i >= cur.n) { if (ms) *ms = -1; return ""; }
    if (ms) *ms = cur.l[i].ms;
    return cur.l[i].text;
}

int lyrics_at(int ms)
{
    if (cur.state != LY_SYNCED) return -1;
    int r = -1;
    for (int i = 0; i < cur.n && cur.l[i].ms <= ms; i++) r = i;
    return r;
}

const char *lyrics_error(void) { return cur.error; }
