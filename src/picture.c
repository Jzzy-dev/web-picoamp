// picture.c - a picture for MSX SCREEN 8 (see picture.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picture.h"
#include "tjpgd.h"
#include "sys.h"
#include "util.h"

#define OUT_W 256
#define OUT_H 192
#define ASPECT_10 11                    // an MSX dot: 1.1 times as wide as high

// ---------------------------------------------------------------- decoding
typedef struct { const uint8_t *p; size_t n, pos; uint8_t *rgb; int w, h; } jsrc_t;

static size_t j_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jsrc_t *s = jd->device;
    if (n > s->n - s->pos) n = s->n - s->pos;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

static int j_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jsrc_t *s = jd->device;
    const uint8_t *b = bitmap;
    int bw = r->right - r->left + 1;
    for (int y = (int)r->top; y <= (int)r->bottom && y < s->h; y++)
    {
        int n = bw;
        if ((int)r->left + n > s->w) n = s->w - (int)r->left;
        if (n > 0) memcpy(s->rgb + ((size_t)y * s->w + r->left) * 3, b + (size_t)(y - r->top) * bw * 3, (size_t)n * 3);
    }
    return 1;
}

uint8_t *pic_decode(const uint8_t *data, size_t len, int min_w, int min_h, int *w, int *h)
{
    if (!data || len < 8) return NULL;
    uint8_t *work = malloc(TJPGD_WORKSPACE_SIZE + 64);   // (the pictures' thread and the full screen's may both decode)
    if (!work) return NULL;
    jsrc_t s = { data, len, 0, NULL, 0, 0 };
    JDEC jd;
    if (jd_prepare(&jd, j_in, work, TJPGD_WORKSPACE_SIZE + 64, &s) == JDR_OK)
    {
        // as small as it may be while it still covers min_w x min_h (1/2, 1/4, 1/8)
        uint8_t sc = 0;
        while (sc < 3 && (int)(jd.width >> (sc + 1)) >= min_w && (int)(jd.height >> (sc + 1)) >= min_h) sc++;
        s.w = (int)((jd.width + (1u << sc) - 1) >> sc);
        s.h = (int)((jd.height + (1u << sc) - 1) >> sc);
        s.rgb = calloc((size_t)s.w * s.h, 3);
        if (s.rgb && jd_decomp(&jd, j_out, sc) == JDR_OK) { free(work); *w = s.w; *h = s.h; return s.rgb; }
        free(s.rgb);
    }
    free(work);
    // the rest (PNG, progressive JPEG ...): the platform's, made smaller to about 4 times the screen
    return plat_decode_rgb(data, len, min_w * 4, min_h * 4, w, h);
}

// ---------------------------------------------------------------- to SCREEN 8
static const uint8_t lv3[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
static const uint8_t lv2[4] = { 0, 73, 146, 255 };

void pic_to_s8(const uint8_t *rgb, int dw, int dh, uint8_t out[192][256])
{
    memset(out, 0, OUT_W * OUT_H);
    if (!rgb || dw <= 0 || dh <= 0) return;
    // fit: dw x dh square pixels into 256 x 192 MSX dots
    long w = OUT_W, h = (long)dh * OUT_W * ASPECT_10 / 10 / dw;
    if (h > OUT_H) { h = OUT_H; w = (long)dw * OUT_H * 10 / ASPECT_10 / dh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > OUT_W) w = OUT_W;
    int ox = (int)(OUT_W - w) / 2, oy = (int)(OUT_H - h) / 2;
    int err[2][OUT_W + 2][3];
    memset(err, 0, sizeof err);
    for (int y = 0; y < h; y++)
    {
        int ya = (int)((long)y * dh / h), yb = (int)((long)(y + 1) * dh / h);
        if (yb <= ya) yb = ya + 1;
        int (*e0)[3] = err[y & 1], (*e1)[3] = err[(y + 1) & 1];
        memset(e1, 0, sizeof err[0]);
        bool rev = y & 1;
        for (int k = 0; k < w; k++)
        {
            int x = rev ? (int)w - 1 - k : k, d = rev ? -1 : 1;
            int xa = (int)((long)x * dw / w), xb = (int)((long)(x + 1) * dw / w);
            if (xb <= xa) xb = xa + 1;
            long sum[3] = { 0, 0, 0 }, n = 0;
            for (int yy = ya; yy < yb; yy++)
            {
                const uint8_t *p = rgb + ((size_t)yy * dw + xa) * 3;
                for (int xx = xa; xx < xb; xx++, p += 3) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; n++; }
            }
            int v[3], q[3];
            for (int c = 0; c < 3; c++) v[c] = (int)((sum[c] + n / 2) / n) + e0[x + 1][c] * 7 / 8 / 16;
            int ir = (v[0] * 7 + 127) / 255, ig = (v[1] * 7 + 127) / 255, ib = (v[2] * 3 + 127) / 255;
            ir = ir < 0 ? 0 : ir > 7 ? 7 : ir;
            ig = ig < 0 ? 0 : ig > 7 ? 7 : ig;
            ib = ib < 0 ? 0 : ib > 3 ? 3 : ib;
            q[0] = v[0] - lv3[ir]; q[1] = v[1] - lv3[ig]; q[2] = v[2] - lv2[ib];
            out[oy + y][ox + x] = (uint8_t)(ig << 5 | ir << 2 | ib);
            for (int c = 0; c < 3; c++)
            {
                int e = q[c] * 16;
                if (x + 1 + d >= 0 && x + 1 + d < OUT_W + 2) e0[x + 1 + d][c] += e * 7 / 16;
                if (x + 1 - d >= 0 && x + 1 - d < OUT_W + 2) e1[x + 1 - d][c] += e * 3 / 16;
                e1[x + 1][c] += e * 5 / 16;
                if (x + 1 + d >= 0 && x + 1 + d < OUT_W + 2) e1[x + 1 + d][c] += e * 1 / 16;
            }
        }
    }
}

int pic_bytes_s8(const uint8_t *data, size_t len, uint8_t out[192][256])
{
    int w, h;
    uint8_t *rgb = pic_decode(data, len, OUT_W * ASPECT_10 / 10, OUT_H, &w, &h);
    if (!rgb) return PIC_E_FORMAT;
    pic_to_s8(rgb, w, h, out);
    free(rgb);
    return PIC_OK;
}

int pic_file_s8(const char *path, uint8_t out[192][256])
{
    size_t len = 0;
    char *d = sys_read_file(path, &len);
    if (!d) return PIC_E_OPEN;
    int r = pic_bytes_s8((const uint8_t *)d, len, out);
    free(d);
    return r;
}

bool pic_is_picture_name(const char *name)
{
    const char *x = strrchr(name, '.');
    if (!x) return false;
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff" };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++) if (!u_stricmp(x, ext[i])) return true;
    return false;
}
