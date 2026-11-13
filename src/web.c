// web.c - the operating system for the web: sys.h through the page (web.h),
// the callbacks of fetches and folders, and what the page calls each frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "web.h"
#include "sys.h"
#include "util.h"
#include "gfx.h"
#include "ui.h"
#include "config.h"
#include "audio.h"
#include "viz.h"
#include "script.h"
#include "wxsvc.h"

// ---------------- sys.h ----------------
struct sys_mutex { int unused; };
static struct sys_mutex one_mutex;

sys_thread *sys_thread_start(int (*fn)(void *), void *arg) { (void)fn; (void)arg; return NULL; }
void sys_thread_join(sys_thread *t) { (void)t; }
void sys_thread_detach(sys_thread *t) { (void)t; }
sys_mutex *sys_mutex_new(void) { return &one_mutex; }
void sys_lock(sys_mutex *m) { (void)m; }
void sys_unlock(sys_mutex *m) { (void)m; }
void sys_sleep_ms(unsigned ms) { (void)ms; }
uint32_t sys_millis(void) { return (uint32_t)(uint64_t)js_millis(); }
int64_t sys_unix_time(void) { return (int64_t)js_now(); }

const char *sys_config_dir(void) { return "/PicoAmp"; }
const char *sys_music_dir(void) { return "music:"; }
int sys_drives(char letters[26]) { (void)letters; return 0; }
bool sys_mkdir(const char *path) { (void)path; return true; }

bool sys_file_exists(const char *path) { return js_fs_size(path, (int)strlen(path)) >= 0; }

bool sys_is_dir(const char *path)
{
    char b[16];
    return js_fs_list(path, (int)strlen(path), b, sizeof b) >= 0;
}

char *sys_read_file(const char *path, size_t *len)
{
    int n = js_fs_size(path, (int)strlen(path));
    if (n < 0) return NULL;
    char *b = malloc((size_t)n + 1);
    int got = js_fs_read(path, (int)strlen(path), b, n);
    if (got < 0) got = 0;
    b[got] = 0;
    if (len) *len = (size_t)got;
    return b;
}

bool sys_write_file(const char *path, const void *data, size_t len)
{
    return js_fs_write(path, (int)strlen(path), data, (int)len) >= 0;
}

void *sys_fopen_tags(void);
void *sys_fopen(const char *path, const char *mode)
{
    (void)mode;
    return !strcmp(path, "mem:tags") ? sys_fopen_tags() : NULL;
}

bool sys_list_dir(const char *path, sys_dirent **out, int *count)
{
    *out = NULL;
    *count = 0;
    static char buf[65536];
    int n = js_fs_list(path, (int)strlen(path), buf, sizeof buf - 1);
    if (n < 0) return false;
    buf[n] = 0;
    int cap = 0;
    char *cur = buf, *line;
    while ((line = u_token(&cur, "\n")))
    {
        if (*count == cap) { cap = cap ? cap * 2 : 32; *out = realloc(*out, sizeof **out * (size_t)cap); }
        sys_dirent *e = &(*out)[(*count)++];
        e->is_dir = line[0] == 'd';
        e->name = strdup(line + 2);
        e->size = 0;
    }
    return true;
}

void sys_free_dir(sys_dirent *d, int count)
{
    for (int i = 0; i < count; i++) free(d[i].name);
    free(d);
}

// the local time of a Unix time: in the browser's zone
void sys_localtime(int64_t t, struct tm *out)
{
    time_t l = (time_t)(t + js_tz_offset((double)t));
    gmtime_r(&l, out);
}

// ---------------- fetches and folders ----------------
typedef struct { net_cb cb; void *user; bool used; } pending_t;
static pending_t pend[64];

void net_fetch(const char *url, net_cb cb, void *user)
{
    for (int i = 0; i < 64; i++)
        if (!pend[i].used)
        {
            pend[i].used = true;
            pend[i].cb = cb;
            pend[i].user = user;
            js_fetch(i, url, (int)strlen(url));
            return;
        }
    cb(user, 0, NULL, 0, "Too many requests at once");
}

EXPORT(net_done)
void net_done(int id, int status, const char *data, int len, const char *err)
{
    if (id < 0 || id >= 64 || !pend[id].used) return;
    pending_t p = pend[id];
    pend[id].used = false;
    p.cb(p.user, status, data, len > 0 ? (size_t)len : 0, err ? err : "");
}

