// wxsvc.h - the place, its time zone and its weather, for the clock and the
// weather pages (as the engine's pva_clock.c): in the background.
//
// Place:  [location] mode in picoamp.ini:
//           ip     GeoJS (get.geojs.io): the city and coordinates of the
//                  internet connection (free, no key)
//           place  a place found by name (Open-Meteo's geocoding: free, no key)
// Zone and weather: Open-Meteo's forecast with timezone=auto, every 30 minutes:
//         the UTC offset in force now, the weather now and the next days.
// Time:   the device's clock (UTC) plus the place's offset.
// wx_tick, once a frame, asks what is due (the web answers later).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WX_DAYS 6

typedef struct { int year, month, day, wday; int code, pp; int tmax10, tmin10; bool ok; } wx_day;

typedef struct
{
    unsigned gen;                // changes with every new piece
    bool   have_place, have_zone, have_wx;
    char   place[160];           // "Amsterdam, North Holland, NL"
    int    utc_offset;           // seconds
    // now
    int    t10, feel10, hum, wind, code;
    bool   is_day;
    wx_day days[WX_DAYS];
    int    ndays;
    char   error[200];
} wx_info;

typedef struct { int year, month, day, wday, hour, min, sec; } wx_time;   // wday 0 = Monday

void wx_start(void);
void wx_tick(void);                    // reads cfg's [location]; begins
void wx_get(wx_info *out);
void wx_refresh(void);                  // the forecast again now
// the time: of the place (internet) or the PC's own zone; true: the place's
bool wx_now(wx_time *t, bool place_time);

// a place by name: results come in the background
enum { WXS_IDLE, WXS_BUSY, WXS_DONE, WXS_FAILED };
void wx_search(const char *name);
int  wx_search_state(void);
int  wx_search_results(char names[][160], int max);
void wx_choose(int i);                  // one of the results: the place
void wx_use_ip(void);                   // the internet connection's place

const char *wx_words(int code);         // "Rain" (the WMO code's word)
