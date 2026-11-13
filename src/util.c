// util.c - small string helpers
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "util.h"

int u_strnicmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

int u_stricmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

bool u_ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    const char *q = strchr(s, '?');
    if (q) a = (size_t)(q - s);
    return a >= b && !u_strnicmp(s + a - b, suf, b);
}

void u_copy(char *dst, size_t size, const char *src)
{
    if (!size) return;
    snprintf(dst, size, "%s", src ? src : "");
}

char *u_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    return s;
}

const char *u_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

void u_join(char *out, size_t size, const char *dir, const char *name)
{
    size_t l = strlen(dir);
#ifdef _WIN32
    const char *sep = (l && (dir[l - 1] == '\\' || dir[l - 1] == '/')) ? "" : "\\";
#else
    const char *sep = (l && dir[l - 1] == '/') ? "" : "/";
#endif
    snprintf(out, size, "%s%s%s", dir, sep, name);
}

bool u_is_url(const char *s)
{
    return !u_strnicmp(s, "http://", 7) || !u_strnicmp(s, "https://", 8);
}

char *u_token(char **p, const char *seps)
{
    char *s = *p;
    if (!s) return NULL;
    s += strspn(s, seps);
    if (!*s) { *p = s; return NULL; }
    char *e = s + strcspn(s, seps);
    if (*e) *e++ = 0;
    *p = e;
    return s;
}
