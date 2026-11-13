// wxsvc.c - the place, its zone and its weather (see wxsvc.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wxsvc.h"
#include "config.h"
#include "json.h"
#include "web.h"
#include "radiodir.h"
#include "sys.h"
#include "util.h"

#define REFRESH_MS   (30u * 60u * 1000u)
#define IP_EVERY_MS  (6u * 60u * 60u * 1000u)
#define RETRY_MS     60000u
#define MAX_RESULTS  8

static bool started, busy;
static wx_info    info;
static bool want_refresh, want_ip;
static char       search_q[100];
static bool want_search;
static int        s_state = WXS_IDLE;
static struct { char name[160]; double lat, lon; } res[MAX_RESULTS];
static int        nres;
static double     lat, lon;
static bool       have_ll;

static const char *base(const char *env, const char *def)
{
    const char *e = getenv(env);
    return e ? e : def;
}
#define METEO_DEF "https://api.open-meteo.com"
#define GEO_DEF   "https://geocoding-api.open-meteo.com"
#define IP_DEF    "https://get.geojs.io"

static void changed(void) { info.gen++; }

// a whole JSON reply through the reader
static bool parse_json(int st, const char *data, size_t len, const char *ne, json_cb cb, void *user, char *err, size_t errlen)
{
    err[0] = 0;
    if (st < 200 || st >= 300)
    {
        if (ne && ne[0]) u_copy(err, errlen, ne);
        else snprintf(err, errlen, "HTTP %d", st);
        return false;
    }
    json_t *j = calloc(1, sizeof *j);
    j->cb = cb;
    j->user = user;
    json_reset(j);
    json_feed(j, data, len);
    bool ok = j->done;
    free(j);
    if (!ok) snprintf(err, errlen, "The reply broke off");
    return ok;
}

static int tenths(const char *v)
{
    double d = atof(v);
    return (int)(d * 10.0 + (d < 0 ? -0.5 : 0.5));
}

// ---------------- GeoJS: the internet connection's place ----------------
typedef struct { char city[64], region[64], cc[8]; double lat, lon; bool have_lat, have_lon; } ip_t;

static void on_ip(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)idx;
    ip_t *p = j->user;
    if (ev != JSON_VALUE) return;
    if (!strcmp(path, "city")) u_copy(p->city, sizeof p->city, v);
    else if (!strcmp(path, "region")) u_copy(p->region, sizeof p->region, v);
    else if (!strcmp(path, "country_code")) u_copy(p->cc, sizeof p->cc, v);
    else if (!strcmp(path, "latitude") && v[0] && strcmp(v, "nil")) { p->lat = atof(v); p->have_lat = true; }
    else if (!strcmp(path, "longitude") && v[0] && strcmp(v, "nil")) { p->lon = atof(v); p->have_lon = true; }
}

static void place_name(char *out, size_t n, const char *city, const char *region, const char *cc)
{
    snprintf(out, n, "%s", city);
    if (region[0] && strcmp(region, city)) { size_t l = strlen(out); snprintf(out + l, n - l, "%s%s", out[0] ? ", " : "", region); }
    if (cc[0]) { size_t l = strlen(out); snprintf(out + l, n - l, "%s%s", out[0] ? ", " : "", cc); }
}

static void after(bool ok);

static void ip_done(void *user, int st, const char *data, size_t len, const char *ne)
{
    (void)user;
    ip_t p;
    memset(&p, 0, sizeof p);
    char err[120];
    bool ok = parse_json(st, data, len, ne, on_ip, &p, err, sizeof err) && p.have_lat && p.have_lon;
    if (!ok)
        snprintf(info.error, sizeof info.error, "No place from GeoJS%s%s", err[0] ? ": " : "", err);
    else
    {
        place_name(info.place, sizeof info.place, p.city, p.region, p.cc);
        if (!info.place[0]) u_copy(info.place, sizeof info.place, "Your internet connection");
        info.have_place = true;
        lat = p.lat;
        lon = p.lon;
        have_ll = true;
        info.error[0] = 0;
        want_refresh = true;
    }
    changed();
    after(ok);
}

static void ask_ip(void)
{
    char url[300];
    snprintf(url, sizeof url, "%s/v1/ip/geo.json", base("PICOAMP_IPAPI", IP_DEF));
    net_fetch(url, ip_done, NULL);
}

// ---------------- Open-Meteo's forecast ----------------
typedef struct { wx_info w; bool have_off, have_cur; } met_t;

