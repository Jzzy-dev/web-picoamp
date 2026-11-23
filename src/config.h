// config.h - the settings, kept in picoamp.ini in the settings folder
// (%APPDATA%\PicoAmp), as key = value lines a person can edit too.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "eq.h"

enum { SCALE_FIT = 0, SCALE_INTEGER = 1, SCALE_STRETCH = 2 };

typedef struct
{
    int    volume;           // 0..100
    bool   muted;            // (not kept: PicoAmp starts with the sound on)
    int    mode;             // MODE_REPEAT | MODE_SHUFFLE
    int8_t eq[EQ_BANDS];
    int    balance;          // 0 left .. 50 centre .. 100 right
    int    view;             // 0 spectrum, 1 scope
    char   music_dir[1024];  // the [Music] entry of the list
    char   start_path[2048]; // the list's folder when it was closed
    // the window
    int    win_x, win_y, win_w, win_h;   // client size; x = y = -1: let Windows choose
    bool   maximized, fullscreen;
    int    scale;            // SCALE_*
    bool   scanlines;
    bool   on_top;
    bool   first_run;        // there was no picoamp.ini yet
    // [clock]
    bool   clock_analog;
    bool   clock_pc_time;    // the PC's zone (else the place's, from the internet)
    int    clock_colours;    // 0 music, 1 cyan, 2 green, 3 amber, 4 red, 5 purple, 6 white
    bool   clock_spectrum;
    // [location]
    bool   loc_ip;           // the internet connection's place (ip-api.com)
    char   loc_name[160];
    double loc_lat, loc_lon;
    // [alarm]
    bool   alarm_on, alarm_weekdays;
    int    alarm_h, alarm_m;
    char   alarm_play[2048]; // a station's URL or a file
    char   alarm_name[256];
    // [news]
    bool   news_by_feed;     // the feed's order (else newest first)
    // [markets]
    int    mk_region;        // 0 Europe, 1 Americas, 2 Asia-Pacific
    int    mk_sort;          // 0 the list's order, 1 the risers first, 2 the fallers first
    int    mk_range;         // the chart's range: 0 1D .. 7 all
    int    mk_refresh;       // seconds between the prices
    char   mk_list[1024];    // My list: Yahoo Finance symbols, a comma between them
    // [cover]
    bool   cover_rows;       // five colours of its own for every row of the cover's cells (else for all)
    bool   cover_sharp;      // the cover's own picture, sharp, over the MSX's (F5); else as the MSX (8-bit)
    int    keep_logos, keep_covers;   // how many are kept (0: all)
    // [pictures]
    int    pic_saver;        // the screensaver after this many minutes without a key (0: off)
    int    pic_interval;     // seconds a picture shows in a slideshow
    bool   pic_shuffle;
    char   pic_folder[1024]; // the screensaver's folder ("": the Pictures folder)
} config;

extern config cfg;

void config_load(void);
void config_save(void);
const char *config_radio_dir(void);     // <settings>\Radio: the station lists
