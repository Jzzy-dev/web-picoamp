// audio.h - the player: what the PicoVerse Audio Engine does on the RP2350,
// here on the PC. A play list of files or one internet station; its own
// decoding thread; the equalizer; the sound for the visualiser.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "eq.h"

enum { ST_STOPPED = 0, ST_PLAYING = 1, ST_PAUSED = 2 };
enum { MODE_REPEAT = 1, MODE_SHUFFLE = 2 };

typedef struct
{
    unsigned gen;            // changes whenever something below changes
    int  state;
    int  volume;             // 0..100
    bool muted;
    int  elapsed, total;     // seconds (total 0: unknown)
    int  bitrate;            // kbit/s
    int  rate;               // Hz
    int  channels;
    bool seekable, stream, buffering, tls;
    int  buf_pct;            // streams: how full the buffer is
    int  pl_pos, pl_count;   // place in the play list
    int  mode;               // MODE_*
    char title[256];
    char artist[256];        // a station: empty
    char album[256];         // a station: its name
    char url[2048];          // the station that plays (or the file)
    char error[160];         // the last error (shown once: err_gen)
    unsigned err_gen;
} audio_status;

bool audio_init(bool null_device);
void audio_shutdown(void);

// play files[start] of a list of files (UTF-8 paths); the list is copied
void audio_play_files(const char *const *files, int count, int start);
void audio_play_url(const char *url, const char *name);
// a list's stations: the play list (N / P the next / previous station)
void audio_play_stations(const char *const *urls, const char *const *names, int count, int start);
void audio_toggle_pause(void);
void audio_stop(void);
void audio_next(void);
void audio_prev(void);
void audio_seek_rel(int seconds);
void audio_seek_to(int seconds);
void audio_set_volume(int v);
void audio_set_mute(bool m);
void audio_set_balance(int b);       // 0 left .. 50 centre .. 100 right
void audio_set_fade(int permille);   // the sleep timer and the alarm: 0..1000 of the volume
void audio_set_mode(int mode);
void audio_set_eq(const int8_t gains[EQ_BANDS]);

void audio_status_get(audio_status *s);
