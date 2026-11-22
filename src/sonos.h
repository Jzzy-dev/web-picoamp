// sonos.h - Sonos speakers in this network, as a player PicoAmp follows: the
// room's song (title, artist, album, its cover), its time, playing or paused,
// and its keys (play / pause, next, previous, seek, shuffle, repeat, volume,
// mute). The sound stays on the speakers. No account and no key: the speakers'
// own UPnP control (port 1400), as the Sonos apps use it in the house.
//
// A "room" is a group of speakers, named after its coordinator ("Living Room",
// "Kitchen + 2"); its id is the coordinator's (RINCON_...). Bonded speakers
// (a pair, a sub, surrounds) are one room.
//
// Windows and the test build: sonos.c (SSDP search over UDP, SOAP over TCP).
// The web: sonos_web.c, through PicoAmp for Windows (its /sonos/... bridge).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "nowplaying.h"

enum { SONOS_IDLE = 0, SONOS_LOADING, SONOS_DONE, SONOS_FAILED };

typedef struct
{
    char id[48];            // the coordinator's UUID (RINCON_...)
    char name[96];          // "Living Room", "Kitchen + 2"
    char ip[48];
    int  port;
    int  members;
} sonos_room;

bool sonos_supported(void);          // false: the entry does not show (the web: no bridge)
// the rooms, searched in the background (fresh: again, also when known)
void sonos_discover(bool fresh);
int  sonos_state(void);              // SONOS_xxx
unsigned sonos_gen(void);            // +1 when the rooms change (the top list shows the entry once there are some)
const char *sonos_error(void);
int  sonos_rooms(sonos_room *out, int max);
bool sonos_room_name(const char *id, char *out, size_t size);

// following one room, as nowplaying.h does for a program of the PC
void sonos_start(const char *id);
void sonos_end(void);
bool sonos_get(np_info *out);
bool sonos_art(uint8_t **data, size_t *len);
void sonos_cmd(int cmd, int arg);    // NP_xxx (NP_SEEK_TO: ms; NP_SHUFFLE 0/1; NP_REPEAT NP_REPEAT_xxx)
void sonos_volume(int volume, bool muted);

#ifdef __wasm__
void sonos_tick(void);               // once a frame: the answers that came, the next ask
#endif

// any room, now (blocking; for the web's bridge: PicoAmp for Windows only)
bool sonos_query(const char *id, np_info *out);
bool sonos_command(const char *id, int cmd, int arg);
bool sonos_set_volume(const char *id, int volume, int muted);   // volume -1 / muted -1: kept
bool sonos_room_art(const char *id, uint8_t **data, size_t *len);
