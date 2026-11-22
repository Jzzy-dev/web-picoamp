// sonos_web.c - Sonos rooms in the browser (sonos.h), through PicoAmp for
// Windows: a page cannot reach the speakers itself (no search in the network,
// no CORS, and a page on https may not ask an http address), so it asks the
// PC's PicoAmp, whose link server (K) answers /sonos/rooms, now, cmd, vol and
// art for any room.
//
// The bridge: PICOAMP_BRIDGE (the page sets it: ?bridge=http://<the PC>:8765,
// kept; the page's own address when it comes from the PC, /web/; else
// http://localhost:8765, PicoAmp for Windows on this PC).
//
// No threads here: one ask at a time, its answer in a callback; sonos_tick()
// asks the followed room again every second and sends the keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sonos.h"
#include "web.h"
#include "sys.h"
#include "util.h"

#define MAX_ROOMS 32
#define POLL_MS   1000

static char        base[256];
static int         dstate = SONOS_IDLE;
static unsigned    dgen;
static char        derr[160];
static sonos_room  rooms[MAX_ROOMS];
static int         n_rooms;
static bool        tried;              // the bridge was asked once (at the start)

static const char *bridge(void)
{
    if (!base[0])
    {
        const char *e = getenv("PICOAMP_BRIDGE");
        u_copy(base, sizeof base, e && *e ? e : "http://localhost:8765");
        size_t l = strlen(base);
        while (l && base[l - 1] == '/') base[--l] = 0;
    }
    return base;
}

// ---- a small reader of the bridge's JSON (its own, known form)
static const char *jfind(const char *j, const char *end, const char *key)
{
    char k[48];
    snprintf(k, sizeof k, "\"%s\":", key);
    size_t kl = strlen(k);
    for (const char *p = j; p + kl <= end; p++)
        if (*p == '"' && !strncmp(p, k, kl))
        {
            p += kl;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            return p;
        }
    return NULL;
}

static bool jstr(const char *j, const char *end, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = jfind(j, end, key);
    if (!p || *p != '"') return false;
    size_t k = 0;
    for (p++; p < end && *p != '"'; p++)
    {
        unsigned c = (unsigned char)*p;
        if (c == '\\' && p + 1 < end)
        {
            p++;
            c = (unsigned char)*p;
            if (c == 'u' && p + 4 < end)
            {
                char h[5] = { p[1], p[2], p[3], p[4], 0 };
                c = (unsigned)strtoul(h, NULL, 16);
                p += 4;
            }
            else if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
        }
        if (k + 4 >= n) break;
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800 && c > 0xFF) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 63)); }
        else out[k++] = (char)c;        // (UTF-8 bytes as they came)
    }
    out[k] = 0;
    return true;
}

static long jnum(const char *j, const char *end, const char *key, long def)
{
    const char *p = jfind(j, end, key);
    return p ? strtol(p, NULL, 10) : def;
}

static bool jbool(const char *j, const char *end, const char *key)
{
    const char *p = jfind(j, end, key);
    return p && !strncmp(p, "true", 4);
}

// ---------------------------------------------------------------- the rooms
static void rooms_done(void *user, int status, const char *data, size_t len, const char *err)
{
    (void)user;
    const char *end = data ? data + len : NULL;
    if (status != 200 || !data)
    {
        if (status) snprintf(derr, sizeof derr, "PicoAmp for Windows said no (%d)", status);
        else snprintf(derr, sizeof derr, "No PicoAmp for Windows at %s (K on there?)", bridge());
        dstate = SONOS_FAILED;
        if (n_rooms) { n_rooms = 0; dgen++; }
        return;
    }
    char st[16];
    jstr(data, end, "state", st, sizeof st);
    sonos_room r[MAX_ROOMS];
    int n = 0;
    // the rooms: one object each
    const char *p = jfind(data, end, "rooms");
    while (p && n < MAX_ROOMS)
    {
        const char *ob = memchr(p, '{', (size_t)(end - p));
        if (!ob) break;
        const char *oe = memchr(ob, '}', (size_t)(end - ob));
        if (!oe) break;
        memset(&r[n], 0, sizeof r[n]);
        jstr(ob, oe, "id", r[n].id, sizeof r[n].id);
        jstr(ob, oe, "name", r[n].name, sizeof r[n].name);
        r[n].members = (int)jnum(ob, oe, "members", 1);
        if (r[n].id[0]) n++;
        p = oe + 1;
    }
    bool same = n == n_rooms && !memcmp(r, rooms, sizeof *r * (size_t)n);
    memcpy(rooms, r, sizeof *r * (size_t)n);
    n_rooms = n;
    if (!same) dgen++;
    if (n) { dstate = SONOS_DONE; derr[0] = 0; }
    else { jstr(data, end, "error", derr, sizeof derr); if (!derr[0]) u_copy(derr, sizeof derr, "No Sonos speakers found"); dstate = SONOS_FAILED; }
}

