// newsfeed.c - the news reader's engine (see newsfeed.h).
//
// The XML reader and the HTML-to-text cleaner are the PicoVerse Audio Engine's
// (pva_news.c), with the text kept as UTF-8. The PC holds a whole feed (up
// to 2 MB) and a whole article page (up to 3 MB) in memory, so they are read
// at once, in a thread, instead of in pieces between the radio's turns.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "newsfeed.h"
#include "sys.h"
#include "util.h"
#ifdef __wasm__
#include "web.h"
#else
#include "net.h"
#endif

#define MAX_ITEMS   100
#define FEED_MAX    (2u * 1024u * 1024u)
#define PAGE_MAX    (3u * 1024u * 1024u)
#define FIELD_MAX   (256u * 1024u)

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// ---------------------------------------------------------------- HTML to text
enum { H_TEXT, H_TAG, H_ATTR, H_COMMENT, H_RAW, H_ENT };
typedef struct
{
    char    *out;
    size_t   len, size;
    bool     page;              // a web page: only <p> text, not in menus etc.
    uint8_t  s;
    char     tag[12];
    uint8_t  tl;
    bool     close, quote_d, quote_s;
    char     raw[12];           // "script" while in it
    uint8_t  rm;                // "</script" matched so far
    uint8_t  cm;                // "-->" matched so far
    char     ent[10];
    uint8_t  el;
    bool     sp;                // a space is due before the next character
    size_t   para;              // where the paragraph began
    uint8_t  inp, skip;         // page: in <p>, in a menu/header/footer...
    bool     article, done;
} html_t;

static void h_put(html_t *H, char c)
{
    if (H->len + 2 >= H->size)
    {
        H->size = H->size ? H->size * 2 : 4096;
        H->out = realloc(H->out, H->size);
    }
    H->out[H->len++] = c;
}

static bool h_on(html_t *H) { return !H->done && (!H->page || (H->inp && !H->skip)); }

static void h_emit(html_t *H, char a)
{
    if (!h_on(H)) return;
    if (a == ' ') { if (H->len > H->para) H->sp = true; return; }
    if (H->sp) h_put(H, ' ');
    H->sp = false;
    h_put(H, a);
}

static void h_cp(html_t *H, uint32_t c)
{
    if (c == '\r' || c == '\n' || c == '\t' || c == 0xA0) c = ' ';
    if (c == 0xAD || c == 0x200B || c == 0xFEFF) return;            // soft hyphens, zero width
    if (c < 0x20) return;
    if (c < 0x80) { h_emit(H, (char)c); return; }
    if (!h_on(H)) return;
    if (H->sp) h_put(H, ' ');
    H->sp = false;
    if (c < 0x800) { h_put(H, (char)(0xC0 | (c >> 6))); h_put(H, (char)(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) { h_put(H, (char)(0xE0 | (c >> 12))); h_put(H, (char)(0x80 | ((c >> 6) & 0x3F))); h_put(H, (char)(0x80 | (c & 0x3F))); }
    else { h_put(H, (char)(0xF0 | (c >> 18))); h_put(H, (char)(0x80 | ((c >> 12) & 0x3F))); h_put(H, (char)(0x80 | ((c >> 6) & 0x3F))); h_put(H, (char)(0x80 | (c & 0x3F))); }
}

// the end of a paragraph: short ones on a web page are left out (buttons, captions)
static void h_para(html_t *H)
{
    H->sp = false;
    size_t n = H->len - H->para;
    if (H->page && n && n < 40) H->len = H->para;
    else if (n) h_put(H, '\n');
    H->para = H->len;
}

static bool tag_is(const char *t, const char *const *l)
{
    for (; *l; l++) if (!strcmp(t, *l)) return true;
    return false;
}

static void h_tag(html_t *H)
{
    static const char *const blocks[] = { "p", "br", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "tr", "ul", "ol", "section", "article", "table", "hr", NULL };
    static const char *const raws[] = { "script", "style", "noscript", "svg", "template", "iframe", "head", NULL };
    static const char *const skips[] = { "nav", "header", "footer", "aside", "form", "figure", "figcaption", "button", "label", "select", "menu", NULL };
    H->tag[H->tl] = 0;
    const char *t = H->tag;
    if (!t[0]) return;
    if (!H->close && tag_is(t, raws)) { snprintf(H->raw, sizeof H->raw, "%s", t); H->rm = 0; H->s = H_RAW; return; }
    if (H->page)
    {
        if (tag_is(t, skips)) { if (H->close) { if (H->skip) H->skip--; } else if (H->skip < 200) H->skip++; }
        if (!strcmp(t, "p")) { if (H->close) { h_para(H); if (H->inp) H->inp--; return; } h_para(H); H->inp = 1; return; }
        if (!strcmp(t, "article")) { if (!H->close) H->article = true; else if (H->article && H->len >= 600) H->done = true; }
    }
    if (tag_is(t, blocks)) h_para(H);
}

static void h_entity(html_t *H)
{
    H->ent[H->el] = 0;
    uint32_t cp = 0;
    if (H->ent[0] == '#') cp = H->ent[1] == 'x' || H->ent[1] == 'X' ? (uint32_t)strtoul(H->ent + 2, NULL, 16) : (uint32_t)strtoul(H->ent + 1, NULL, 10);
    else
    {
        static const struct { char n[8]; uint16_t c; } es[] = { { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
            { "apos", '\'' }, { "nbsp", ' ' }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D }, { "ldquo", 0x201C },
            { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "euro", 0x20AC }, { "eacute", 0xE9 },
            { "egrave", 0xE8 }, { "euml", 0xEB }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "szlig", 0xDF },
            { "iuml", 0xEF }, { "ccedil", 0xE7 }, { "aacute", 0xE1 }, { "oacute", 0xF3 }, { "iacute", 0xED },
            { "ntilde", 0xF1 }, { "agrave", 0xE0 }, { "ecirc", 0xEA }, { "ocirc", 0xF4 }, { "acirc", 0xE2 },
            { "Eacute", 0xC9 }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "copy", 0xA9 }, { "shy", 0xAD },
            { "laquo", 0xAB }, { "raquo", 0xBB }, { "pound", 0xA3 }, { "deg", 0xB0 } };
        for (unsigned i = 0; i < sizeof es / sizeof es[0]; i++) if (!strcmp(H->ent, es[i].n)) { cp = es[i].c; break; }
        if (!cp) { h_emit(H, '&'); for (uint8_t i = 0; i < H->el; i++) h_emit(H, H->ent[i]); h_emit(H, ';'); return; }
    }
    h_cp(H, cp);
}

