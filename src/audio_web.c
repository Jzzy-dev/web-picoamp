// audio_web.c - the player in the browser (audio.h): the play list and the
// status here; the sound itself is the page's (an audio element through Web
// Audio: the equalizer, balance and volume, and the analysers that feed the
// visualiser). The page tells what happens through the web_audio_* calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audio.h"
#include "tags.h"
#include "util.h"
#include "sys.h"
#include "web.h"
#include "viz.h"
#include "cover.h"

static audio_status st;
static char **pl;                   // files, or "url\x1Fname" for stations
static int   pl_n, pl_pos;
static bool  pl_stations;
static int   volume = 80, balance = 50, fade = 1000;
static bool  muted;

static void changed(void) { st.gen++; }

static void apply_gain(void)
{
    double g = (muted || volume <= 0) ? 0 : pow(10.0, -(100 - volume) / 40.0) * fade / 1000.0;
    double l = balance <= 50 ? g : g * (100 - balance) / 50.0;
    double r = balance >= 50 ? g : g * balance / 50.0;
    js_audio_gain(l, r);
}

bool audio_init(bool null_device) { (void)null_device; viz_init(); st.volume = volume; apply_gain(); return true; }
void audio_shutdown(void) { js_audio_stop(); }

static void free_playlist(void)
{
    for (int i = 0; i < pl_n; i++) free(pl[i]);
    free(pl);
    pl = NULL;
    pl_n = pl_pos = 0;
}

static void set_error(const char *e)
{
    u_copy(st.error, sizeof st.error, e);
    st.err_gen++;
    changed();
}

static void media_meta(void)
{
    js_audio_meta(st.title, (int)strlen(st.title), st.artist, (int)strlen(st.artist), st.album, (int)strlen(st.album));
}

static void start_url(const char *url, const char *name)
{
    st.stream = true;
    st.seekable = false;
    st.state = ST_PLAYING;
    st.buffering = true;
    st.elapsed = st.total = st.bitrate = st.rate = 0;
    st.channels = 2;
    st.tls = !u_strnicmp(url, "https:", 6);
    st.title[0] = st.artist[0] = 0;
    cover_none();
    u_copy(st.album, sizeof st.album, name && *name ? name : url);
    u_copy(st.url, sizeof st.url, url);
    changed();
    media_meta();
    js_audio_play(url, (int)strlen(url), 0);
}

static void start_track(int i)
{
    pl_pos = i;
    st.pl_pos = i;
    st.pl_count = pl_n;
    if (pl_stations)
    {
        char *sep = strchr(pl[i], '\x1F');
        char url[2048];
        snprintf(url, sizeof url, "%.*s", sep ? (int)(sep - pl[i]) : (int)strlen(pl[i]), pl[i]);
        start_url(url, sep ? sep + 1 : "");
        return;
    }
    const char *f = pl[i];
    st.stream = false;
    st.seekable = true;
    st.tls = false;
    st.state = ST_PLAYING;
    st.buffering = true;
    st.elapsed = st.total = st.bitrate = st.rate = 0;
    st.channels = 0;
    // the name until the tags come (web_tags)
    u_copy(st.title, sizeof st.title, u_basename(f));
    char *dot = strrchr(st.title, '.');
    if (dot && dot != st.title) *dot = 0;
    st.artist[0] = st.album[0] = 0;
    cover_none();                       // (the page gives the tag's picture or the folder's)
    u_copy(st.url, sizeof st.url, f);
    changed();
    media_meta();
    js_audio_play(f, (int)strlen(f), 1);
}

void audio_play_files(const char *const *files, int count, int start)
{
    if (count <= 0) return;
    free_playlist();
    pl = malloc(sizeof *pl * (size_t)count);
    for (int i = 0; i < count; i++) pl[i] = strdup(files[i]);
    pl_n = count;
    pl_stations = false;
    start_track(start >= 0 && start < count ? start : 0);
}

