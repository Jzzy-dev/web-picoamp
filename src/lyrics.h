// lyrics.h - the words of what plays (L), from LRCLIB (lrclib.net): free, no
// account, no key; for many songs synced (a time on every line), else plain.
// Asked by artist, title, album and length (a station's song: by artist and
// title only), in a thread of its own; kept in <config>/Lyrics ("none" too,
// asked again after a week).
#pragma once
#include <stdbool.h>

enum { LY_NONE, LY_LOADING, LY_SYNCED, LY_PLAIN, LY_INSTRUMENTAL, LY_NOT_FOUND, LY_FAILED };

// what plays now (the same song again: nothing); empty title: none
void lyrics_want(const char *artist, const char *title, const char *album, int seconds);
void lyrics_again(void);                 // look for the song's words again (what was kept for it forgotten)
int  lyrics_state(void);                 // LY_xxx (takes a result that came)
int  lyrics_count(void);                 // lines
const char *lyrics_line(int i, int *ms); // its text; *ms its time (synced), else -1
int  lyrics_at(int ms);                  // the line sung at ms (synced; -1 before the first)
const char *lyrics_error(void);          // LY_FAILED: why
