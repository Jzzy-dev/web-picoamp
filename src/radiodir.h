// radiodir.h - the Radio Browser station directory (radio-browser.info),
// as the PicoVerse Audio Engine uses it: a list at a time, in the background.
#pragma once
#include <stdbool.h>

enum { RD_IDLE, RD_LOADING, RD_DONE, RD_FAILED };

typedef struct
{
    char name[128];
    char url[1024];         // url_resolved
    char uuid[40];
    char codec[16];
    int  bitrate;
    int  count;             // countries: their stations
    char code[8];           // countries: ISO 3166-1
} rd_item;

// path: "/json/countries?..." or "/json/stations/search?..."; the reply
// replaces the last list (append: is added to it)
void rd_fetch(const char *path, bool countries, bool append);
int  rd_state(void);                    // RD_*
const char *rd_error(void);
// the list (valid until the next rd_fetch); raw: objects in the last reply
const rd_item *rd_items(int *count, int *raw);
void rd_cancel(void);
void rd_click(const char *uuid);        // tell the directory a station is played
void rd_urlenc(char *out, size_t size, const char *s);
bool rd_codec_ok(const char *codec);    // one PicoAmp plays
const char *rd_codec_query(void);       // "&codec=MP3" or "" (MP3 and AAC)
