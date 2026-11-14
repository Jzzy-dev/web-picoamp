// web.h - what the C program asks of the page (JavaScript, web/picoamp.js)
// and what the page calls in it. The program runs in the page's own thread:
// no threads; what takes time (a fetch, a folder) is asked for and answered
// later through a callback.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define JS(name) __attribute__((import_module("env"), import_name(#name)))
#define EXPORT(name) __attribute__((export_name(#name)))

// time
JS(js_millis)       double js_millis(void);
JS(js_now)          double js_now(void);                 // Unix time, seconds (with fractions)
JS(js_tz_offset)    int    js_tz_offset(double unix_s);  // the browser's zone: seconds east of UTC
JS(js_log)          void   js_log(const char *s, int n);

// the settings' files: a small file system in the browser's storage
JS(js_fs_size)      int    js_fs_size(const char *path, int n);          // -1: none
JS(js_fs_read)      int    js_fs_read(const char *path, int n, void *buf, int max);
JS(js_fs_write)     int    js_fs_write(const char *path, int n, const void *buf, int len);
JS(js_fs_list)      int    js_fs_list(const char *dir, int n, char *out, int max);   // "name\n"...

// a station's logo (Radio Browser's favicon, kept in the browser's cache): the
// answer comes in web_logo(bytes) (len 0: none)
JS(js_station_logo) void   js_station_logo(const char *url, int un, const char *uuid, int uun, const char *name, int nn, const char *rb, int rn);

// the web: fetch url; the answer comes in net_done(id, ...)
JS(js_fetch)        void   js_fetch(int id, const char *url, int n);

// the music folder (picked by the person): its entries come in dir_done(id, ...)
JS(js_music_pick)   void   js_music_pick(void);
JS(js_music_list)   void   js_music_list(int id, const char *path, int n);
JS(js_music_name)   int    js_music_name(char *out, int max);            // 0: no folder

// the sound (an audio element and Web Audio)
JS(js_audio_play)   void   js_audio_play(const char *src, int n, int is_file);
JS(js_audio_pause)  void   js_audio_pause(int on);
JS(js_audio_stop)   void   js_audio_stop(void);
JS(js_audio_seek)   void   js_audio_seek(double s);
JS(js_audio_gain)   void   js_audio_gain(double left, double right);
JS(js_audio_eq)     void   js_audio_eq(int g0, int g1, int g2, int g3, int g4);
JS(js_audio_meta)   void   js_audio_meta(const char *title, int tn, const char *artist, int an, const char *album, int bn);
JS(js_can_play)     int    js_can_play(const char *codec, int n);       // the browser plays it

// covers: a picture TJpgDec cannot read (PNG ...): the browser decodes it; it comes in web_cover_rgb
JS(js_decode_image) int    js_decode_image(const void *data, int len);

// the page
JS(js_open_lists)   void   js_open_lists(void);          // the station lists' editor
JS(js_fullscreen)   void   js_fullscreen(void);

// the place: the browser's own position (asks the person), or none
JS(js_geolocate)    void   js_geolocate(int id);          // the answer: geo_done(id, ok, lat, lon)

// (net_done, dir_done, geo_done and the others are in web.c)
typedef void (*net_cb)(void *user, int status, const char *data, size_t len, const char *err);
void net_fetch(const char *url, net_cb cb, void *user);
typedef void (*dir_cb)(void *user, const char *lines, int ok);
void music_list(const char *path, dir_cb cb, void *user);