static void h_byte(html_t *H, uint8_t c, uint32_t *u8, uint8_t *u8n)
{
    switch (H->s)
    {
    case H_TEXT:
        if (c == '<') { H->s = H_TAG; H->tl = 0; H->close = false; return; }
        if (c == '&') { H->s = H_ENT; H->el = 0; return; }
        if (c < 0x80) { *u8n = 0; h_cp(H, c); return; }
        if (c >= 0xC0) { *u8 = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F); *u8n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; return; }
        if (*u8n) { *u8 = *u8 << 6 | (c & 0x3F); if (!--*u8n) h_cp(H, *u8); }
        else h_cp(H, c);                                      // (Latin-1)
        return;
    case H_ENT:
        if (c == ';') { H->s = H_TEXT; h_entity(H); return; }
        if (H->el < sizeof H->ent - 1 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#')) { H->ent[H->el++] = (char)c; return; }
        H->s = H_TEXT;
        h_emit(H, '&');
        for (uint8_t i = 0; i < H->el; i++) h_emit(H, H->ent[i]);
        h_byte(H, c, u8, u8n);
        return;
    case H_TAG:
        if (H->tl == 0 && !H->close && c == '/') { H->close = true; return; }
        if (H->tl == 0 && c == '!') { H->s = H_COMMENT; H->cm = 0; return; }
        if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/')
        {
            H->s = c == '>' ? H_TEXT : H_ATTR;
            H->quote_d = H->quote_s = false;
            h_tag(H);
            if (H->s == H_RAW && c != '>') H->s = H_ATTR, H->raw[sizeof H->raw - 1] = 1;
            return;
        }
        if (H->tl < sizeof H->tag - 1) H->tag[H->tl++] = lc((char)c);
        return;
    case H_ATTR:
        if (c == '"' && !H->quote_s) H->quote_d = !H->quote_d;
        else if (c == '\'' && !H->quote_d) H->quote_s = !H->quote_s;
        else if (c == '>' && !H->quote_d && !H->quote_s)
        {
            if (H->raw[sizeof H->raw - 1] == 1) { H->raw[sizeof H->raw - 1] = 0; H->s = H_RAW; H->rm = 0; }
            else H->s = H_TEXT;
        }
        return;
    case H_COMMENT:
        if (H->cm == 0) { H->cm = c == '-' ? 1 : 0xFF; if (c == '>') H->s = H_TEXT; return; }
        if (H->cm == 0xFF) { if (c == '>') H->s = H_TEXT; return; }
        if (H->cm == 1) { H->cm = c == '-' ? 10 : 0xFF; return; }
        if (c == '-') { if (H->cm < 12) H->cm++; return; }
        if (c == '>' && H->cm == 12) { H->s = H_TEXT; return; }
        H->cm = 10;
        return;
    case H_RAW:
    {
        size_t rl = strlen(H->raw);
        char want = H->rm == 0 ? '<' : H->rm == 1 ? '/' : H->raw[H->rm - 2];
        if (lc((char)c) == want) { if (++H->rm == rl + 2) { H->s = H_ATTR; H->quote_d = H->quote_s = false; H->raw[0] = 0; } }
        else H->rm = c == '<' ? 1 : 0;
        return;
    }
    }
}

char *nf_html_text(const char *s, size_t n, bool page)
{
    html_t H;
    memset(&H, 0, sizeof H);
    H.page = page;
    uint32_t u8 = 0;
    uint8_t u8n = 0;
    for (size_t i = 0; i < n && !H.done; i++) h_byte(&H, (uint8_t)s[i], &u8, &u8n);
    h_para(&H);
    while (H.len && (H.out[H.len - 1] == '\n' || H.out[H.len - 1] == ' ')) H.len--;
    h_put(&H, 0);
    return H.out;
}

// ---------------------------------------------------------------- dates
static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static const char wdays[] = "MonTueWedThuFriSatSun";

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// "29 Oct 2026 20:15:00 GMT" / "+0100" / "EST", "2026-10-29T18:30:00Z" / "+01:00" -> UTC seconds (0: not understood)
static int64_t parse_date(const char *s)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    const char *z = NULL;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    if (s[0] >= '0' && s[0] <= '9' && strlen(s) > 10 && s[4] == '-')
    {
        if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) < 5) return 0;
        const char *p = s + 16;
        if (*p == ':') { ss = atoi(p + 1); p += 3; }
        while (*p == '.' || (*p >= '0' && *p <= '9')) p++;
        z = p;
    }
    else
    {
        const char *c = strchr(s, ',');
        if (c && c - s < 5) s = c + 1;
        char mon[4] = "";
        int n = 0;
        if (sscanf(s, " %d %3s %d %d:%d%n", &d, mon, &y, &hh, &mm, &n) < 5) return 0;
        const char *p = s + n;
        if (*p == ':') { ss = atoi(p + 1); p += 3; }
        const char *mp = strstr(mons, mon);
        if (!mon[0] || !mp || (mp - mons) % 3) return 0;
        mo = (int)((mp - mons) / 3 + 1);
        while (*p == ' ') p++;
        z = p;
    }
    if (y < 100) y += 2000;
    if (y < 1990 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mm > 59) return 0;
    int64_t t = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
    int off = 0;
    if (z && (*z == '+' || *z == '-'))
    {
        int h = 0, m = 0;
        if (z[3] == ':') sscanf(z + 1, "%2d:%2d", &h, &m);
        else { int v = atoi(z + 1); h = v / 100; m = v % 100; }
        off = (h * 60 + m) * 60;
        if (*z == '-') off = -off;
    }
    else if (z)
    {
        static const struct { char n[5]; int h; } zs[] = { { "EST", -5 }, { "EDT", -4 }, { "CST", -6 }, { "CDT", -5 },
            { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 }, { "CEST", 2 }, { "CET", 1 }, { "BST", 1 } };
        for (unsigned i = 0; i < sizeof zs / sizeof zs[0]; i++)
            if (!strncmp(z, zs[i].n, strlen(zs[i].n))) { off = zs[i].h * 3600; break; }
    }
    return t - off;
}