static void on_meteo(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    met_t *m = j->user;
    if (ev != JSON_VALUE) return;
    wx_info *w = &m->w;
    if (!strcmp(path, "utc_offset_seconds")) { w->utc_offset = atoi(v); m->have_off = true; }
    else if (!strncmp(path, "current.", 8))
    {
        const char *f = path + 8;
        if (!strcmp(f, "temperature_2m")) { w->t10 = tenths(v); m->have_cur = true; }
        else if (!strcmp(f, "apparent_temperature")) w->feel10 = tenths(v);
        else if (!strcmp(f, "relative_humidity_2m")) w->hum = atoi(v);
        else if (!strcmp(f, "wind_speed_10m")) w->wind = (tenths(v) + 5) / 10;
        else if (!strcmp(f, "weather_code")) w->code = atoi(v);
        else if (!strcmp(f, "is_day")) w->is_day = atoi(v) != 0;
    }
    else if (!strncmp(path, "daily.", 6) && idx < WX_DAYS)
    {
        const char *f = path + 6;
        wx_day *d = &w->days[idx];
        if (!strcmp(f, "time"))
        {
            sscanf(v, "%d-%d-%d", &d->year, &d->month, &d->day);
            struct tm tm = { 0 };
            tm.tm_year = d->year - 1900;
            tm.tm_mon = d->month - 1;
            tm.tm_mday = d->day;
            tm.tm_hour = 12;
            mktime(&tm);
            d->wday = (tm.tm_wday + 6) % 7;
            if (idx + 1 > w->ndays) w->ndays = idx + 1;
        }
        else if (!strcmp(f, "weather_code")) { d->code = atoi(v); d->ok = true; }
        else if (!strcmp(f, "temperature_2m_max")) d->tmax10 = tenths(v);
        else if (!strcmp(f, "temperature_2m_min")) d->tmin10 = tenths(v);
        else if (!strcmp(f, "precipitation_probability_max")) d->pp = v[0] == 'n' ? -1 : atoi(v);
    }
}

static void meteo_done(void *user, int st, const char *data, size_t len, const char *ne)
{
    met_t *m = user;
    char err[120];
    bool ok = parse_json(st, data, len, ne, on_meteo, m, err, sizeof err) && m->have_off;
    if (ok)
    {
        info.utc_offset = m->w.utc_offset;
        info.have_zone = true;
        if (m->have_cur)
        {
            info.t10 = m->w.t10; info.feel10 = m->w.feel10; info.hum = m->w.hum; info.wind = m->w.wind;
            info.code = m->w.code; info.is_day = m->w.is_day;
            memcpy(info.days, m->w.days, sizeof info.days);
            info.ndays = m->w.ndays;
            info.have_wx = true;
        }
        info.error[0] = 0;
    }
    else snprintf(info.error, sizeof info.error, "No weather from Open-Meteo%s%s", err[0] ? ": " : "", err);
    changed();
    free(m);
    after(ok);
}

static void ask_meteo(void)
{
    met_t *m = calloc(1, sizeof *m);
    for (int i = 0; i < WX_DAYS; i++) m->w.days[i].pp = -1;
    char url[600];
    snprintf(url, sizeof url, "%s/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&timezone=auto&forecast_days=%d&current=temperature_2m,weather_code,apparent_temperature,"
             "relative_humidity_2m,wind_speed_10m,is_day&daily=weather_code,temperature_2m_max,"
             "temperature_2m_min,precipitation_probability_max", base("PICOAMP_METEO", METEO_DEF), lat, lon, WX_DAYS);
    net_fetch(url, meteo_done, m);
}

// ---------------- the place search ----------------
typedef struct { char name[64], admin[64], cc[8]; double lat, lon; bool ll; } geo_cur;

static void on_geo(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)idx;
    geo_cur *g = j->user;
    if (ev == JSON_END && !strcmp(path, "results"))
    {
        if (g->name[0] && g->ll && nres < MAX_RESULTS)
        {
            place_name(res[nres].name, sizeof res[nres].name, g->name, g->admin, g->cc);
            res[nres].lat = g->lat;
            res[nres].lon = g->lon;
            nres++;
        }
        memset(g, 0, sizeof *g);
        return;
    }
    if (ev != JSON_VALUE || strncmp(path, "results.", 8)) return;
    const char *f = path + 8;
    if (!strcmp(f, "name")) u_copy(g->name, sizeof g->name, v);
    else if (!strcmp(f, "admin1")) u_copy(g->admin, sizeof g->admin, v);
    else if (!strcmp(f, "country_code")) u_copy(g->cc, sizeof g->cc, v);
    else if (!strcmp(f, "latitude")) { g->lat = atof(v); g->ll = true; }
    else if (!strcmp(f, "longitude")) g->lon = atof(v);
}

