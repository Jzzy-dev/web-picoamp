// library.c - folders and station lists (see library.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "library.h"
#include "config.h"
#include "sys.h"
#include "util.h"
#include "radiodir.h"
#include "web.h"

#define FAV_FILE "Favourites.m3u"

static const char default_stations[] =
    "#EXTM3U\r\n"
    "#EXTINF:-1,Radio Paradise (MP3 128k)\r\nhttps://stream.radioparadise.com/mp3-128\r\n"
    "#EXTINF:-1,SomaFM Groove Salad (MP3 128k)\r\nhttps://ice1.somafm.com/groovesalad-128-mp3\r\n"
    "#EXTINF:-1,SomaFM Secret Agent (MP3 128k)\r\nhttps://ice1.somafm.com/secretagent-128-mp3\r\n"
    "#EXTINF:-1,SomaFM Drone Zone (MP3 128k)\r\nhttps://ice1.somafm.com/dronezone-128-mp3\r\n"
    "#EXTINF:-1,SomaFM DEF CON Radio (MP3 128k)\r\nhttps://ice1.somafm.com/defcon-128-mp3\r\n";

static const char readme[] =
    "PicoAmp for the web - station lists\r\n"
    "\r\n"
    "Every list here is a list under Internet radio. O opens this editor;\r\n"
    "a list is plain .m3u text:\r\n"
    "\r\n"
    "  #EXTM3U\r\n"
    "  #EXTINF:-1,Name of the station\r\n"
    "  https://address.of/the/stream\r\n"
    "\r\n"
    "Favourites is written by PicoAmp itself (F adds, D removes).\r\n"
    "Dropping an .m3u or .pls file on the page adds it as a list.\r\n";

