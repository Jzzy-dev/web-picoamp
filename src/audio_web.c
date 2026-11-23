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
#include "sonos.h"
#include "config.h"

static audio_status st;
static char **pl;                   // files, or "url\x1Fname" for stations
static int   pl_n, pl_pos;
static bool  pl_stations;
static int   volume = 80, balance = 50, fade = 1000;
static bool  muted;

static void changed(void) { st.gen++; }

// ---------------- a Sonos room (sonos_web.c) ----------------
static bool     following;
static char     fol_id[64];
static np_info  sys_now;
static uint32_t bars_at, bars_rng = 12345;
static float    bars_lp[4], bars_env, bars_beat, bars_amp[4] = { 1.0f, 0.6f, 0.35f, 0.2f };
static void     free_playlist(void);

static void sys_leave(bool pause_it)
{
    if (!following) return;
    if (pause_it && sys_now.state == NP_PLAYING) sonos_cmd(NP_PAUSE, 0);
    sonos_tick();                       // (the key goes out before the room is let go)
    following = false;
    sonos_end();
    fol_id[0] = 0;
    st.system = false;
    st.source[0] = st.title[0] = st.artist[0] = st.album[0] = st.url[0] = 0;
    st.state = ST_STOPPED;
    st.elapsed = st.total = 0;
    st.volume = volume;
    st.muted = muted;
    st.seekable = false;
    changed();
}

void audio_follow_sonos(const char *id)
{
    if (following && !strcmp(id, fol_id)) return;
    if (following) sys_leave(false);
    js_audio_stop();
    free_playlist();
    following = true;
    u_copy(fol_id, sizeof fol_id, id);
    sonos_start(id);
    memset(&sys_now, 0, sizeof sys_now);
    st.system = true;
    st.stream = st.buffering = st.tls = false;
    st.state = ST_STOPPED;
    st.title[0] = st.artist[0] = st.album[0] = 0;
    st.bitrate = st.rate = st.channels = 0;
    st.pl_pos = st.pl_count = 0;
    snprintf(st.url, sizeof st.url, "sonos:%s", id);
    cover_none();
    changed();
}

bool audio_follow_art(uint8_t **data, size_t *len) { return following && sonos_art(data, len); }