bool sonos_supported(void) { return true; }
int  sonos_state(void) { return dstate; }
unsigned sonos_gen(void) { return dgen; }
const char *sonos_error(void) { return derr; }

void sonos_discover(bool fresh)
{
    if (dstate == SONOS_LOADING) return;
    tried = true;
    dstate = SONOS_LOADING;
    char u[400];
    snprintf(u, sizeof u, "%s/sonos/rooms%s", bridge(), fresh ? "?fresh=1" : "");
    net_fetch(u, rooms_done, NULL);
}

int sonos_rooms(sonos_room *out, int max)
{
    int n = n_rooms < max ? n_rooms : max;
    memcpy(out, rooms, sizeof *out * (size_t)n);
    return n;
}

bool sonos_room_name(const char *id, char *out, size_t size)
{
    for (int i = 0; i < n_rooms; i++) if (!strcmp(rooms[i].id, id)) { u_copy(out, size, rooms[i].name); return true; }
    return false;
}

// ---------------------------------------------------------------- following one room
#define QMAX 16
static bool     fon;
static char     fid[48];
static np_info  fnow;
static bool     asking, art_asking;
static uint32_t asked_at;
static unsigned art_seen;              // the bridge's "art" (a hash of the picture's address)
static uint8_t *art;
static size_t   art_len;
static unsigned art_gen;
static struct { int cmd, arg; } fq[QMAX];
static int      fqn;
static int      fvol = -1, fmute = -1;
static bool     sending;
static unsigned fsession;              // (an answer for a room no longer followed is dropped)

static void art_ask(void);

static void now_done(void *user, int status, const char *data, size_t len, const char *err)
{
    (void)err;
    asking = false;
    if ((unsigned)(uintptr_t)user != fsession || !fon) return;
    if (status != 200 || !data)
    {
        fnow.state = NP_STOPPED;
        if (!status) snprintf(fnow.artist, sizeof fnow.artist, "No answer from PicoAmp for Windows");
        else { char nm[96] = "The room"; sonos_room_name(fid, nm, sizeof nm); snprintf(fnow.artist, sizeof fnow.artist, "%s does not answer", nm); }
        return;
    }
    const char *end = data + len;
    np_info i;
    memset(&i, 0, sizeof i);
    char st[16];
    jstr(data, end, "state", st, sizeof st);
    i.state = !strcmp(st, "playing") ? NP_PLAYING : !strcmp(st, "paused") ? NP_PAUSED : NP_STOPPED;
    jstr(data, end, "app", i.app, sizeof i.app);
    jstr(data, end, "title", i.title, sizeof i.title);
    jstr(data, end, "artist", i.artist, sizeof i.artist);
    jstr(data, end, "album", i.album, sizeof i.album);
    i.elapsed_ms = (int)jnum(data, end, "elapsed_ms", 0);
    i.total_ms = (int)jnum(data, end, "total_ms", 0);
    i.can_seek = jbool(data, end, "can_seek");
    i.can_next = jbool(data, end, "can_next");
    i.can_prev = jbool(data, end, "can_prev");
    i.can_shuffle = jbool(data, end, "can_shuffle");
    i.can_repeat = jbool(data, end, "can_repeat");
    i.shuffle = jbool(data, end, "shuffle");
    i.repeat = (int)jnum(data, end, "repeat", 0);
    i.volume = (int)jnum(data, end, "volume", -1);
    i.muted = jbool(data, end, "muted");
    i.at = sys_millis();
    unsigned a = (unsigned)strtoul(jfind(data, end, "art") ? jfind(data, end, "art") : "0", NULL, 10);
    // a key sent meanwhile: what it changed stays until the room says it
    if (fvol >= 0) i.volume = fnow.volume;
    if (fmute >= 0) i.muted = fnow.muted;
    i.art_gen = art_gen;
    fnow = i;
    if (a != art_seen) { art_seen = a; art_ask(); }
}

static void art_done(void *user, int status, const char *data, size_t len, const char *err)
{
    (void)err;
    art_asking = false;
    if ((unsigned)(uintptr_t)user != fsession) return;
    free(art);
    art = NULL;
    art_len = 0;
    if (status == 200 && data && len > 16) { art = malloc(len); memcpy(art, data, len); art_len = len; }
    art_gen++;
    fnow.art_gen = art_gen;
}

static void art_ask(void)
{
    if (art_asking) return;
    if (!art_seen) { free(art); art = NULL; art_len = 0; art_gen++; fnow.art_gen = art_gen; return; }
    art_asking = true;
    char u[400];
    snprintf(u, sizeof u, "%s/sonos/art?id=%s&a=%u", bridge(), fid, art_seen);
    net_fetch(u, art_done, (void *)(uintptr_t)fsession);
}