void audio_play_url(const char *url, const char *name)
{
    free_playlist();
    st.pl_pos = st.pl_count = 0;
    start_url(url, name);
}

void audio_play_stations(const char *const *urls, const char *const *names, int count, int start)
{
    if (count <= 0) return;
    free_playlist();
    pl = malloc(sizeof *pl * (size_t)count);
    for (int i = 0; i < count; i++)
    {
        size_t n = strlen(urls[i]) + strlen(names[i]) + 2;
        pl[i] = malloc(n);
        snprintf(pl[i], n, "%s\x1F%s", urls[i], names[i]);
    }
    pl_n = count;
    pl_stations = true;
    start_track(start >= 0 && start < count ? start : 0);
}

void audio_toggle_pause(void)
{
    if (st.state == ST_STOPPED)
    {
        if (pl_n) start_track(pl_pos);
        else if (st.stream && st.url[0]) { char u[2048], n[256]; u_copy(u, sizeof u, st.url); u_copy(n, sizeof n, st.album); start_url(u, n); }
        return;
    }
    if (st.stream)
    {
        // a station does not pause: it stops (and starts again live)
        js_audio_stop();
        st.state = ST_STOPPED;
        st.buffering = false;
        changed();
        return;
    }
    st.state = st.state == ST_PAUSED ? ST_PLAYING : ST_PAUSED;
    js_audio_pause(st.state == ST_PAUSED);
    changed();
}

void audio_stop(void)
{
    js_audio_stop();
    st.state = ST_STOPPED;
    st.buffering = false;
    st.elapsed = 0;
    changed();
}

static int pick(int dir)
{
    if (st.mode & MODE_SHUFFLE && pl_n > 1)
    {
        int n;
        do n = rand() % pl_n; while (n == pl_pos);
        return n;
    }
    return (pl_pos + dir + pl_n) % pl_n;
}

void audio_next(void) { if (pl_n) start_track(pick(1)); }

void audio_prev(void)
{
    if (!pl_n) return;
    if (!pl_stations && st.elapsed >= 3) { js_audio_seek(0); return; }   // back to the start first
    start_track(pick(-1));
}

void audio_seek_to(int s)
{
    if (st.stream || st.state == ST_STOPPED) return;
    if (s < 0) s = 0;
    if (st.total && s >= st.total) s = st.total - 1;
    js_audio_seek(s);
    st.elapsed = s;
    changed();
}

void audio_seek_rel(int s) { audio_seek_to(st.elapsed + s); }

void audio_set_volume(int v) { volume = v < 0 ? 0 : v > 100 ? 100 : v; st.volume = volume; apply_gain(); changed(); }
void audio_set_mute(bool m) { muted = m; st.muted = m; apply_gain(); changed(); }
void audio_set_balance(int b) { balance = b < 0 ? 0 : b > 100 ? 100 : b; apply_gain(); }
void audio_set_fade(int p) { p = p < 0 ? 0 : p > 1000 ? 1000 : p; if (p != fade) { fade = p; apply_gain(); } }
void audio_set_mode(int m) { st.mode = m; changed(); }
void audio_set_eq(const int8_t g[EQ_BANDS]) { js_audio_eq(g[0], g[1], g[2], g[3], g[4]); }

void audio_status_get(audio_status *s) { *s = st; }

// ---------------- what the page tells ----------------
EXPORT(web_audio_playing)
void web_audio_playing(int rate, int channels)
{
    st.buffering = false;
    // (a station's own rate is not known here: the browser resamples it)
    if (!st.stream && !st.rate) { if (rate) st.rate = rate; if (channels) st.channels = channels; }
    if (st.state == ST_STOPPED) st.state = ST_PLAYING;
    changed();
}

EXPORT(web_audio_waiting) void web_audio_waiting(void) { st.buffering = true; changed(); }

EXPORT(web_audio_time)
void web_audio_time(double elapsed, double total)
{
    int e = (int)elapsed, t = total > 0 && isfinite(total) ? (int)(total + 0.5) : 0;
    if (e != st.elapsed || t != st.total)
    {
        st.elapsed = e;
        st.total = t;
        if (!st.stream && t > 0 && st.bitrate == 0 && st.total) {}
        changed();
    }
}