static void search_done(void *user, int st, const char *data, size_t len, const char *ne)
{
    geo_cur *g = user;
    char err[120];
    nres = 0;
    bool ok = parse_json(st, data, len, ne, on_geo, g, err, sizeof err);
    free(g);
    s_state = ok ? WXS_DONE : WXS_FAILED;
}

static void do_search(void)
{
    char enc[300], url[500];
    rd_urlenc(enc, sizeof enc, search_q);
    snprintf(url, sizeof url, "%s/v1/search?name=%s&count=%d&language=en&format=json", base("PICOAMP_GEO", GEO_DEF), enc, MAX_RESULTS);
    net_fetch(url, search_done, calloc(1, sizeof(geo_cur)));
}

// ---------------- what to ask next (wx_tick, once a frame) ----------------
static uint32_t next, ip_at, retry = RETRY_MS;
static bool ip_ok, asking_ip;

static void after(bool ok)
{
    uint32_t now = sys_millis();
    busy = false;
    if (asking_ip) { asking_ip = false; if (ok) { ip_ok = true; retry = RETRY_MS; next = now; return; } }
    else if (ok) { next = now + REFRESH_MS; retry = RETRY_MS; return; }
    next = now + retry;
    if (retry < 10 * 60000u) retry *= 2;
}

void wx_tick(void)
{
    if (!started || busy) return;
    uint32_t now = sys_millis();
    if (want_search) { want_search = false; do_search(); return; }
    if (cfg.loc_ip && (want_ip || ((!ip_ok || now - ip_at >= IP_EVERY_MS) && (int32_t)(now - next) >= 0)))
    {
        want_ip = false;
        ip_at = now;
        busy = asking_ip = true;
        ask_ip();
        return;
    }
    if (have_ll && (want_refresh || (int32_t)(now - next) >= 0))
    {
        want_refresh = false;
        busy = true;
        ask_meteo();
    }
}

void wx_start(void)
{
    if (started) return;
    started = true;
    if (!cfg.loc_ip && cfg.loc_name[0])
    {
        u_copy(info.place, sizeof info.place, cfg.loc_name);
        info.have_place = true;
        lat = cfg.loc_lat;
        lon = cfg.loc_lon;
        have_ll = true;
    }
    want_refresh = true;
}

void wx_get(wx_info *out)
{
    *out = info;
}

void wx_refresh(void) { want_refresh = true; }

bool wx_now(wx_time *t, bool place_time)
{
    int64_t now = sys_unix_time();
    struct tm tm;
    bool placed = place_time && info.have_zone;
    if (placed)
    {
        time_t l = (time_t)(now + info.utc_offset);
        gmtime_r(&l, &tm);
    }
    else sys_localtime(now, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->wday = (tm.tm_wday + 6) % 7;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
    return placed;
}

void wx_search(const char *name)
{
    u_copy(search_q, sizeof search_q, name);
    s_state = WXS_BUSY;
    want_search = true;
}

int wx_search_state(void)
{
    return s_state;
}

int wx_search_results(char names[][160], int max)
{
    int n = nres < max ? nres : max;
    for (int i = 0; i < n; i++) u_copy(names[i], 160, res[i].name);
    return n;
}

void wx_choose(int i)
{
    if (i < 0 || i >= nres) return;
    cfg.loc_ip = false;
    u_copy(cfg.loc_name, sizeof cfg.loc_name, res[i].name);
    cfg.loc_lat = res[i].lat;
    cfg.loc_lon = res[i].lon;
    u_copy(info.place, sizeof info.place, res[i].name);
    info.have_place = true;
    info.have_zone = info.have_wx = false;
    lat = res[i].lat;
    lon = res[i].lon;
    have_ll = true;
    changed();
    want_refresh = true;
}

void wx_use_ip(void)
{
    cfg.loc_ip = true;
    cfg.loc_name[0] = 0;
    ip_ok = false;
    want_ip = true;
}

const char *wx_words(int code)
{
    static const struct { int max; const char *w; } t[] = {
        { 0, "Clear" }, { 1, "Mainly clear" }, { 2, "Partly cloudy" }, { 3, "Overcast" }, { 48, "Fog" },
        { 57, "Drizzle" }, { 67, "Rain" }, { 77, "Snow" }, { 82, "Showers" }, { 86, "Snow showers" }, { 255, "Thunderstorm" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (code <= t[i].max) return t[i].w;
    return "";
}
