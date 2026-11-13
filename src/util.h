// util.h - small string helpers
#pragma once
#include <stddef.h>
#include <stdbool.h>

int   u_strnicmp(const char *a, const char *b, size_t n);
int   u_stricmp(const char *a, const char *b);
bool  u_ends_with_ci(const char *s, const char *suffix);   // ignores a "?query"
void  u_copy(char *dst, size_t size, const char *src);     // always 0-terminated
char *u_trim(char *s);                                     // spaces off both ends
const char *u_basename(const char *path);                  // after the last / or '\'
void  u_join(char *out, size_t size, const char *dir, const char *name);
bool  u_is_url(const char *s);
// the next piece of *p up to one of seps (cut off with a 0), skipping empty ones; NULL at the end
char *u_token(char **p, const char *seps);