EXPORT(web_audio_bitrate) void web_audio_bitrate(int kbps) { st.bitrate = kbps; changed(); }

EXPORT(web_audio_ended)
void web_audio_ended(void)
{
    // the end of a file: the next (repeat goes round; shuffle picks one)
    if (pl_n && !pl_stations)
    {
        if (st.mode & MODE_SHUFFLE) { start_track(pick(1)); return; }
        if (pl_pos + 1 < pl_n) { start_track(pl_pos + 1); return; }
        if (st.mode & MODE_REPEAT) { start_track(0); return; }
    }
    st.state = ST_STOPPED;
    st.elapsed = 0;
    st.buffering = false;
    changed();
}

EXPORT(web_audio_error)
void web_audio_error(const char *msg)
{
    js_audio_stop();
    st.state = ST_STOPPED;
    st.buffering = false;
    set_error(msg);
}

// a stream's title (when the page knows one)
EXPORT(web_audio_title)
void web_audio_title(const char *title)
{
    u_copy(st.title, sizeof st.title, title);
    changed();
    media_meta();
}

// a file's first bytes (and its last 128): its ID3 tags
static const unsigned char *tag_buf;
static size_t tag_len;

void *sys_fopen_tags(void)
{
    return tag_buf ? fmemopen((void *)tag_buf, tag_len, "rb") : NULL;
}

// the format from the first bytes: an MPEG audio frame (after ID3), or a WAV header
static void read_format(const unsigned char *b, size_t n)
{
    size_t p = 0;
    if (n >= 10 && !memcmp(b, "ID3", 3))
        p = 10 + ((size_t)(b[6] & 0x7F) << 21 | (size_t)(b[7] & 0x7F) << 14 | (size_t)(b[8] & 0x7F) << 7 | (b[9] & 0x7F));
    if (n >= 36 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4))
    {
        st.channels = b[22] | b[23] << 8;
        st.rate = (int)(b[24] | b[25] << 8 | b[26] << 16 | (uint32_t)b[27] << 24);
        st.bitrate = (int)((uint32_t)(b[28] | b[29] << 8 | b[30] << 16 | (uint32_t)b[31] << 24) * 8 / 1000);
        return;
    }
    static const int rates[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
    for (; p + 4 <= n && p < 256 * 1024; p++)
    {
        if (b[p] != 0xFF || (b[p + 1] & 0xE0) != 0xE0) continue;
        int ver = (b[p + 1] >> 3) & 3, layer = (b[p + 1] >> 1) & 3, ri = (b[p + 2] >> 2) & 3, bri = b[p + 2] >> 4;
        if (ver == 1 || layer == 0 || ri == 3 || bri == 0 || bri == 15) continue;
        st.rate = rates[ver == 3 ? 0 : ver == 2 ? 1 : 2][ri];
        st.channels = (b[p + 3] >> 6) == 3 ? 1 : 2;
        return;
    }
}

// the tags and the tag's picture: 1 a picture was there (else the page looks in the folder)
EXPORT(web_tags)
int web_tags(const unsigned char *buf, int len)
{
    read_format(buf, (size_t)len);
    changed();
    tag_buf = buf;
    tag_len = (size_t)len;
    track_tags t;
    if (tags_read("mem:tags", &t))
    {
        if (t.title[0]) u_copy(st.title, sizeof st.title, t.title);
        u_copy(st.artist, sizeof st.artist, t.artist);
        u_copy(st.album, sizeof st.album, t.album);
        changed();
        media_meta();
    }
    tag_buf = NULL;
    size_t off, plen;
    if (cover_find_in_tag(buf, (size_t)len, &off, &plen) && off + plen <= (size_t)len)
    {
        cover_from_bytes(buf + off, plen);
        return cover_state() != COVER_NONE;
    }
    return 0;
}
