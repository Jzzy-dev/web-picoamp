// cover.c - the cover view's picture on a PC: found as the engine finds it,
// decoded (TJpgDec as the engine; else the platform's decoder), converted
// with PICOAMP's palette by the engine's pva_cover.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cover.h"
#include "pva_cover.h"
#include "tjpgd.h"
#include "gfx.h"
#include "sys.h"
#include "util.h"

static pva_cover_t *cv;
static uint8_t out[PVA_COVER_SIZE];
static int state = COVER_NONE;
static unsigned gen;
static char key[2200];                  // where the picture came from (a folder's image: once for its tracks)

static void set_state(int s) { state = s; gen++; }
int cover_state(void) { return state; }
unsigned cover_gen(void) { return gen; }
const uint8_t *cover_data(void) { return state == COVER_READY ? out : NULL; }

// PICOAMP's palette in the MSX's form (0RRR0BBB, 00000GGG)
static void msx_palette(uint8_t pal[32])
{
    for (int i = 0; i < 16; i++)
    {
        uint32_t c = gfx_pal_base[i];
        int r = (int)((c >> 16 & 255) * 7 + 127) / 255, g = (int)((c >> 8 & 255) * 7 + 127) / 255, b = (int)((c & 255) * 7 + 127) / 255;
        pal[i * 2] = (uint8_t)(r << 4 | b);
        pal[i * 2 + 1] = (uint8_t)g;
    }
}

static void convert(void)
{
    uint8_t pal[32];
    msx_palette(pal);
    pva_cover_convert(cv, pal, COVER_FREE, out);     // (a PC does it at once: about 10 ms)
    set_state(COVER_READY);
}

void cover_from_rgb(int w, int h, const uint8_t *rgb, int stride)
{
    if (!cv) cv = malloc(sizeof *cv);
    if (!cv || w <= 0 || h <= 0) { set_state(COVER_NONE); return; }
    pva_cover_begin(cv, w, h);
    for (int y = 0; y < h; y++) pva_cover_block(cv, 0, y, w, 1, rgb + (size_t)y * stride);
    pva_cover_end(cv);
    convert();
}

// ---------------------------------------------------------------- JPEG (TJpgDec, from memory)
typedef struct { const uint8_t *p; size_t n, pos; } src_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    src_t *s = jd->device;
    if (n > s->n - s->pos) n = s->n - s->pos;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    (void)jd;
    pva_cover_block(cv, r->left, r->top, r->right - r->left + 1, r->bottom - r->top + 1, bitmap);
    return 1;
}

static bool decode_jpeg(const uint8_t *data, size_t len)
{
    static uint8_t work[TJPGD_WORKSPACE_SIZE + 64];
    src_t s = { data, len, 0 };
    JDEC jd;
    if (jd_prepare(&jd, jpg_in, work, sizeof work, &s) != JDR_OK) return false;
    uint8_t sc = 0;
    unsigned m = jd.width < jd.height ? jd.width : jd.height;
    while (sc < 3 && (m >> (sc + 1)) >= PVA_COVER_PX) sc++;
    pva_cover_begin(cv, jd.width >> sc, jd.height >> sc);
    if (jd_decomp(&jd, jpg_out, sc) != JDR_OK) return false;
    pva_cover_end(cv);
    return true;
}

void cover_from_bytes(const uint8_t *data, size_t len)
{
    if (!cv) cv = malloc(sizeof *cv);
    if (!cv || !data || len < 8) { set_state(COVER_NONE); return; }
    if (decode_jpeg(data, len)) { convert(); return; }
    if (plat_decode_image(data, len)) { if (state != COVER_READY) set_state(COVER_MAKING); return; }
    set_state(COVER_NONE);
}

