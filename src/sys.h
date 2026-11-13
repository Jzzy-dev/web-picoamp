// sys.h - the little the program needs from the operating system: threads,
// locks, time, folders and files (UTF-8 paths with '/' or '\').
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct sys_thread sys_thread;
typedef struct sys_mutex  sys_mutex;

sys_thread *sys_thread_start(int (*fn)(void *), void *arg);
void        sys_thread_join(sys_thread *t);
void        sys_thread_detach(sys_thread *t);   // it ends by itself
sys_mutex  *sys_mutex_new(void);
void        sys_lock(sys_mutex *m);
void        sys_unlock(sys_mutex *m);
void        sys_sleep_ms(unsigned ms);
uint32_t    sys_millis(void);
int64_t     sys_unix_time(void);
struct tm;
void        sys_localtime(int64_t t, struct tm *out);   // the device's own zone

// a folder's entries; is_dir set for folders. Returns false if it cannot be read.
typedef struct { char *name; bool is_dir; uint64_t size; } sys_dirent;
bool  sys_list_dir(const char *path, sys_dirent **out, int *count);
void  sys_free_dir(sys_dirent *d, int count);
bool  sys_is_dir(const char *path);
bool  sys_mkdir(const char *path);         // and its parents
bool  sys_file_exists(const char *path);
char *sys_read_file(const char *path, size_t *len);   // malloc'd, 0-terminated
bool  sys_write_file(const char *path, const void *data, size_t len);
void *sys_fopen(const char *path, const char *mode);  // a FILE *

// where things are
const char *sys_config_dir(void);          // %APPDATA%\PicoAmp (~/.config/picoamp)
const char *sys_music_dir(void);           // the user's Music folder
int   sys_drives(char letters[26]);        // Windows: "CDE..."; 0 elsewhere
