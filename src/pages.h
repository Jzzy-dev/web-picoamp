// pages.h - the pages besides the player (full screen, clock, weather, news)
// and what they share with it
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "viz.h"

enum { PG_PLAYER, PG_FULL, PG_CLOCK, PG_WX, PG_NEWS, PG_PICS, PG_MARKETS, PG_COUNT };

typedef struct
{
    void (*open)(void);
    void (*frame)(void);                 // draw the page (every frame)
    void (*key)(int k);
    void (*mouse)(int x, int y, int button);
    void (*close)(void);
    void (*describe)(char *out, size_t n);   // for the scripts: what the page shows
} page_t;

extern const page_t page_full, page_clock, page_wx, page_news, page_pics, page_markets;
void pics_saver(void);                   // the screensaver: the pictures page's slideshow

// from ui.c
extern audio_status ui_st;               // the player now
extern viz_data     ui_viz;
void ui_page(int pg);                    // open a page (PG_PLAYER: back to the player)
int  ui_page_now(void);
void fs_cover(void);                     // the full screen page shows the cover (fullscreen.c)
bool ui_playback_key(int k);             // the keys that work on every page
void ui_row0(int row, int col, int width, uint8_t colour);   // what plays: a ticker
const char *ui_now_playing(void);        // "title - artist  . station"
void ui_note(const char *text, bool error);                  // a message on the page
const char *ui_note_text(void);          // the message now ("" when none)
void ui_cover_follow(void);              // the cover of what plays (asked for, taken when it comes)
bool ui_note_error(void);
void ui_eq_next(void);                   // E

// from fullscreen.c etc.
void clock_service_tick(void);           // the sleep timer and the alarm (every frame)
void clock_alarm_ack(void);
void news_open_market(const char *name);  // the news reader with the markets' headlines (ESC: back there)
void news_feed_taken(void);              // the markets fetch a feed: the news page's own is gone
int  news_import(const char *opml_path);  // feeds added, -1: not readable              // a playback key: the alarm has been heard