static void sent(void *user, int status, const char *data, size_t len, const char *err)
{
    (void)user; (void)status; (void)data; (void)len; (void)err;
    sending = false;
    asked_at = 0;                      // (ask the room again at once)
}

void sonos_tick(void)
{
    // the rooms at the start only when the bridge was named (or the page comes from the PC): a
    // page that asks localhost unasked makes browsers ask the person about it
    if (!tried) { const char *e = getenv("PICOAMP_BRIDGE"); if (e && *e) sonos_discover(false); else tried = true; }
    if (!fon) return;
    char u[400];
    if (!sending && fqn)
    {
        int c = fq[0].cmd, a = fq[0].arg;
        memmove(fq, fq + 1, sizeof fq[0] * (size_t)(--fqn));
        static const char *const names[] = { "", "toggle", "play", "pause", "pause", "next", "prev", "seek", "shuffle", "repeat" };
        snprintf(u, sizeof u, "%s/sonos/cmd?id=%s&c=%s&%s=%d", bridge(), fid, c >= 1 && c <= 9 ? names[c] : "", c == NP_SEEK_TO ? "ms" : "on", a);
        sending = true;
        net_fetch(u, sent, NULL);
        return;
    }
    if (!sending && (fvol >= 0 || fmute >= 0))
    {
        int k = snprintf(u, sizeof u, "%s/sonos/vol?id=%s", bridge(), fid);
        if (fvol >= 0) k += snprintf(u + k, sizeof u - (size_t)k, "&v=%d", fvol);
        if (fmute >= 0) snprintf(u + k, sizeof u - (size_t)k, "&m=%d", fmute);
        fvol = fmute = -1;
        sending = true;
        net_fetch(u, sent, NULL);
        return;
    }
    if (!asking && !sending && (!asked_at || sys_millis() - asked_at >= POLL_MS))
    {
        asked_at = sys_millis();
        asking = true;
        snprintf(u, sizeof u, "%s/sonos/now?id=%s", bridge(), fid);
        net_fetch(u, now_done, (void *)(uintptr_t)fsession);
    }
}

void sonos_start(const char *id)
{
    sonos_end();
    fon = true;
    fsession++;
    u_copy(fid, sizeof fid, id);
    memset(&fnow, 0, sizeof fnow);
    char nm[96];
    if (sonos_room_name(id, nm, sizeof nm)) snprintf(fnow.app, sizeof fnow.app, "Sonos %s", nm);
    else u_copy(fnow.app, sizeof fnow.app, "Sonos");
    fnow.state = NP_STOPPED;
    fnow.volume = -1;
    fqn = 0;
    fvol = fmute = -1;
    asked_at = 0;
    art_seen = 0;
    free(art);
    art = NULL;
    art_len = 0;
}

void sonos_end(void) { fon = false; fsession++; }

bool sonos_get(np_info *out)
{
    if (!fon) return false;
    *out = fnow;
    return true;
}

bool sonos_art(uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    if (!art || !art_len) return false;
    *data = malloc(art_len);
    memcpy(*data, art, art_len);
    *len = art_len;
    return true;
}

void sonos_cmd(int cmd, int arg)
{
    if (!fon) return;
    bool ok = !((cmd == NP_NEXT && !fnow.can_next) || (cmd == NP_PREV && !fnow.can_prev) || (cmd == NP_SEEK_TO && !fnow.can_seek)
                || (cmd == NP_SHUFFLE && !fnow.can_shuffle) || (cmd == NP_REPEAT && !fnow.can_repeat));
    if (cmd == NP_TOGGLE) cmd = fnow.state == NP_PLAYING ? NP_PAUSE : NP_PLAY;
    if (!ok || fqn >= QMAX) return;
    fq[fqn].cmd = cmd;
    fq[fqn].arg = arg;
    fqn++;
    if (cmd == NP_PLAY) fnow.state = NP_PLAYING;
    if (cmd == NP_PAUSE && fnow.state == NP_PLAYING) fnow.state = NP_PAUSED;
    if (cmd == NP_SHUFFLE) fnow.shuffle = arg != 0;
    if (cmd == NP_REPEAT) fnow.repeat = arg;
    if (cmd == NP_SEEK_TO) { fnow.elapsed_ms = arg; fnow.at = sys_millis(); }
}

void sonos_volume(int volume, bool muted)
{
    if (!fon) return;
    if (volume != fnow.volume) fvol = volume;
    if (muted != fnow.muted) fmute = muted ? 1 : 0;
    fnow.volume = volume;
    fnow.muted = muted;
}
