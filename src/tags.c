// tags.c - title, artist and album from ID3v2 (2.2, 2.3, 2.4) or ID3v1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tags.h"
#include "sys.h"
#include "util.h"

// one text frame's bytes (encoding byte first) -> UTF-8
static void text_to_utf8(const unsigned char *p, size_t n, char *out, size_t outlen)
{
    size_t o = 0;
    if (!n || !outlen) { if (outlen) out[0] = 0; return; }
    int enc = p[0];
    p++; n--;
#define PUT(cp) do { uint32_t c_ = (cp); \
        if (c_ < 0x80) { if (o + 1 < outlen) out[o++] = (char)c_; } \
        else if (c_ < 0x800) { if (o + 2 < outlen) { out[o++] = (char)(0xC0 | (c_ >> 6)); out[o++] = (char)(0x80 | (c_ & 0x3F)); } } \
        else if (c_ < 0x10000) { if (o + 3 < outlen) { out[o++] = (char)(0xE0 | (c_ >> 12)); out[o++] = (char)(0x80 | ((c_ >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c_ & 0x3F)); } } \
        else { if (o + 4 < outlen) { out[o++] = (char)(0xF0 | (c_ >> 18)); out[o++] = (char)(0x80 | ((c_ >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c_ >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c_ & 0x3F)); } } \
    } while (0)
    if (enc == 1 || enc == 2)
    {
        bool be = enc == 2;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
        for (size_t i = 0; i + 1 < n; i += 2)
        {
            uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i + 1] << 8 | p[i]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n)
            {
                uint32_t d = be ? (uint32_t)(p[i + 2] << 8 | p[i + 3]) : (uint32_t)(p[i + 3] << 8 | p[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
            PUT(c);
        }
    }
    else if (enc == 3)
    {
        for (size_t i = 0; i < n && p[i]; i++) if (o + 1 < outlen) out[o++] = (char)p[i];
    }
    else
        for (size_t i = 0; i < n && p[i]; i++) PUT(p[i]);
#undef PUT
    out[o] = 0;
}

static void trim_in_place(char *s)
{
    char *t = u_trim(s);
    if (t != s) memmove(s, t, strlen(t) + 1);
}

static uint32_t syncsafe(const unsigned char *b) { return (uint32_t)(b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

bool tags_read(const char *path, track_tags *t)
{
    memset(t, 0, sizeof *t);
    FILE *f = sys_fopen(path, "rb");
    if (!f) return false;
    unsigned char h[10];
    bool any = false;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4)
    {
        int ver = h[3];
        uint32_t size = syncsafe(h + 6);
        if (size > 4 * 1024 * 1024) size = 4 * 1024 * 1024;
        unsigned char *b = malloc(size);
        size_t got = fread(b, 1, size, f);
        size_t p = 0;
        if (h[5] & 0x40 && ver >= 3 && got >= 4)          // extended header
            p += ver == 4 ? syncsafe(b) : (uint32_t)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) + 4;
        size_t fh = ver == 2 ? 6 : 10;
        while (p + fh <= got)
        {
            char id[5] = { 0 };
            uint32_t fs;
            if (ver == 2) { memcpy(id, b + p, 3); fs = (uint32_t)(b[p + 3] << 16 | b[p + 4] << 8 | b[p + 5]); }
            else
            {
                memcpy(id, b + p, 4);
                fs = ver == 4 ? syncsafe(b + p + 4) : (uint32_t)(b[p + 4] << 24 | b[p + 5] << 16 | b[p + 6] << 8 | b[p + 7]);
            }
            if (!id[0]) break;
            p += fh;
            if (fs > got - p) break;
            char *dst = NULL;
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = t->title;
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = t->artist;
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = t->album;
            if (dst && !dst[0]) { text_to_utf8(b + p, fs, dst, sizeof t->title); any = true; }
            p += fs;
        }
        free(b);
    }
    if (!t->title[0] || !t->artist[0])
    {
        // ID3v1 at the end
        unsigned char v1[128];
        if (!fseek(f, -128, SEEK_END) && fread(v1, 1, 128, f) == 128 && !memcmp(v1, "TAG", 3))
        {
            char tmp[31];
            unsigned char raw[32];
            raw[0] = 0;
            struct { char *dst; int off; } fl[] = { { t->title, 3 }, { t->artist, 33 }, { t->album, 63 } };
            for (int i = 0; i < 3; i++)
                if (!fl[i].dst[0])
                {
                    memcpy(raw + 1, v1 + fl[i].off, 30);
                    raw[31] = 0;
                    text_to_utf8(raw, 31, tmp, sizeof tmp);
                    u_copy(fl[i].dst, sizeof t->title, u_trim(tmp));
                    any = true;
                }
        }
    }
    fclose(f);
    trim_in_place(t->title);
    trim_in_place(t->artist);
    trim_in_place(t->album);
    return any;
}