void nf_fmt_date(const nf_item *it, char *o, size_t n, bool lng)
{
    if (!it->t)
    {
        snprintf(o, n, "%.*s", lng ? 40 : 6, it->raw ? it->raw : "");
        return;
    }
    time_t tt = (time_t)it->t, now = time(NULL);
    struct tm a, b;
    sys_localtime(tt, &a);
    sys_localtime(now, &b);
    int wd = (a.tm_wday + 6) % 7;
    if (lng) snprintf(o, n, "%.3s %d %.3s %d %02d:%02d", wdays + wd * 3, a.tm_mday, mons + a.tm_mon * 3, a.tm_year + 1900, a.tm_hour, a.tm_min);
    else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(o, n, "%02d:%02d", a.tm_hour, a.tm_min);
    else snprintf(o, n, "%2d %.3s", a.tm_mday, mons + a.tm_mon * 3);
}

// ---------------------------------------------------------------- the feed (XML)
enum { F_NONE, F_TITLE, F_TEXT, F_DATE, F_LINK, F_CONTENT, F_CHTITLE };
enum { X_TEXT, X_TAG, X_CDATA, X_COMMENT, X_SKIP };

typedef struct
{
    nf_item *v;
    int      n;
    char     chtitle[200];
    // the reader
    uint8_t  xs;
    char     tag[40];
    uint8_t  tlen;
    bool     tag_close, tag_skip, tag_link;
    uint8_t  cd_match;
    char     cdpre[9];
    uint8_t  cdpre_n;
    bool     in_item, in_channel;
    int      xitem;
    uint8_t  field;
    char    *fbuf;
    size_t   flen;
    char     ent[10];
    uint8_t  elen;
    bool     in_ent;
    char     attrs[1200];
    size_t   alen;
    // the story being read
    char    *f_title, *f_text, *f_content, f_link[1200], f_raw[60];
    int64_t  f_t;
} feed_t;

static char *clean_field(const char *s, size_t n) { return nf_html_text(s, n, false); }

static void item_start(feed_t *F)
{
    F->in_item = true;
    free(F->f_title); free(F->f_text); free(F->f_content);
    F->f_title = F->f_text = F->f_content = NULL;
    F->f_link[0] = F->f_raw[0] = 0;
    F->f_t = 0;
}

static void item_end(feed_t *F)
{
    F->in_item = false;
    int pos = F->xitem++;
    if (!F->f_title || !F->f_title[0] || F->n >= MAX_ITEMS) return;
    nf_item *it = &F->v[F->n++];
    memset(it, 0, sizeof *it);
    it->title = F->f_title;
    it->text = F->f_text ? F->f_text : strdup("");
    it->content = F->f_content;
    it->link = strdup(F->f_link);
    it->raw = !F->f_t && F->f_raw[0] ? strdup(F->f_raw) : NULL;
    it->t = F->f_t;
    it->pos = pos;
    F->f_title = F->f_text = F->f_content = NULL;
}

static void field_end(feed_t *F)
{
    F->fbuf[F->flen] = 0;
    switch (F->field)
    {
    case F_TITLE:
        if (!F->f_title)
        {
            F->f_title = clean_field(F->fbuf, F->flen);
            for (char *p = F->f_title; *p; p++) if (*p == '\n') *p = ' ';
        }
        break;
    case F_CHTITLE:
        if (!F->chtitle[0])
        {
            char *t = clean_field(F->fbuf, F->flen);
            for (char *p = t; *p; p++) if (*p == '\n') *p = ' ';
            u_copy(F->chtitle, sizeof F->chtitle, t);
            free(t);
        }
        break;
    case F_TEXT: if (!F->f_text) F->f_text = clean_field(F->fbuf, F->flen); break;
    case F_CONTENT: if (!F->f_content) F->f_content = clean_field(F->fbuf, F->flen); break;
    case F_LINK:
        if (!F->f_link[0])
        {
            char *p = F->fbuf;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            size_t k = strlen(p);
            while (k && (p[k - 1] == ' ' || p[k - 1] == '\n' || p[k - 1] == '\r' || p[k - 1] == '\t')) k--;
            snprintf(F->f_link, sizeof F->f_link, "%.*s", (int)k, p);
        }
        break;
    case F_DATE:
        if (!F->f_t)
        {
            F->f_t = parse_date(F->fbuf);
            if (!F->f_t) { char *t = clean_field(F->fbuf, F->flen); u_copy(F->f_raw, sizeof F->f_raw, t); free(t); }
        }
        break;
    }
    F->field = F_NONE;
    F->flen = 0;
}

static void put(feed_t *F, char c)
{
    if (F->field && F->flen < FIELD_MAX) F->fbuf[F->flen++] = c;
}