static float bars_rand(void)
{
    bars_rng = bars_rng * 1664525u + 1013904223u;
    return (float)((bars_rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

// the room's sound does not come here: the bars move on a pretend sound while it plays
static void pretend_bars(void)
{
    uint32_t now = sys_millis();
    if (sys_now.state != NP_PLAYING || sys_now.muted) { bars_at = now; return; }
    if ((int32_t)(now - bars_at) > 100) bars_at = now - 10;
    static int16_t pcm[480 * 2];
    static const float cut[4] = { 0.02f, 0.08f, 0.25f, 0.6f };
    float vol = sys_now.volume >= 0 ? 0.35f + 0.65f * (float)sys_now.volume / 100.0f : 1.0f;
    while ((int32_t)(now - bars_at) >= 10)
    {
        bars_at += 10;
        bars_beat += 0.02f + 0.002f * bars_rand();
        if (bars_beat >= 1.0f) { bars_beat -= 1.0f; bars_env = 1.0f; }
        bars_env *= 0.93f;
        for (int k = 0; k < 4; k++)
        {
            bars_amp[k] += 0.03f * bars_rand();
            if (bars_amp[k] < 0.1f) bars_amp[k] = 0.1f;
            if (bars_amp[k] > 1.0f) bars_amp[k] = 1.0f;
        }
        for (int n = 0; n < 480; n++)
        {
            float w = bars_rand(), o = 0;
            for (int k = 0; k < 4; k++)
            {
                bars_lp[k] += cut[k] * (w - bars_lp[k]);
                float band = k ? bars_lp[k] - bars_lp[k - 1] : bars_lp[0] * (1.0f + 2.5f * bars_env);
                o += band * bars_amp[k];
            }
            int v = (int)(o * 9000.0f * vol);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[2 * n] = pcm[2 * n + 1] = (int16_t)v;
        }
        viz_feed(pcm, 480, 48000);
    }
}

// what the room says now, as the player's status
static void sys_poll(void)
{
    sonos_tick();
    if (!following) return;
    np_info i;
    if (!sonos_get(&i)) return;
    sys_now = i;
    audio_status o = st;
    st.state = i.state == NP_PLAYING ? ST_PLAYING : i.state == NP_PAUSED ? ST_PAUSED : ST_STOPPED;
    u_copy(st.source, sizeof st.source, i.app);
    u_copy(st.title, sizeof st.title, i.title);
    u_copy(st.artist, sizeof st.artist, i.artist);
    u_copy(st.album, sizeof st.album, i.album);
    int e = i.elapsed_ms;
    if (i.state == NP_PLAYING) e += (int)(sys_millis() - i.at);
    if (i.total_ms && e > i.total_ms) e = i.total_ms;
    st.elapsed = e / 1000;
    st.total = i.total_ms / 1000;
    st.seekable = i.can_seek && i.total_ms > 0;
    st.mode = (i.shuffle ? MODE_SHUFFLE : 0) | (i.repeat != NP_REPEAT_OFF ? MODE_REPEAT : 0);
    if (i.volume >= 0) { st.volume = i.volume; st.muted = i.muted; }
    st.art_gen = i.art_gen;
    unsigned g = st.gen;
    st.gen = o.gen;
    if (memcmp(&o, &st, sizeof st)) g++;
    st.gen = g;
    pretend_bars();
}

// the volume outside (the TV's own, the Android app): the sound at full, the volume only shown and passed on
static bool ext_volume;

static void apply_gain(void)
{
    int v = ext_volume ? 100 : volume;
    double g = (muted || v <= 0) ? 0 : pow(10.0, -(100 - v) / 40.0) * fade / 1000.0;
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
    sys_leave(true);
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
    sys_leave(true);
    free_playlist();
    st.pl_pos = st.pl_count = 0;
    start_url(url, name);
}

void audio_play_stations(const char *const *urls, const char *const *names, int count, int start)
{
    sys_leave(true);
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
    if (following) { sonos_cmd(NP_TOGGLE, 0); return; }
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
    if (following) { sonos_cmd(NP_PAUSE, 0); return; }
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

void audio_next(void) { if (following) sonos_cmd(NP_NEXT, 0); else if (pl_n) start_track(pick(1)); }

void audio_prev(void)
{
    if (following) { sonos_cmd(NP_PREV, 0); return; }
    if (!pl_n) return;
    if (!pl_stations && st.elapsed >= 3) { js_audio_seek(0); return; }   // back to the start first
    start_track(pick(-1));
}

void audio_seek_to(int s)
{
    if (following) { sonos_cmd(NP_SEEK_TO, (s < 0 ? 0 : s) * 1000); return; }
    if (st.stream || st.state == ST_STOPPED) return;
    if (s < 0) s = 0;
    if (st.total && s >= st.total) s = st.total - 1;
    js_audio_seek(s);
    st.elapsed = s;
    changed();
}

void audio_seek_rel(int s) { audio_seek_to(st.elapsed + s); }

void audio_set_volume(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (following && sys_now.volume >= 0) { sonos_volume(v, st.muted); st.volume = v; changed(); return; }
    volume = v; st.volume = volume; apply_gain(); changed();
}
void audio_ext_volume(bool on) { ext_volume = on; apply_gain(); }
int audio_own_volume(void) { return volume; }
// the volume outside changed (the TV's buttons): shown, not passed back
void audio_ext_volume_is(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    volume = v;
    cfg.volume = v;                     // (the player's + and - go on from it)
    if (!(following && sys_now.volume >= 0)) { st.volume = v; changed(); }
}
void audio_set_mute(bool m)
{
    if (following && sys_now.volume >= 0) { sonos_volume(st.volume, m); st.muted = m; changed(); return; }
    muted = m; st.muted = m; apply_gain(); changed();
}
void audio_set_balance(int b) { balance = b < 0 ? 0 : b > 100 ? 100 : b; apply_gain(); }
void audio_set_fade(int p) { p = p < 0 ? 0 : p > 1000 ? 1000 : p; if (p != fade) { fade = p; apply_gain(); } }
void audio_set_mode(int m)
{
    if (following)
    {
        // the room's shuffle and repeat (repeat: of its queue)
        if ((m ^ st.mode) & MODE_SHUFFLE) sonos_cmd(NP_SHUFFLE, (m & MODE_SHUFFLE) != 0);
        if ((m ^ st.mode) & MODE_REPEAT) sonos_cmd(NP_REPEAT, (m & MODE_REPEAT) ? NP_REPEAT_LIST : NP_REPEAT_OFF);
    }
    st.mode = m; changed();
}
void audio_set_eq(const int8_t g[EQ_BANDS]) { js_audio_eq(g[0], g[1], g[2], g[3], g[4]); }

void audio_status_get(audio_status *s)
{
    if (following) sys_poll();
    else sonos_tick();
    *s = st;
}

// ---------------- what the page tells ----------------
EXPORT(web_audio_playing)
void web_audio_playing(int rate, int channels)
{
    if (following) return;                 // (the page's sound is not the room's)
    st.buffering = false;
    // (a station's own rate is not known here: the browser resamples it)
    if (!st.stream && !st.rate) { if (rate) st.rate = rate; if (channels) st.channels = channels; }
    if (st.state == ST_STOPPED) st.state = ST_PLAYING;
    changed();
}

EXPORT(web_audio_waiting) void web_audio_waiting(void) { if (!following) { st.buffering = true; changed(); } }

EXPORT(web_audio_time)
void web_audio_time(double elapsed, double total)
{
    if (following) return;                 // (the page's sound is not the room's)
    int e = (int)elapsed, t = total > 0 && isfinite(total) ? (int)(total + 0.5) : 0;
    if (e != st.elapsed || t != st.total)
    {
        st.elapsed = e;
        st.total = t;
        if (!st.stream && t > 0 && st.bitrate == 0 && st.total) {}
        changed();
    }
}

EXPORT(web_audio_bitrate) void web_audio_bitrate(int kbps) { if (!following) { st.bitrate = kbps; changed(); } }

EXPORT(web_audio_ended)
void web_audio_ended(void)
{
    if (following) return;                 // (the page's sound is not the room's)
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
    if (following) return;                 // (the page's sound is not the room's)
    js_audio_stop();
    st.state = ST_STOPPED;
    st.buffering = false;
    set_error(msg);
}

// a stream's title (the page reads it from the station): "Artist - Title" split as the MSX does
EXPORT(web_audio_title)
void web_audio_title(const char *t)
{
    if (following) return;                 // (the page's sound is not the room's)
    const char *d = strstr(t, " - ");
    if (d && d != t)
    {
        size_t a = (size_t)(d - t);
        if (a >= sizeof st.artist) a = sizeof st.artist - 1;
        memcpy(st.artist, t, a);
        st.artist[a] = 0;
        u_copy(st.title, sizeof st.title, d + 3);
    }
    else
    {
        u_copy(st.title, sizeof st.title, t);
        st.artist[0] = 0;
    }
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
