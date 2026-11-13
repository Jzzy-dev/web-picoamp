// library.h - what the list at the bottom of the screen shows: the folders of
// the PC, and the internet radio station lists.
//
// A place is a string:
//   ""                the top: [Internet radio], [Music], the drives
//   "radio:"          the station lists: every .m3u / .pls in <settings>\Radio
//   "radio:Name.m3u"  the stations of one list
//   "rb:..."          the Radio Browser directory: rb:top, rb:votes, rb:trend,
//                     rb:countries, rb:genres, rb:decades, rb:country=NL|Name,
//                     rb:genre=jazz, rb:decade=80s|1980s, rb:search=text
//   anything else     a folder of the PC: its folders and music files
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { E_PARENT = 0, E_FOLDER = 1, E_FILE = 2, E_STATION = 6, E_MORE = 7 };

typedef struct
{
    int  type;
    bool tls;               // a station over https
    char name[256];         // as shown (UTF-8)
    char target[2048];      // the place to open, the file, or the station's URL
    char uuid[40];          // a station of the Radio Browser directory
} lib_entry;

typedef struct
{
    char       place[2048];
    char       title[512];  // the path line ("/Internet radio/Favourites", "C:/Music")
    lib_entry *e;
    int        count;
    bool       ok;
} lib_list;

enum { LIB_FAIL = 0, LIB_OK = 1, LIB_PENDING = 2 };

void lib_init(void);                    // the Radio folder and its first lists
// LIB_PENDING: a list of the station directory comes in the background:
// lib_poll() until it is LIB_OK (or LIB_FAIL, lib_error() says why)
int  lib_open(lib_list *l, const char *place);
int  lib_poll(lib_list *l);
int  lib_more(lib_list *l);             // "More stations...": the next page, added
void lib_cancel(void);
const char *lib_error(void);
void lib_free(lib_list *l);
void lib_parent(const char *place, char *out, size_t size);   // "" at the top
bool lib_is_favourites(const char *place);

// favourites (Radio\Favourites.m3u): 1 added / removed, 0 already / not there, -1 error
int  lib_fav_add(const char *name, const char *url);
int  lib_fav_del(const char *url);
bool lib_fav_move(const char *url, int dir);   // U / J in Internet radio/Favourites
// a dropped .m3u / .pls: copied into the Radio folder; false if it has no stations
bool lib_import_list(const char *path, char *msg, size_t msglen);

bool lib_is_music(const char *name);