static void put_cp(feed_t *F, uint32_t cp)
{
    if (cp < 0x80) put(F, (char)cp);
    else if (cp < 0x800) { put(F, (char)(0xC0 | (cp >> 6))); put(F, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { put(F, (char)(0xE0 | (cp >> 12))); put(F, (char)(0x80 | ((cp >> 6) & 0x3F))); put(F, (char)(0x80 | (cp & 0x3F))); }
}

// an XML entity: the character goes on as text (in HTML fields: as HTML, so
// "&lt;b&gt;" is a tag and "&amp;amp;" an ampersand)
static void entity(feed_t *F)
{
    F->ent[F->elen] = 0;
    const char *e = F->ent;
    if (e[0] == '#')
    {
        uint32_t cp = e[1] == 'x' || e[1] == 'X' ? (uint32_t)strtoul(e + 2, NULL, 16) : (uint32_t)strtoul(e + 1, NULL, 10);
        if (cp == '&' && F->field != F_LINK) { put(F, '&'); put(F, 'a'); put(F, 'm'); put(F, 'p'); put(F, ';'); }
        else if (cp == '<' && F->field != F_LINK) { put(F, '&'); put(F, 'l'); put(F, 't'); put(F, ';'); }
        else put_cp(F, cp);
    }
    else if (!strcmp(e, "amp")) put(F, '&');
    else if (!strcmp(e, "lt")) put(F, '<');
    else if (!strcmp(e, "gt")) put(F, '>');
    else if (!strcmp(e, "quot")) put(F, '"');
    else if (!strcmp(e, "apos")) put(F, '\'');
    else { put(F, '&'); for (uint8_t i = 0; i < F->elen; i++) put(F, F->ent[i]); put(F, ';'); }
}

static void text_char(feed_t *F, char c)
{
    if (!F->field) return;
    if (F->in_ent)
    {
        if (c == ';') { F->in_ent = false; entity(F); return; }
        if (F->elen < sizeof F->ent - 1 && c != ' ' && c != '<') { F->ent[F->elen++] = c; return; }
        F->in_ent = false;
        put(F, '&');
        for (uint8_t i = 0; i < F->elen; i++) put(F, F->ent[i]);
    }
    if (c == '&') { F->in_ent = true; F->elen = 0; return; }
    put(F, c);
}

// <link href="..." rel="alternate"/> (Atom)
static void link_attrs(feed_t *F)
{
    F->attrs[F->alen] = 0;
    const char *r = strstr(F->attrs, "rel=");
    if (r && strncmp(r + 5, "alternate", 9)) return;
    const char *h = strstr(F->attrs, "href=");
    if (!h || (h[5] != '"' && h[5] != '\'')) return;
    char q = h[5];
    const char *b = h + 6, *e = strchr(b, q);
    if (!e || F->f_link[0]) return;
    snprintf(F->f_link, sizeof F->f_link, "%.*s", (int)(e - b), b);
    // (&amp; in the address)
    char *a;
    while ((a = strstr(F->f_link, "&amp;"))) memmove(a + 1, a + 5, strlen(a + 5) + 1);
}

static void tag_done(feed_t *F)
{
    F->tag[F->tlen] = 0;
    const char *t = strchr(F->tag, ':');
    bool ns = t != NULL;
    t = t ? t + 1 : F->tag;
    F->tag_link = false;
    if (!F->tag_close)
    {
        if (!strcmp(t, "item") || !strcmp(t, "entry")) item_start(F);
        else if (!strcmp(t, "channel") || !strcmp(t, "feed")) F->in_channel = true;
        else if (F->in_item && !F->tag_skip)
        {
            uint8_t f = !strcmp(t, "title") ? F_TITLE : (!strcmp(t, "description") || !strcmp(t, "summary")) ? F_TEXT :
                        (!strcmp(t, "pubDate") || !strcmp(t, "date") || !strcmp(t, "updated") || !strcmp(t, "published")) ? F_DATE :
                        (!strcmp(t, "link") && !ns) ? F_LINK :
                        (!strcmp(t, "encoded") || (!strcmp(t, "content") && !ns)) ? F_CONTENT : F_NONE;
            if (f == F_DATE && F->f_t) f = F_NONE;
            if (f) { F->field = f; F->flen = 0; F->in_ent = false; }
            if (f == F_LINK) { F->tag_link = true; F->alen = 0; }
        }
        else if (!F->in_item && F->in_channel && !F->tag_skip && !strcmp(t, "title") && !F->chtitle[0])
        {
            F->field = F_CHTITLE;
            F->flen = 0;
            F->in_ent = false;
        }
    }
    else
    {
        if (F->field) field_end(F);
        if (!strcmp(t, "item") || !strcmp(t, "entry")) item_end(F);
    }
}

static void xml_byte(feed_t *F, char c)
{
    switch (F->xs)
    {
    case X_TEXT:
        if (c == '<') { F->xs = X_TAG; F->tlen = 0; F->tag_close = false; F->tag_skip = false; F->cdpre_n = 0; return; }
        text_char(F, c);
        return;
    case X_TAG:
        if (F->tlen == 0 && !F->tag_close && F->cdpre_n == 0 && c == '/') { F->tag_close = true; return; }
        if (F->tlen == 0 && !F->tag_close && (c == '!' || F->cdpre_n))
        {
            if (F->cdpre_n < 8) F->cdpre[F->cdpre_n++] = c;
            F->cdpre[F->cdpre_n] = 0;
            if (!strcmp(F->cdpre, "![CDATA[")) { F->xs = X_CDATA; F->cd_match = 0; return; }
            if (!strcmp(F->cdpre, "!--")) { F->xs = X_COMMENT; F->cd_match = 0; return; }
            if (strncmp("![CDATA[", F->cdpre, F->cdpre_n) && strncmp("!--", F->cdpre, F->cdpre_n)) F->xs = X_SKIP;
            return;
        }
        if (c == '?') { F->xs = X_SKIP; return; }
        if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/')
        {
            if (c == '/' && !F->tag_close) F->tag_skip = true;
            tag_done(F);
            if (c == '>') { F->xs = X_TEXT; if (F->tag_skip && F->field) field_end(F); }
            else F->xs = X_SKIP;
            return;
        }
        if (F->tlen < sizeof F->tag - 1) F->tag[F->tlen++] = c;
        return;
    case X_SKIP:
        if (F->tag_link && F->alen < sizeof F->attrs - 1) F->attrs[F->alen++] = c;
        if (c == '/') F->tag_skip = true;
        else if (c == '>')
        {
            F->xs = X_TEXT;
            if (F->tag_link) { link_attrs(F); F->tag_link = false; }
            if (F->tag_skip && F->field && !F->tag_close) field_end(F);
        }
        else if (c != ' ') F->tag_skip = false;
        return;
    case X_CDATA:
        if (c == ']') { if (F->cd_match < 2) F->cd_match++; else put(F, ']'); return; }
        if (c == '>' && F->cd_match == 2) { F->xs = X_TEXT; return; }
        while (F->cd_match) { put(F, ']'); F->cd_match--; }
        put(F, c);
        return;
    case X_COMMENT:
        if (c == '-') { if (F->cd_match < 2) F->cd_match++; return; }
        if (c == '>' && F->cd_match == 2) { F->xs = X_TEXT; return; }
        F->cd_match = 0;
        return;
    }
}

// ---------------------------------------------------------------- the threads
static sys_mutex *lock;
static nf_item   *items;
static int        n_items, state = NF_IDLE;
static char       err[200], feed_title[200];
static unsigned   gen;
static bool       by_date = true;

static int        a_state = NF_IDLE;
static char      *a_text;
static size_t     a_bytes;
static unsigned   a_gen;

static void init(void) { if (!lock) lock = sys_mutex_new(); }

static void free_items(nf_item *v, int n)
{
    for (int i = 0; i < n; i++) { free(v[i].title); free(v[i].text); free(v[i].link); free(v[i].raw); free(v[i].content); }
    free(v);
}

static int cmp_date(const void *a, const void *b)
{
    const nf_item *x = a, *y = b;
    if (x->t || y->t)
    {
        if (!x->t) return 1;
        if (!y->t) return -1;
        if (x->t != y->t) return x->t > y->t ? -1 : 1;
    }
    return x->pos - y->pos;
}

static int cmp_pos(const void *a, const void *b) { return ((const nf_item *)a)->pos - ((const nf_item *)b)->pos; }

void nf_sort(bool d)
{
    init();
    by_date = d;
    sys_lock(lock);
    if (items) qsort(items, (size_t)n_items, sizeof *items, d ? cmp_date : cmp_pos);
    sys_unlock(lock);
}

typedef struct { char url[1200]; unsigned gen; } job_t;

#ifndef __wasm__
static char *fetch_all(const char *url, size_t max, size_t *len, char *e, size_t en, unsigned *g, unsigned want, size_t *progress)
{
    int st;
    // (Yahoo Finance's feeds answer a browser's User-Agent)
    const char *ua = strstr(url, "yahoo.com/") ? "User-Agent: Mozilla/5.0 (compatible; PicoAmp for Windows)" : NULL;
    net_conn *c = net_get(url, ua, &st, e, en, NULL);
    if (!c) return NULL;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap + 1);
    long r;
    while (n < max && *g == want && (r = net_read(c, b + n, cap - n)) > 0)
    {
        n += (size_t)r;
        if (progress) *progress = n;
        if (n == cap) { cap *= 2; b = realloc(b, cap + 1); }
    }
    net_close(c);
    b[n] = 0;
    *len = n;
    return b;
}

#endif

// a feed's answer (body NULL: none, e why)
static void feed_done(job_t *jb, char *body, size_t len, const char *e)
{
    feed_t *F = calloc(1, sizeof *F);
    F->v = calloc(MAX_ITEMS, sizeof *F->v);
    F->fbuf = malloc(FIELD_MAX + 1);
    if (body) for (size_t i = 0; i < len; i++) xml_byte(F, body[i]);
    free(body);
    item_start(F);                       // (frees a story that did not end)
    free(F->fbuf);
    sys_lock(lock);
    if (jb->gen == gen)
    {
        free_items(items, n_items);
        items = F->v;
        n_items = F->n;
        u_copy(feed_title, sizeof feed_title, F->chtitle);
        if (body || F->n) { state = NF_DONE; err[0] = 0; }
        else { state = NF_FAILED; u_copy(err, sizeof err, e[0] ? e : "The feed did not come"); }
        if (items) qsort(items, (size_t)n_items, sizeof *items, by_date ? cmp_date : cmp_pos);
    }
    else free_items(F->v, F->n);
    sys_unlock(lock);
    free(F);
    free(jb);
}

// a story's page's answer
static void art_done(job_t *jb, char *page, size_t len)
{
    char *t = page ? nf_html_text(page, len, true) : NULL;
    free(page);
    sys_lock(lock);
    if (jb->gen == a_gen)
    {
        free(a_text);
        a_text = t;
        a_state = t && strlen(t) >= 80 ? NF_DONE : NF_FAILED;
        t = NULL;
    }
    sys_unlock(lock);
    free(t);
    free(jb);
}

#ifndef __wasm__
static int feed_run(void *arg)
{
    job_t *jb = arg;
    char e[200] = "";
    size_t len = 0;
    char *body = fetch_all(jb->url, FEED_MAX, &len, e, sizeof e, &gen, jb->gen, NULL);
    feed_done(jb, body, len, e);
    return 0;
}

static int art_run(void *arg)
{
    job_t *jb = arg;
    char e[200] = "";
    size_t len = 0;
    char *page = fetch_all(jb->url, PAGE_MAX, &len, e, sizeof e, &a_gen, jb->gen, &a_bytes);
    art_done(jb, page, len);
    return 0;
}
#define START_FEED(jb) sys_thread_detach(sys_thread_start(feed_run, jb))
#define START_ART(jb)  sys_thread_detach(sys_thread_start(art_run, jb))
#else
// the browser: the sites do not let other pages read them, so through the relay ("relay:",
// web/picoamp.js); the answer comes in a callback (a copy, 0-terminated, cut to max)
static char *web_copy(int status, const char *data, size_t len, size_t max, size_t *out_len, const char *e, char *m, size_t mn)
{
    if (status < 200 || status >= 300 || !data)
    {
        if (status) snprintf(m, mn, "HTTP %d", status);
        else u_copy(m, mn, e && *e ? e : "no answer");
        return NULL;
    }
    if (len > max) len = max;
    char *b = malloc(len + 1);
    memcpy(b, data, len);
    b[len] = 0;
    *out_len = len;
    return b;
}

static void feed_got(void *user, int status, const char *data, size_t len, const char *e)
{
    char m[200] = "";
    size_t n = 0;
    char *b = web_copy(status, data, len, FEED_MAX, &n, e, m, sizeof m);
    feed_done(user, b, n, m);
}

static void art_got(void *user, int status, const char *data, size_t len, const char *e)
{
    char m[200] = "";
    size_t n = 0;
    char *b = web_copy(status, data, len, PAGE_MAX, &n, e, m, sizeof m);
    a_bytes = n;
    art_done(user, b, n);
}

static void web_start(job_t *jb, net_cb cb)
{
    char u[1300];
    snprintf(u, sizeof u, "relay:%s", jb->url);
    net_fetch(u, cb, jb);
}
#define START_FEED(jb) web_start(jb, feed_got)
#define START_ART(jb)  web_start(jb, art_got)
#endif

void nf_fetch(const char *url)
{
    init();
    job_t *jb = calloc(1, sizeof *jb);
    u_copy(jb->url, sizeof jb->url, url);
    sys_lock(lock);
    jb->gen = ++gen;
    a_gen++;
    state = NF_BUSY;
    a_state = NF_IDLE;
    feed_title[0] = 0;
    sys_unlock(lock);
    START_FEED(jb);
}

int nf_state(void) { init(); sys_lock(lock); int s = state; sys_unlock(lock); return s; }
unsigned nf_serial(void) { init(); sys_lock(lock); unsigned g = (unsigned)gen; sys_unlock(lock); return g; }
const char *nf_error(void) { return err; }
const char *nf_feed_title(void) { return feed_title; }
nf_item *nf_items(int *count) { *count = n_items; return items; }

void nf_article(int i)
{
    init();
    sys_lock(lock);
    a_gen++;
    free(a_text);
    a_text = NULL;
    a_bytes = 0;
    if (i < 0 || i >= n_items) { a_state = NF_FAILED; sys_unlock(lock); return; }
    nf_item *it = &items[i];
    // the feed's own text, when it is more than the summary
    if (it->content && strlen(it->content) > strlen(it->text) + 400)
    {
        a_text = strdup(it->content);
        a_state = NF_DONE;
        sys_unlock(lock);
        return;
    }
    if (!u_is_url(it->link)) { a_state = NF_FAILED; sys_unlock(lock); return; }
    job_t *jb = calloc(1, sizeof *jb);
    u_copy(jb->url, sizeof jb->url, it->link);
    jb->gen = a_gen;
    a_state = NF_BUSY;
    sys_unlock(lock);
    START_ART(jb);
}

int nf_article_state(void) { init(); sys_lock(lock); int s = a_state; sys_unlock(lock); return s; }
const char *nf_article_text(void) { return a_text ? a_text : ""; }
size_t nf_article_bytes(void) { return a_bytes; }

// ---------------------------------------------------------------- My feeds (OPML)
#include "config.h"
static nf_feed *favs;
static int      n_favs = -1;

static const char *opml_path(void)
{
    static char p[1200];
    u_join(p, sizeof p, sys_config_dir(), "MyFeeds.opml");
    return p;
}

static void xml_esc(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++)
    {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (r) { size_t l = strlen(r); memcpy(out + o, r, l); o += l; }
        else out[o++] = *s;
    }
    out[o] = 0;
}