// ---------------------------------------------------------------- the picture of a tag
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t ss32(const uint8_t *b) { return (uint32_t)(b[0] & 0x7F) << 21 | (uint32_t)(b[1] & 0x7F) << 14 | (uint32_t)(b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

bool cover_find_in_tag(const uint8_t *b, size_t n, size_t *off, size_t *len)
{
    bool found = false, front = false;
    if (n >= 10 && !memcmp(b, "ID3", 3) && b[3] >= 3 && b[3] <= 4)
    {
        // ID3v2.3 / 2.4: APIC frames (the front cover first)
        int ver = b[3];
        size_t end = 10 + ss32(b + 6), p = 10;
        if (end > n) end = n;
        if (b[5] & 0x40) p += ver == 4 ? ss32(b + 10) : be32(b + 10) + 4;
        while (p + 10 <= end && b[p])
        {
            uint32_t fs = ver == 4 ? ss32(b + p + 4) : be32(b + p + 4);
            size_t body = p + 10;
            if (fs == 0 || body + fs > end) break;
            if (!memcmp(b + p, "APIC", 4) && !(b[p + 9] & (ver == 4 ? 0x0B : 0xC0)) && (!found || !front))
            {
                const uint8_t *f = b + body;
                size_t i = 1;
                while (i < fs && f[i]) i++;                    // MIME type
                if (++i < fs)
                {
                    uint8_t type = f[i++];
                    if (f[0] == 1 || f[0] == 2) { while (i + 1 < fs && (f[i] || f[i + 1])) i += 2; i += 2; }
                    else { while (i < fs && f[i]) i++; i++; }
                    if (i < fs) { *off = body + i; *len = fs - i; found = true; front = type == 3; }
                }
            }
            p = body + fs;
        }
    }
    else if (n >= 8 && !memcmp(b, "fLaC", 4))
    {
        // FLAC: a PICTURE block (type 6)
        size_t p = 4;
        while (p + 4 <= n)
        {
            uint8_t t = b[p];
            size_t bl = (size_t)b[p + 1] << 16 | (size_t)b[p + 2] << 8 | b[p + 3];
            size_t q = p + 4;
            if ((t & 0x7F) == 6 && q + 32 <= n)
            {
                uint32_t type = be32(b + q), ml = be32(b + q + 4);
                size_t d = q + 8 + ml;
                if (d + 4 <= n) { uint32_t dl = be32(b + d); d += 4 + dl + 16; }
                if (d + 4 <= n)
                {
                    uint32_t len32 = be32(b + d);
                    if (d + 4 + len32 <= n && (!found || !front)) { *off = d + 4; *len = len32; found = true; front = type == 3; }
                }
            }
            if (t & 0x80) break;
            p = q + bl;
        }
    }
    return found;
}

// ---------------------------------------------------------------- a file starts
#ifndef __wasm__
static bool folder_image(const char *track, char *img, size_t size)
{
    static const char *const names[] = { "cover", "folder", "front", "album", "albumart", "AlbumArtSmall" };
    static const char *const ext[] = { ".jpg", ".jpeg", ".png" };
    char dir[2048];
    u_copy(dir, sizeof dir, track);
    char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
    if (bs > sl) sl = bs;
    if (!sl) return false;
    *sl = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        for (unsigned e = 0; e < 3; e++)
        {
            snprintf(img, size, "%s/%s%s", dir, names[i], ext[e]);
            if (sys_file_exists(img)) return true;
        }
    return false;
}
#endif

void cover_track(const char *path)
{
#ifdef __wasm__
    (void)path;             // (the web: the page gives the tag's bytes and the folder's image)
#else
    // the tag's picture
    FILE *f = sys_fopen(path, "rb");
    if (f)
    {
        uint8_t *b = malloc(4u << 20);
        size_t n = b ? fread(b, 1, 4u << 20, f) : 0;
        fclose(f);
        size_t off, len;
        if (b && cover_find_in_tag(b, n, &off, &len))
        {
            key[0] = 0;
            cover_from_bytes(b + off, len);
            free(b);
            if (state != COVER_NONE) return;
        }
        else free(b);
    }
    // an image in its folder (made once for its tracks)
    char img[2300];
    if (folder_image(path, img, sizeof img))
    {
        if (!strcmp(img, key) && state == COVER_READY) return;
        u_copy(key, sizeof key, img);
        size_t len = 0;
        char *d = sys_read_file(img, &len);
        cover_from_bytes((const uint8_t *)d, len);
        free(d);
        return;
    }
    key[0] = 0;
    set_state(COVER_NONE);
#endif
}

void cover_none(void)
{
    key[0] = 0;
    if (state != COVER_NONE) set_state(COVER_NONE);
}
