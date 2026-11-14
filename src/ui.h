// ui.h - PICOAMP's screen and keys, drawn into the gfx picture
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// keys: printable ones as their character; these for the rest
enum
{
    K_ENTER = 13, K_BS = 8, K_ESC = 27,
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END, K_DEL,
    K_MEDIA_PLAY, K_MEDIA_STOP, K_MEDIA_NEXT, K_MEDIA_PREV, K_VOL_UP, K_VOL_DOWN, K_VOL_MUTE,
    K_CTRL_UP, K_CTRL_DOWN,
    K_F1 = 0x120                          // the help, from every page
};
enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_WHEEL_UP = 4, MOUSE_WHEEL_DOWN = 5, MOUSE_DRAG = 6, MOUSE_UP = 7 };

void ui_init(void);
void ui_frame(void);                      // once a frame: read the player, draw
void ui_key(int key);
void ui_mouse(int x, int y, int button);  // x, y in the 256 x 192 picture
void ui_drop(const char *const *paths, int n);   // files dropped on the window
void ui_message(const char *text, bool error);
bool ui_quit_requested(void);
void ui_save_state(void);
void ui_logo(const uint8_t *data, size_t len);   // the page: the logo of the station that plays (len 0: none)
void ui_reload(void);                     // the place again (its lists were changed)                 // into cfg before it is saved
// for the scripts: the list (path, entries) or what the page shows, as text (malloc'd)
char *ui_describe(bool the_list);

// from the platform
void plat_open_folder(const char *path);
void plat_quit(void);
void plat_minimize(void);                 // ESC with nothing to close: the music plays on