static void favs_save(void)
{
    size_t cap = 512 + (size_t)n_favs * 2000, o = 0;
    char *b = malloc(cap);
    o += (size_t)snprintf(b + o, cap - o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<opml version=\"2.0\">\r\n"
                          "  <head><title>PicoAmp: My feeds</title></head>\r\n  <body>\r\n");
    for (int i = 0; i < n_favs; i++)
    {
        char nm[400], url[1800];
        xml_esc(nm, sizeof nm, favs[i].name);
        xml_esc(url, sizeof url, favs[i].url);
        o += (size_t)snprintf(b + o, cap - o, "    <outline type=\"rss\" text=\"%s\" title=\"%s\" xmlUrl=\"%s\"/>\r\n", nm, nm, url);
    }
    o += (size_t)snprintf(b + o, cap - o, "  </body>\r\n</opml>\r\n");
    sys_mkdir(sys_config_dir());
    sys_write_file(opml_path(), b, o);
    free(b);
}

// an attribute's value in a tag (entities decoded)
static bool attr(const char *tag, const char *name, char *out, size_t n)
{
    char key[40];
    snprintf(key, sizeof key, " %s=", name);
    const char *p = tag;
    while ((p = strstr(p, key)))
    {
        p += strlen(key);
        char q = *p;
        if (q != '"' && q != '\'') continue;
        const char *e = strchr(p + 1, q);
        if (!e) return false;
        char *t = nf_html_text(p + 1, (size_t)(e - p - 1), false);
        u_copy(out, n, t);
        free(t);
        return true;
    }
    return false;
}

