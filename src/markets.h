// markets.h - the market monitor ($): its catalogue (as the PicoVerse Audio
// Engine's pva_markets_cat.c) and the service that fetches the prices, a chart
// and the region's headlines in the background (mktsvc.c), from Yahoo Finance's
// chart service (unofficial, no key, delayed by 15 to 20 minutes for most exchanges).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct { const char *sym, *name; uint8_t dec; } mk_item;
typedef struct { uint8_t region; const char *name; const mk_item *items; uint8_t n; } mk_list;
typedef struct { const char *sym, *name; const mk_item *items; uint8_t n; } mk_members;
extern const char *const mk_region_codes[];         // NULL at the end
extern const char *const mk_region_names[];
extern const mk_list mk_lists[];                    // name NULL at the end
extern const mk_members mk_member_lists[];          // sym NULL at the end

#define MS_ROWS   100                               // instruments in a list at most
#define MS_RANGES 8                                 // 1D 5D 1M 6M YTD 1Y 5Y all

enum { MS_IDLE, MS_BUSY, MS_DONE, MS_FAILED };

typedef struct { bool have; double price, prev; } ms_quote;

void ms_init(void);
void ms_poll(void);                                 // the browser's: each frame (no threads there)
// the list whose prices are fetched (now, then every refresh seconds); NULL: none
void ms_set_list(const mk_item *items, int n);
void ms_refresh_now(void);
void ms_set_refresh(int seconds);
int  ms_state(void);
const char *ms_error(void);
uint32_t ms_changes(void);                          // goes up when prices came
int64_t ms_updated(void);                           // when (UTC seconds; 0: not yet)
bool ms_quote_get(int row, ms_quote *q);

// a chart: one instrument over a range (0 1D .. 7 all)
void ms_chart(const char *sym, int range);
int  ms_chart_state(void);
typedef struct
{
    int      n;                                     // points
    int64_t *t;                                     // their times (UTC)
    double  *v;                                     // their prices
    double   prev, price;                           // the close before the range (prev 0: none), the price now
    int64_t  start, end;                            // 1D: the exchange's day (0: not known)
    int      range;
} ms_series;
bool ms_chart_get(ms_series *s);                    // a copy; free with ms_series_free
void ms_series_free(ms_series *s);

// the region's market headlines (0 Europe, 1 Americas, 2 Asia-Pacific), one after the other
void ms_tape_region(int region);
bool ms_tape_get(char *out, size_t n);              // false: none yet

// Yahoo Finance's addresses (the test build: PICOAMP_YAHOO and PICOAMP_YFEEDS)
void ms_feed_url(const char *sym, char *out, size_t n);   // an instrument's headlines (RSS)