bool lib_is_music(const char *name)
{
    static const char *const ext[] = { ".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".webm" };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++) if (u_ends_with_ci(name, ext[i])) return true;
    return false;
}

static bool is_list_file(const char *name)
{
    return u_ends_with_ci(name, ".m3u") || u_ends_with_ci(name, ".m3u8") || u_ends_with_ci(name, ".pls");
}

void lib_init(void)
{
    const char *rd = config_radio_dir();
    sys_mkdir(rd);
    char p[2200];
    u_join(p, sizeof p, rd, "Stations.m3u");
    if (cfg.first_run && !sys_file_exists(p))
    {
        u_join(p, sizeof p, rd, "Stations.m3u");
        sys_write_file(p, default_stations, sizeof default_stations - 1);
        u_join(p, sizeof p, rd, "README.txt");
        sys_write_file(p, readme, sizeof readme - 1);
    }
    u_join(p, sizeof p, rd, FAV_FILE);
    if (!sys_file_exists(p)) sys_write_file(p, "#EXTM3U\r\n", 9);
}

static void add(lib_list *l, int type, const char *name, const char *target)
{
    if (l->count % 64 == 0) l->e = realloc(l->e, sizeof *l->e * (size_t)(l->count + 64));
    lib_entry *e = &l->e[l->count++];
    memset(e, 0, sizeof *e);
    e->type = type;
    u_copy(e->name, sizeof e->name, name);
    u_copy(e->target, sizeof e->target, target);
    e->tls = type == E_STATION && !u_strnicmp(target, "https:", 6);
}

static int cmp_entry(const void *a, const void *b)
{
    const lib_entry *x = a, *y = b;
    if ((x->type == E_PARENT) != (y->type == E_PARENT)) return x->type == E_PARENT ? -1 : 1;
    if ((x->type == E_FOLDER) != (y->type == E_FOLDER)) return x->type == E_FOLDER ? -1 : 1;
    return u_stricmp(x->name, y->name);
}

// the stations of an .m3u or .pls text
typedef void (*station_fn)(void *ctx, const char *name, const char *url);

static void parse_list(char *text, station_fn fn, void *ctx)
{
    char name[256] = "";
    bool pls = strstr(text, "[playlist]") != NULL || strstr(text, "[Playlist]") != NULL;
    // .pls: FileN= and TitleN= may come in any order
    char *pls_file[200] = { 0 }, *pls_title[200] = { 0 };
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;
    char *cur = text;
    for (char *line; (line = u_token(&cur, "\r\n")); )
    {
        line = u_trim(line);
        if (!*line) continue;
        if (pls)
        {
            int idx = 0;
            if (!u_strnicmp(line, "File", 4) && (idx = atoi(line + 4)) > 0 && idx < 200)
                { char *eq = strchr(line, '='); if (eq) pls_file[idx] = u_trim(eq + 1); }
            else if (!u_strnicmp(line, "Title", 5) && (idx = atoi(line + 5)) > 0 && idx < 200)
                { char *eq = strchr(line, '='); if (eq) pls_title[idx] = u_trim(eq + 1); }
            continue;
        }
        if (!u_strnicmp(line, "#EXTINF:", 8))
        {
            char *comma = strchr(line, ',');
            u_copy(name, sizeof name, comma ? u_trim(comma + 1) : "");
            continue;
        }
        if (line[0] == '#') continue;
        if (u_is_url(line))
        {
            fn(ctx, name[0] ? name : line, line);
            name[0] = 0;
        }
    }
    if (pls)
        for (int i = 1; i < 200; i++)
            if (pls_file[i] && u_is_url(pls_file[i]))
                fn(ctx, pls_title[i] && *pls_title[i] ? pls_title[i] : pls_file[i], pls_file[i]);
}

static void add_station(void *ctx, const char *name, const char *url) { add(ctx, E_STATION, name, url); }

static void list_name(const char *file, char *out, size_t size)
{
    u_copy(out, size, file);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static int rb_open(lib_list *l, const char *place);
static int music_open(lib_list *l, const char *place);
static int music_poll(lib_list *l);
static char rb_err[160];
static int      mu_state;            // the music folder: LIB_PENDING while it comes, then LIB_OK / LIB_FAIL
static unsigned mu_gen;

int lib_open(lib_list *l, const char *place)
{
    lib_free(l);
    u_copy(l->place, sizeof l->place, place);
    l->ok = true;
    if (place[0]) add(l, E_PARENT, "..", "");          // as on the MSX: [..] first
    if (!strncmp(place, "rb:", 3)) return rb_open(l, place);
    if (!strncmp(place, "music:", 6) || !strcmp(place, "pick:")) return music_open(l, place);
    if (!place[0])
    {
        u_copy(l->title, sizeof l->title, "/");
        add(l, E_FOLDER, "Internet radio", "radio:");
        char nm[300];
        if (js_music_name(nm, sizeof nm) > 0)
        {
            char t[340];
            snprintf(t, sizeof t, "Music (%s)", nm);
            add(l, E_FOLDER, t, "music:");
            add(l, E_FOLDER, "Choose another music folder", "pick:");
        }
        else add(l, E_FOLDER, "Choose a music folder", "pick:");
        return LIB_OK;
    }
    if (!strncmp(place, "radio:", 6))
    {
        const char *file = place + 6;
        const char *rd = config_radio_dir();
        if (!*file)
        {
            u_copy(l->title, sizeof l->title, "/Internet radio");
            sys_dirent *d;
            int n;
            if (!sys_list_dir(rd, &d, &n)) { l->ok = false; return LIB_FAIL; }
            // Favourites, the directory's folders, then the lists of the folder by name
            add(l, E_FOLDER, "Favourites", "radio:" FAV_FILE);
            add(l, E_FOLDER, "Top stations", "rb:top");
            add(l, E_FOLDER, "Most voted", "rb:votes");
            add(l, E_FOLDER, "Trending now", "rb:trend");
            add(l, E_FOLDER, "By country", "rb:countries");
            add(l, E_FOLDER, "By genre", "rb:genres");
            add(l, E_FOLDER, "By decade", "rb:decades");
            int first = l->count;
            for (int i = 0; i < n; i++)
            {
                if (d[i].is_dir || !is_list_file(d[i].name) || !u_stricmp(d[i].name, FAV_FILE)) continue;
                char nm[256], tg[300];
                list_name(d[i].name, nm, sizeof nm);
                snprintf(tg, sizeof tg, "radio:%s", d[i].name);
                add(l, E_FOLDER, nm, tg);
            }
            sys_free_dir(d, n);
            qsort(l->e + first, (size_t)(l->count - first), sizeof *l->e, cmp_entry);
            return LIB_OK;
        }
        char nm[256], path[2200];
        list_name(file, nm, sizeof nm);
        snprintf(l->title, sizeof l->title, "/Internet radio/%s", nm);
        u_join(path, sizeof path, rd, file);
        char *text = sys_read_file(path, NULL);
        if (!text) { l->ok = false; return LIB_FAIL; }
        parse_list(text, add_station, l);
        free(text);
        return LIB_OK;
    }
    // a folder (not on the web: the music folder is music:)
    u_copy(l->title, sizeof l->title, place);
    for (char *p = l->title; *p; p++) if (*p == '\\') *p = '/';
    sys_dirent *d;
    int n;
    if (!sys_list_dir(place, &d, &n)) { l->ok = false; return LIB_FAIL; }
    for (int i = 0; i < n; i++)
    {
        char full[2200];
        u_join(full, sizeof full, place, d[i].name);
        if (d[i].is_dir) add(l, E_FOLDER, d[i].name, full);
        else if (lib_is_music(d[i].name)) add(l, E_FILE, d[i].name, full);
    }
    sys_free_dir(d, n);
    if (l->count) qsort(l->e, (size_t)l->count, sizeof *l->e, cmp_entry);
    return LIB_OK;
}

void lib_free(lib_list *l)
{
    free(l->e);
    l->e = NULL;
    l->count = 0;
}

void lib_parent(const char *place, char *out, size_t size)
{
    if (!place[0]) { out[0] = 0; return; }
    if (!strncmp(place, "rb:", 3))
    {
        const char *p = place + 3;
        u_copy(out, size, !strncmp(p, "country=", 8) ? "rb:countries" : !strncmp(p, "genre=", 6) ? "rb:genres"
                        : !strncmp(p, "decade=", 7) ? "rb:decades" : "radio:");
        return;
    }
    if (!strncmp(place, "radio:", 6)) { u_copy(out, size, place[6] ? "radio:" : ""); return; }
    if (!strncmp(place, "music:", 6))
    {
        const char *sl = strrchr(place + 6, '/');
        if (!place[6]) out[0] = 0;
        else if (!sl) u_copy(out, size, "music:");
        else snprintf(out, size, "%.*s", (int)(sl - place), place);
        return;
    }
    char buf[2048];
    u_copy(buf, sizeof buf, place);
    size_t l = strlen(buf);
    while (l > 1 && (buf[l - 1] == '\\' || buf[l - 1] == '/') && buf[l - 2] != ':') buf[--l] = 0;
    char *sep = NULL;
    for (char *p = buf; *p; p++) if (*p == '\\' || *p == '/') sep = p;
    // the root of a drive (C:\) or of the file system: up to the top
    if (!sep || (sep[1] == 0)) { out[0] = 0; return; }
    if (sep == buf) { u_copy(out, size, "/"); return; }
    if (sep > buf && sep[-1] == ':') sep[1] = 0;      // C:\Music -> C:\ (keep the separator)
    else *sep = 0;
    u_copy(out, size, buf);
}

bool lib_is_favourites(const char *place)
{
    return !u_stricmp(place, "radio:" FAV_FILE);
}

// ---------------- favourites ----------------
typedef struct { char (*name)[256]; char (*url)[2048]; int n; } fav_t;

static void fav_take(void *ctx, const char *name, const char *url)
{
    fav_t *f = ctx;
    if (f->n % 32 == 0)
    {
        f->name = realloc(f->name, sizeof *f->name * (size_t)(f->n + 32));
        f->url = realloc(f->url, sizeof *f->url * (size_t)(f->n + 32));
    }
    u_copy(f->name[f->n], sizeof f->name[f->n], name);
    u_copy(f->url[f->n], sizeof f->url[f->n], url);
    f->n++;
}

static const char *fav_path(void)
{
    static char p[2200];
    u_join(p, sizeof p, config_radio_dir(), FAV_FILE);
    return p;
}

static void fav_load(fav_t *f)
{
    memset(f, 0, sizeof *f);
    char *text = sys_read_file(fav_path(), NULL);
    if (text) { parse_list(text, fav_take, f); free(text); }
}

static bool fav_save(const fav_t *f)
{
    size_t cap = 64 + (size_t)f->n * 2400, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n, "#EXTM3U\r\n");
    for (int i = 0; i < f->n; i++)
        n += (size_t)snprintf(b + n, cap - n, "#EXTINF:-1,%s\r\n%s\r\n", f->name[i], f->url[i]);
    bool ok = sys_write_file(fav_path(), b, n);
    free(b);
    return ok;
}

static void fav_free(fav_t *f) { free(f->name); free(f->url); }

int lib_fav_add(const char *name, const char *url)
{
    fav_t f;
    fav_load(&f);
    for (int i = 0; i < f.n; i++) if (!strcmp(f.url[i], url)) { fav_free(&f); return 0; }
    fav_take(&f, name && *name ? name : url, url);
    int r = fav_save(&f) ? 1 : -1;
    fav_free(&f);
    return r;
}

int lib_fav_del(const char *url)
{
    fav_t f;
    fav_load(&f);
    int r = 0;
    for (int i = 0; i < f.n; i++)
        if (!strcmp(f.url[i], url))
        {
            memmove(f.name + i, f.name + i + 1, sizeof *f.name * (size_t)(f.n - i - 1));
            memmove(f.url + i, f.url + i + 1, sizeof *f.url * (size_t)(f.n - i - 1));
            f.n--;
            r = fav_save(&f) ? 1 : -1;
            break;
        }
    fav_free(&f);
    return r;
}

bool lib_fav_move(const char *url, int dir)
{
    fav_t f;
    fav_load(&f);
    bool ok = false;
    for (int i = 0; i < f.n; i++)
        if (!strcmp(f.url[i], url))
        {
            int j = i + dir;
            if (j < 0 || j >= f.n) break;
            char n[256], u[2048];
            memcpy(n, f.name[i], sizeof n); memcpy(u, f.url[i], sizeof u);
            memcpy(f.name[i], f.name[j], sizeof n); memcpy(f.url[i], f.url[j], sizeof u);
            memcpy(f.name[j], n, sizeof n); memcpy(f.url[j], u, sizeof u);
            ok = fav_save(&f);
            break;
        }
    fav_free(&f);
    return ok;
}

static void count_one(void *ctx, const char *name, const char *url) { (void)name; (void)url; (*(int *)ctx)++; }

bool lib_import_list(const char *path, char *msg, size_t msglen)
{
    size_t len;
    char *text = sys_read_file(path, &len);
    if (!text) { snprintf(msg, msglen, "Cannot read %s", u_basename(path)); return false; }
    char *copy = malloc(len + 1);
    memcpy(copy, text, len + 1);
    int n = 0;
    parse_list(copy, count_one, &n);
    free(copy);
    if (!n) { free(text); snprintf(msg, msglen, "%s has no stations", u_basename(path)); return false; }
    char dst[2200];
    u_join(dst, sizeof dst, config_radio_dir(), u_basename(path));
    if (!u_stricmp(u_basename(path), FAV_FILE)) u_join(dst, sizeof dst, config_radio_dir(), "Favourites (imported).m3u");
    bool ok = sys_write_file(dst, text, len);
    free(text);
    char nm[256];
    list_name(u_basename(dst), nm, sizeof nm);
    if (ok) snprintf(msg, msglen, "%d stations: Internet radio/%s", n, nm);
    else snprintf(msg, msglen, "Cannot write to the Radio folder");
    return ok;
}

// ---------------- the Radio Browser directory ----------------
static const char *const genres[] = {
    "pop", "rock", "classic rock", "hard rock", "metal", "punk", "alternative", "indie", "jazz", "smooth jazz",
    "blues", "soul", "funk", "disco", "rnb", "hip hop", "rap", "reggae", "latin", "salsa", "country", "folk",
    "classical", "opera", "ambient", "chillout", "lounge", "dance", "electronic", "house", "techno", "trance",
    "drum and bass", "dubstep", "eurodance", "schlager", "chanson", "world", "christmas", "kids", "news", "talk",
    "sports", "comedy", "soundtrack", "instrumental", "piano", "new age", "gospel", "christian",
};
static const char *const decades[][2] = {
    { "1950s", "50s" }, { "1960s", "60s" }, { "1970s", "70s" }, { "1980s", "80s" }, { "1990s", "90s" },
    { "2000s", "00s" }, { "2010s", "10s" }, { "Oldies", "oldies" },
};
#define PAGE 60

static char     rb_place[2048];      // the list that comes in
static int      rb_offset;           // its next page
static bool     rb_pending;
static rd_item *countries;           // kept: the countries come once
static int      n_countries;

const char *lib_error(void) { return rb_err; }

// the query of a station list, and its title
static bool rb_query(const char *place, char *q, size_t qn, char *title, size_t tn)
{
    const char *p = place + 3;
    char enc[600], name[300];
    if (!strcmp(p, "top"))   { snprintf(q, qn, "order=clickcount&reverse=true"); snprintf(title, tn, "/Internet radio/Top stations"); return true; }
    if (!strcmp(p, "votes")) { snprintf(q, qn, "order=votes&reverse=true"); snprintf(title, tn, "/Internet radio/Most voted"); return true; }
    if (!strcmp(p, "trend")) { snprintf(q, qn, "order=clicktrend&reverse=true"); snprintf(title, tn, "/Internet radio/Trending now"); return true; }
    if (!strncmp(p, "country=", 8))
    {
        const char *bar = strchr(p + 8, '|');
        char code[16];
        snprintf(code, sizeof code, "%.*s", bar ? (int)(bar - p - 8) : 2, p + 8);
        rd_urlenc(enc, sizeof enc, code);
        snprintf(q, qn, "countrycode=%s&order=clickcount&reverse=true", enc);
        snprintf(title, tn, "/Internet radio/By country/%s", bar ? bar + 1 : code);
        return true;
    }
    if (!strncmp(p, "genre=", 6) || !strncmp(p, "decade=", 7))
    {
        bool genre = p[0] == 'g';
        const char *t = p + (genre ? 6 : 7);
        const char *bar = strchr(t, '|');
        snprintf(name, sizeof name, "%.*s", bar ? (int)(bar - t) : (int)strlen(t), t);
        rd_urlenc(enc, sizeof enc, name);
        snprintf(q, qn, "tag=%s&order=clickcount&reverse=true", enc);
        if (genre && name[0] >= 'a' && name[0] <= 'z') name[0] = (char)(name[0] - 32);
        snprintf(title, tn, "/Internet radio/%s/%s", genre ? "By genre" : "By decade", bar ? bar + 1 : name);
        return true;
    }
    if (!strncmp(p, "search=", 7))
    {
        rd_urlenc(enc, sizeof enc, p + 7);
        snprintf(q, qn, "name=%s&order=clickcount&reverse=true", enc);
        snprintf(title, tn, "/Internet radio/Search/%s", p + 7);
        return true;
    }
    return false;
}

static void rb_build(lib_list *l, const char *place)
{
    lib_free(l);
    u_copy(l->place, sizeof l->place, place);
    l->ok = true;
    add(l, E_PARENT, "..", "");
    if (!strcmp(place, "rb:countries"))
    {
        u_copy(l->title, sizeof l->title, "/Internet radio/By country");
        for (int i = 0; i < n_countries; i++)
        {
            char tg[300];
            snprintf(tg, sizeof tg, "rb:country=%s|%s", countries[i].code, countries[i].name);
            add(l, E_FOLDER, countries[i].name, tg);
        }
        return;
    }
    char q[800];
    rb_query(place, q, sizeof q, l->title, sizeof l->title);
    int n, raw;
    const rd_item *it = rd_items(&n, &raw);
    for (int i = 0; i < n; i++)
    {
        add(l, E_STATION, it[i].name, it[i].url);
        u_copy(l->e[l->count - 1].uuid, sizeof l->e[0].uuid, it[i].uuid);
    }
    // a full page may have more after it
    if (raw >= PAGE) add(l, E_MORE, "More stations...", place);
}

static int rb_open(lib_list *l, const char *place)
{
    const char *p = place + 3;
    if (!strcmp(p, "genres") || !strcmp(p, "decades"))
    {
        bool g = p[0] == 'g';
        u_copy(l->title, sizeof l->title, g ? "/Internet radio/By genre" : "/Internet radio/By decade");
        if (g)
            for (unsigned i = 0; i < sizeof genres / sizeof genres[0]; i++)
            {
                char nm[40], tg[60];
                snprintf(nm, sizeof nm, "%s", genres[i]);
                nm[0] = (char)(nm[0] - 32);
                snprintf(tg, sizeof tg, "rb:genre=%s", genres[i]);
                add(l, E_FOLDER, nm, tg);
            }
        else
            for (unsigned i = 0; i < sizeof decades / sizeof decades[0]; i++)
            {
                char tg[60];
                snprintf(tg, sizeof tg, "rb:decade=%s|%s", decades[i][1], decades[i][0]);
                add(l, E_FOLDER, decades[i][0], tg);
            }
        return LIB_OK;
    }
    if (!strcmp(p, "countries") && countries) { rb_build(l, place); return LIB_OK; }
    u_copy(rb_place, sizeof rb_place, place);
    rb_pending = true;
    rb_offset = 0;
    rb_err[0] = 0;
    if (!strcmp(p, "countries")) { rd_fetch("/json/countries?hidebroken=true&order=name", true, false); return LIB_PENDING; }
    char q[800], title[512], path[1100];
    if (!rb_query(place, q, sizeof q, title, sizeof title)) { rb_pending = false; u_copy(rb_err, sizeof rb_err, "Unknown list"); return LIB_FAIL; }
    snprintf(path, sizeof path, "/json/stations/search?%s%s&hidebroken=true&limit=%d", q, rd_codec_query(), PAGE);
    rd_fetch(path, false, false);
    return LIB_PENDING;
}

int lib_more(lib_list *l)
{
    char q[800], title[512], path[1100];
    if (!rb_query(l->place, q, sizeof q, title, sizeof title)) return LIB_FAIL;
    u_copy(rb_place, sizeof rb_place, l->place);
    rb_pending = true;
    rb_offset += PAGE;
    snprintf(path, sizeof path, "/json/stations/search?%s%s&hidebroken=true&limit=%d&offset=%d", q, rd_codec_query(), PAGE, rb_offset);
    rd_fetch(path, false, true);
    return LIB_PENDING;
}

static int cmp_country(const void *a, const void *b)
{
    const char *x = ((const rd_item *)a)->name, *y = ((const rd_item *)b)->name;
    if (!u_strnicmp(x, "The ", 4)) x += 4;              // "The Netherlands" with the N
    if (!u_strnicmp(y, "The ", 4)) y += 4;
    return u_stricmp(x, y);
}

int lib_poll(lib_list *l)
{
    if (mu_state) return music_poll(l);
    if (!rb_pending) return LIB_FAIL;
    int s = rd_state();
    if (s == RD_LOADING) return LIB_PENDING;
    rb_pending = false;
    if (s != RD_DONE) { u_copy(rb_err, sizeof rb_err, s == RD_FAILED ? rd_error() : "Stopped"); return LIB_FAIL; }
    if (!strcmp(rb_place, "rb:countries"))
    {
        int n;
        const rd_item *it = rd_items(&n, NULL);
        free(countries);
        countries = malloc(sizeof *countries * (size_t)(n ? n : 1));
        memcpy(countries, it, sizeof *countries * (size_t)n);
        n_countries = n;
        qsort(countries, (size_t)n, sizeof *countries, cmp_country);
    }
    rb_build(l, rb_place);
    return LIB_OK;
}

void lib_cancel(void)
{
    mu_state = 0;
    mu_gen++;
    if (rb_pending) rd_cancel();
    rb_pending = false;
}

// ---------------- the music folder (the browser's: web/picoamp.js) ----------------
// places: "music:" its top, "music:a/b" a folder in it; "pick:" asks for a folder
static char     mu_place[2048];
static char    *mu_lines;

static void mu_done(void *user, const char *lines, int ok)
{
    if ((unsigned)(uintptr_t)user != mu_gen) return;
    free(mu_lines);
    mu_lines = strdup(lines);
    mu_state = ok ? LIB_OK : LIB_FAIL;
    if (!ok) u_copy(rb_err, sizeof rb_err, lines[0] ? lines : "The music folder cannot be read");
}

static int music_open(lib_list *l, const char *place)
{
    rb_err[0] = 0;
    u_copy(mu_place, sizeof mu_place, place);
    mu_state = LIB_PENDING;
    mu_gen++;
    if (!strcmp(place, "pick:")) { u_copy(mu_place, sizeof mu_place, "music:"); js_music_pick(); }
    music_list(mu_place + 6, mu_done, (void *)(uintptr_t)mu_gen);
    (void)l;
    return mu_state == LIB_PENDING ? LIB_PENDING : music_poll(l);
}

static int music_poll(lib_list *l)
{
    if (mu_state == LIB_PENDING) return LIB_PENDING;
    if (mu_state != LIB_OK) { mu_state = 0; return LIB_FAIL; }
    mu_state = 0;
    lib_free(l);
    u_copy(l->place, sizeof l->place, mu_place);
    l->ok = true;
    add(l, E_PARENT, "..", "");
    char nm[300];
    if (js_music_name(nm, sizeof nm) <= 0) u_copy(nm, sizeof nm, "Music");
    snprintf(l->title, sizeof l->title, "/%s%s%s", nm, mu_place[6] ? "/" : "", mu_place + 6);
    char *cur = mu_lines, *line;
    while (cur && (line = u_token(&cur, "\n")))
    {
        if (!line[0] || !line[1]) continue;
        char full[2200];
        snprintf(full, sizeof full, "%s%s%s", mu_place, mu_place[6] ? "/" : "", line + 2);
        if (line[0] == 'd') add(l, E_FOLDER, line + 2, full);
        else if (lib_is_music(line + 2)) add(l, E_FILE, line + 2, full);
    }
    if (l->count > 1) qsort(l->e + 1, (size_t)(l->count - 1), sizeof *l->e, cmp_entry);
    return LIB_OK;
}