static int opml_read(const char *text, bool add)
{
    int added = 0;
    for (const char *p = text; (p = strstr(p, "<outline")); )
    {
        const char *e = strchr(p, '>');
        if (!e) break;
        char tag[3000];
        snprintf(tag, sizeof tag, " %.*s", (int)(e - p - 8), p + 8);
        for (char *q = tag; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
        char name[120] = "", url[600] = "";
        if (attr(tag, "xmlUrl", url, sizeof url) && u_is_url(url))
        {
            if (!attr(tag, "text", name, sizeof name) || !name[0]) attr(tag, "title", name, sizeof name);
            if (!name[0]) u_copy(name, sizeof name, url);
            if (!nf_fav_has(url))
            {
                favs = realloc(favs, sizeof *favs * (size_t)(n_favs + 1));
                u_copy(favs[n_favs].name, sizeof favs[0].name, name);
                u_copy(favs[n_favs].url, sizeof favs[0].url, url);
                n_favs++;
                added++;
            }
        }
        p = e;
    }
    (void)add;
    return added;
}

static void favs_load(void)
{
    if (n_favs >= 0) return;
    n_favs = 0;
    char *t = sys_read_file(opml_path(), NULL);
    if (t) { opml_read(t, false); free(t); return; }
    // the first time: as the engine without [news] feeds
    static const char *const def[] = { "bbcw", "alj", "dw", "guardw", "nos" };
    for (unsigned i = 0; i < sizeof def / sizeof def[0]; i++)
        for (int c = 0; nf_catalogue[c].name; c++)
            if (!strcmp(nf_catalogue[c].code, def[i]))
            {
                favs = realloc(favs, sizeof *favs * (size_t)(n_favs + 1));
                u_copy(favs[n_favs].name, sizeof favs[0].name, nf_catalogue[c].name);
                u_copy(favs[n_favs].url, sizeof favs[0].url, nf_catalogue[c].url);
                n_favs++;
            }
    favs_save();
}

int nf_favs(nf_feed **out) { favs_load(); *out = favs; return n_favs; }

bool nf_fav_has(const char *url)
{
    for (int i = 0; i < n_favs; i++) if (!strcmp(favs[i].url, url)) return true;
    return false;
}

bool nf_fav_add(const char *name, const char *url)
{
    favs_load();
    if (nf_fav_has(url)) return false;
    favs = realloc(favs, sizeof *favs * (size_t)(n_favs + 1));
    u_copy(favs[n_favs].name, sizeof favs[0].name, name && *name ? name : url);
    u_copy(favs[n_favs].url, sizeof favs[0].url, url);
    n_favs++;
    favs_save();
    return true;
}

void nf_fav_del(int i)
{
    favs_load();
    if (i < 0 || i >= n_favs) return;
    memmove(favs + i, favs + i + 1, sizeof *favs * (size_t)(n_favs - i - 1));
    n_favs--;
    favs_save();
}

void nf_fav_move(int i, int dir)
{
    favs_load();
    int j = i + dir;
    if (i < 0 || i >= n_favs || j < 0 || j >= n_favs) return;
    nf_feed t = favs[i];
    favs[i] = favs[j];
    favs[j] = t;
    favs_save();
}

int nf_import_opml(const char *path)
{
    favs_load();
    char *t = sys_read_file(path, NULL);
    if (!t) return -1;
    int n = opml_read(t, true);
    free(t);
    if (n) favs_save();
    return n;
}

// ---------------------------------------------------------------- the catalogue
// From the engine (pva_news_cat.c): gathered from the lists at
// opentweet.io/news-rss-feeds, feeder.co, rssly.io/feeds/news and
// rss.feedspot.com, plus the feeds of NOS and a few Dutch, European and retro
// computing sites.
const char *const nf_topics[] = {
    "World news",
    "UK and Europe",
    "Nederland",
    "Deutsch, Francais, Espanol",
    "US news",
    "Business",
    "Technology",
    "Science and space",
    "Sports",
    "Games",
    "MSX and retro computing",
    NULL
};

// code, name, URL
const nf_cat nf_catalogue[] = {
    // 0 World news
    { 0, "bbcw",    "BBC News, World",            "https://feeds.bbci.co.uk/news/world/rss.xml" },
    { 0, "bbc",     "BBC News, top stories",      "https://feeds.bbci.co.uk/news/rss.xml" },
    { 0, "alj",     "Al Jazeera",                 "https://www.aljazeera.com/xml/rss/all.xml" },
    { 0, "dw",      "DW (English)",               "https://rss.dw.com/xml/rss-en-all" },
    { 0, "guardw",  "The Guardian, World",        "https://www.theguardian.com/world/rss" },
    { 0, "nytw",    "New York Times, World",      "https://rss.nytimes.com/services/xml/rss/nyt/World.xml" },
    { 0, "skyw",    "Sky News, World",            "https://feeds.skynews.com/feeds/rss/world.xml" },
    { 0, "npr",     "NPR News",                   "https://feeds.npr.org/1001/rss.xml" },
    { 0, "f24",     "France 24 (English)",        "https://www.france24.com/en/rss" },
    { 0, "cbc",     "CBC News",                   "https://www.cbc.ca/webfeed/rss/rss-topstories" },
    { 0, "abcau",   "ABC News (Australia)",       "https://www.abc.net.au/news/feed/45910/rss.xml" },
    { 0, "scmp",    "South China Morning Post",   "https://www.scmp.com/rss/91/feed/" },
    { 0, "toi",     "The Times of India",         "https://timesofindia.indiatimes.com/rssfeedstopstories.cms" },
    { 0, "gnw",     "Google News, World",         "https://news.google.com/rss/headlines/section/topic/WORLD?hl=en-US&gl=US&ceid=US:en" },
    // 1 UK and Europe
    { 1, "bbcuk",   "BBC News, UK",               "https://feeds.bbci.co.uk/news/uk/rss.xml" },
    { 1, "guarduk", "The Guardian, UK",           "https://www.theguardian.com/uk/rss" },
    { 1, "indep",   "The Independent",            "https://www.independent.co.uk/rss" },
    { 1, "sky",     "Sky News",                   "https://feeds.skynews.com/feeds/rss/home.xml" },
    { 1, "rte",     "RTE News",                   "https://www.rte.ie/feeds/rss/?index=/news/" },
    { 1, "euron",   "Euronews",                   "https://www.euronews.com/rss" },
    { 1, "polieu",  "Politico Europe",            "https://www.politico.eu/feed/" },
    { 1, "lmen",    "Le Monde in English",        "https://www.lemonde.fr/en/rss/une.xml" },
    { 1, "spint",   "Der Spiegel International",  "https://www.spiegel.de/international/index.rss" },
    // 2 Nederland
    { 2, "nos",     "NOS Nieuws",                 "https://feeds.nos.nl/nosnieuwsalgemeen" },
    { 2, "nosbi",   "NOS Binnenland",             "https://feeds.nos.nl/nosnieuwsbinnenland" },
    { 2, "nosbu",   "NOS Buitenland",             "https://feeds.nos.nl/nosnieuwsbuitenland" },
    { 2, "nospol",  "NOS Politiek",               "https://feeds.nos.nl/nosnieuwspolitiek" },
    { 2, "noseco",  "NOS Economie",               "https://feeds.nos.nl/nosnieuwseconomie" },
    { 2, "nostech", "NOS Tech",                   "https://feeds.nos.nl/nosnieuwstech" },
    { 2, "nosopm",  "NOS Opmerkelijk",            "https://feeds.nos.nl/nosnieuwsopmerkelijk" },
    { 2, "noscul",  "NOS Cultuur en media",       "https://feeds.nos.nl/nosnieuwscultuurenmedia" },
    { 2, "nuus",    "Nieuwsuur",                  "https://feeds.nos.nl/nieuwsuuralgemeen" },
    { 2, "jeugd",   "NOS Jeugdjournaal",          "https://feeds.nos.nl/jeugdjournaal" },
    { 2, "nu",      "NU.nl",                      "https://www.nu.nl/rss/Algemeen" },
    { 2, "tweak",   "Tweakers",                   "https://tweakers.net/feeds/mixed.xml" },
    { 2, "tweakn",  "Tweakers, nieuws",           "https://tweakers.net/feeds/nieuws.xml" },
    // 3 Deutsch, Francais, Espanol, Italiano
    { 3, "tages",   "tagesschau",                 "https://www.tagesschau.de/index~rss2.xml" },
    { 3, "spiegel", "Der Spiegel",                "https://www.spiegel.de/schlagzeilen/index.rss" },
    { 3, "dwde",    "DW (Deutsch)",               "https://rss.dw.com/xml/rss-de-all" },
    { 3, "lemonde", "Le Monde, a la une",         "https://www.lemonde.fr/rss/une.xml" },
    { 3, "finfo",   "franceinfo, les titres",     "https://www.francetvinfo.fr/titres.rss" },
    { 3, "f24fr",   "France 24 (Francais)",       "https://www.france24.com/fr/rss" },
    { 3, "elpais",  "El Pais, portada",           "https://feeds.elpais.com/mrss-s/pages/ep/site/elpais.com/portada" },
    { 3, "mundo",   "BBC Mundo",                  "https://feeds.bbci.co.uk/mundo/rss.xml" },
    { 3, "ansa",    "ANSA, ultima ora",           "https://www.ansa.it/sito/notizie/topnews/topnews_rss.xml" },
    // 4 US news
    { 4, "nyt",     "New York Times",             "https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml" },
    { 4, "cbs",     "CBS News",                   "https://www.cbsnews.com/latest/rss/main" },
    { 4, "nbc",     "NBC News",                   "https://feeds.nbcnews.com/nbcnews/public/news" },
    { 4, "pbs",     "PBS News Hour",              "https://www.pbs.org/newshour/feeds/rss/headlines" },
    { 4, "abc",     "ABC News",                   "https://abcnews.go.com/abcnews/topstories" },
    { 4, "lat",     "Los Angeles Times",          "https://www.latimes.com/index.rss" },
    { 4, "time",    "TIME",                       "https://time.com/feed/" },
    { 4, "propub",  "ProPublica",                 "https://www.propublica.org/feeds/propublica/main" },
    { 4, "atlan",   "The Atlantic",               "https://www.theatlantic.com/feed/all/" },
    { 4, "vox",     "Vox",                        "https://www.vox.com/rss/index.xml" },
    { 4, "politi",  "Politico",                   "https://rss.politico.com/politics-news.xml" },
    { 4, "hill",    "The Hill",                   "https://thehill.com/news/feed/" },
    { 4, "nprpol",  "NPR Politics",               "https://feeds.npr.org/1014/rss.xml" },
    // 5 Business
    { 5, "bbcbiz",  "BBC News, Business",         "https://feeds.bbci.co.uk/news/business/rss.xml" },
    { 5, "wsjm",    "WSJ, Markets",               "https://feeds.content.dowjones.io/public/rss/RSSMarketsMain" },
    { 5, "mw",      "MarketWatch",                "https://feeds.content.dowjones.io/public/rss/mw_topstories" },
    { 5, "cnbc",    "CNBC, top news",             "https://www.cnbc.com/id/100003114/device/rss/rss.html" },
    { 5, "ft",      "Financial Times",            "https://www.ft.com/rss/home/international" },
    { 5, "econfin", "The Economist, Finance",     "https://www.economist.com/finance-and-economics/rss.xml" },
    { 5, "fortune", "Fortune",                    "https://fortune.com/feed/fortune-feeds/?id=3230629" },
    { 5, "bi",      "Business Insider",           "https://feeds.businessinsider.com/custom/all" },
    { 5, "fastco",  "Fast Company",               "https://www.fastcompany.com/latest/rss" },
    // 6 Technology
    { 6, "ars",     "Ars Technica",               "https://feeds.arstechnica.com/arstechnica/index" },
    { 6, "verge",   "The Verge",                  "https://www.theverge.com/rss/index.xml" },
    { 6, "tc",      "TechCrunch",                 "https://techcrunch.com/feed/" },
    { 6, "wired",   "Wired",                      "https://www.wired.com/feed/rss" },
    { 6, "engadg",  "Engadget",                   "https://www.engadget.com/rss.xml" },
    { 6, "hn",      "Hacker News, front page",    "https://hnrss.org/frontpage" },
    { 6, "techme",  "Techmeme",                   "https://www.techmeme.com/feed.xml" },
    { 6, "mittr",   "MIT Technology Review",      "https://www.technologyreview.com/feed/" },
    { 6, "ieee",    "IEEE Spectrum",              "https://feeds.feedburner.com/IeeeSpectrumFullText" },
    { 6, "bleep",   "BleepingComputer",           "https://www.bleepingcomputer.com/feed/" },
    { 6, "krebs",   "Krebs on Security",          "https://krebsonsecurity.com/feed/" },
    { 6, "bbctech", "BBC News, Technology",       "https://feeds.bbci.co.uk/news/technology/rss.xml" },
    // 7 Science and space
    { 7, "nasa",    "NASA news releases",         "https://www.nasa.gov/news-release/feed/" },
    { 7, "scid",    "ScienceDaily",               "https://www.sciencedaily.com/rss/all.xml" },
    { 7, "phys",    "Phys.org",                   "https://phys.org/rss-feed/" },
    { 7, "newsci",  "New Scientist",              "https://www.newscientist.com/feed/" },
    { 7, "space",   "Space.com",                  "https://www.space.com/feeds/all" },
    { 7, "quanta",  "Quanta Magazine",            "https://www.quantamagazine.org/feed/" },
    { 7, "sciam",   "Scientific American",        "https://www.scientificamerican.com/platform/syndication/rss/" },
    { 7, "nature",  "Nature",                     "https://www.nature.com/nature.rss" },
    { 7, "bbcsci",  "BBC Science, environment",   "https://feeds.bbci.co.uk/news/science_and_environment/rss.xml" },
    // 8 Sports
    { 8, "bbcsp",   "BBC Sport",                  "https://feeds.bbci.co.uk/sport/rss.xml" },
    { 8, "bbcfb",   "BBC Sport, Football",        "https://feeds.bbci.co.uk/sport/football/rss.xml" },
    { 8, "nossp",   "NOS Sport",                  "https://feeds.nos.nl/nossportalgemeen" },
    { 8, "nosvb",   "NOS Voetbal",                "https://feeds.nos.nl/nosvoetbal" },
    { 8, "nosf1",   "NOS Formule 1",              "https://feeds.nos.nl/nossportformule1" },
    { 8, "nosws",   "NOS Wielrennen",             "https://feeds.nos.nl/nossportwielrennen" },
    { 8, "guardsp", "The Guardian, Sport",        "https://www.theguardian.com/uk/sport/rss" },
    { 8, "skysp",   "Sky Sports",                 "https://www.skysports.com/rss/12040" },
    { 8, "espn",    "ESPN, top headlines",        "https://www.espn.com/espn/rss/news" },
    { 8, "f1",      "Formula 1",                  "https://www.formula1.com/en/latest/all.xml" },
    { 8, "cric",    "ESPNcricinfo",               "https://www.cricinfo.com/rss/content/story/feeds/0.xml" },
    // 9 Games
    { 9, "ign",     "IGN",                        "https://www.ign.com/rss/articles/feed" },
    { 9, "gspot",   "GameSpot",                   "https://www.gamespot.com/feeds/mashup/" },
    { 9, "pcg",     "PC Gamer",                   "https://www.pcgamer.com/rss/" },
    { 9, "rps",     "Rock Paper Shotgun",         "https://www.rockpapershotgun.com/feed" },
    { 9, "eurog",   "Eurogamer",                  "https://www.eurogamer.net/feed" },
    { 9, "nlife",   "Nintendo Life",              "https://www.nintendolife.com/feeds/latest" },
    { 9, "kotaku",  "Kotaku",                     "https://kotaku.com/feed" },
    // 10 MSX and retro computing
    { 10, "mrc",    "MSX Resource Center",        "https://www.msx.org/feed/news" },
    { 10, "timext", "Time Extension",             "https://www.timeextension.com/feeds/latest" },
    { 10, "hackad", "Hackaday",                   "https://hackaday.com/blog/feed/" },
    { 10, "vinold", "Vintage is the New Old",     "https://www.vintageisthenewold.com/feed" },
    { 10, "hnrss",  "Hacker News, best",          "https://hnrss.org/newest?points=100" },
    { -1, NULL, NULL, NULL }
};