typedef struct { dir_cb cb; void *user; bool used; } dpending_t;
static dpending_t dpend[8];

void music_list(const char *path, dir_cb cb, void *user)
{
    for (int i = 0; i < 8; i++)
        if (!dpend[i].used)
        {
            dpend[i] = (dpending_t){ cb, user, true };
            js_music_list(i, path, (int)strlen(path));
            return;
        }
    cb(user, "", 0);
}

EXPORT(dir_done)
void dir_done(int id, const char *lines, int ok)
{
    if (id < 0 || id >= 8 || !dpend[id].used) return;
    dpending_t p = dpend[id];
    dpend[id].used = false;
    p.cb(p.user, lines ? lines : "", ok);
}

// memory for the page to hand things over in
EXPORT(web_alloc) void *web_alloc(int n) { return malloc((size_t)(n > 0 ? n : 1)); }
EXPORT(web_free) void web_free(void *p) { free(p); }

// ---------------- the platform's own (ui.h) ----------------
void plat_open_folder(const char *path) { (void)path; js_open_lists(); }
void plat_quit(void) { audio_stop(); }
void plat_minimize(void) {}

// ---------------- what the page calls ----------------
static bool started;

EXPORT(web_start)
void web_start(void)
{
    gfx_init();
    config_load();
    // the music folder may only be read again once the person asks for it
    if (!strncmp(cfg.start_path, "music:", 6) || !strncmp(cfg.start_path, "rb:", 3)) u_copy(cfg.start_path, sizeof cfg.start_path, "radio:");
    audio_init(false);
    ui_init();
    started = true;
    ui_save_state();
    config_save();
}

EXPORT(web_frame)
void web_frame(void)
{
    if (!started) return;
    wx_tick();
    ui_frame();
}

EXPORT(web_key) void web_key(int k) { if (started) ui_key(k); }
EXPORT(web_mouse) void web_mouse(int x, int y, int b) { if (started) ui_mouse(x, y, b); }
EXPORT(web_reload) void web_reload(void) { if (started) ui_reload(); }
EXPORT(web_save) void web_save(void) { if (started) { ui_save_state(); config_save(); } }

// the picture
EXPORT(web_fb) uint8_t *web_fb(void) { return &gfx_fb[0][0]; }
EXPORT(web_fb_stride) int web_fb_stride(void) { return GFX_MAXW; }
EXPORT(web_width) int web_width(void) { return gfx_w; }
EXPORT(web_height) int web_height(void) { return gfx_h; }
EXPORT(web_palette) uint32_t *web_palette(void) { return gfx_pal; }

// the settings the page itself needs (scaling, scan lines)
EXPORT(web_cfg_scale) int web_cfg_scale(void) { return cfg.scale; }
EXPORT(web_cfg_scanlines) int web_cfg_scanlines(void) { return cfg.scanlines; }
EXPORT(web_cfg_set_view)
void web_cfg_set_view(int scale, int scan) { cfg.scale = scale; cfg.scanlines = scan != 0; }
EXPORT(web_message) void web_message(const char *s, int error) { ui_message(s, error != 0); }

// the scripts (the tests): run a script; web_script_step once a frame
static char script_out[65536];
static FILE *script_fp;
EXPORT(web_script)
void web_script(const char *text)
{
    script_out[0] = 0;
    if (script_fp) fclose(script_fp);
    script_fp = fmemopen(script_out, sizeof script_out - 1, "w");
    script_start(text, script_fp);
}
EXPORT(web_script_step) int web_script_step(void) { int r = script_step(); if (script_fp) fflush(script_fp); return r; }
EXPORT(web_script_out) const char *web_script_out(void) { return script_out; }

// the sound for the analyser: what is heard now (stereo 16-bit)
static int16_t viz_buf[2 * 4096];
EXPORT(web_viz_buf) int16_t *web_viz_buf(void) { return viz_buf; }
EXPORT(web_viz_feed) void web_viz_feed(int frames, int rate) { if (frames > 4096) frames = 4096; viz_feed(viz_buf, (uint32_t)frames, (uint32_t)rate); }

// files dropped on the page (the page has put them where these names lead): "\n" between
EXPORT(web_drop)
void web_drop(char *names)
{
    const char *v[64];
    int n = 0;
    char *cur = names, *t;
    while (n < 64 && (t = u_token(&cur, "\n"))) if (*t) v[n++] = t;
    if (n) ui_drop(v, n);
}
