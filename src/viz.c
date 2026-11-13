// viz.c - data for the spectrum analyser, the VU meters and the oscilloscope.
//
// The PicoVerse Audio Engine's pva_viz.c (msx-picoplay-picoamp), unchanged
// in what it computes, so the bars move as on the MSX: about 50 times a
// second the newest 512 samples go through a Hann window and an FFT (the bands
// below 1.5 kHz use a second FFT of the sound at a quarter of the rate); the
// magnitudes are grouped into 24 bands spaced evenly on a log scale from 60 Hz
// to 16 kHz, and turned into levels 0..31 (2 dB a step). A bar rises at once
// and falls a step per update; a peak stays a moment, then falls.
//
// Here the sound comes from the audio callback (what is heard now), and the
// result is read by the screen with viz_poll().
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <math.h>
#include <string.h>
#include "viz.h"
#include "sys.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N           512u
#define RING        1024u
#define DECIM       4u
#define LOW_MAX_HZ  1500.0f
#define UPDATE_MS   20u
#define PEAK_HOLD   12u
#define F_LO        60.0f
#define F_HI        16000.0f

static sys_mutex *lock;
static int16_t  ring[RING];
static uint32_t wpos;
static uint32_t fed;
static uint32_t last_rate = 44100;
static uint32_t last_ms;
static uint8_t  level[VIZ_BANDS], peak[VIZ_BANDS], hold[VIZ_BANDS];
static uint8_t  vu[2];
static int32_t  vu_peak[2];
static float    win[N], re[N], im[N];
static uint16_t band_lo[VIZ_BANDS], band_hi[VIZ_BANDS], band_hz[VIZ_BANDS];
static bool     band_dec[VIZ_BANDS];
static int16_t  dring[N];
static uint32_t dpos;
static int32_t  dacc;
static uint8_t  dcount;
static uint32_t bands_rate;
static int8_t   scope[VIZ_WAVE];

static void plan_bands(uint32_t rate)
{
    float hi = F_HI < rate * 0.48f ? F_HI : rate * 0.48f;
    float edge[VIZ_BANDS + 1];
    for (unsigned b = 0; b <= VIZ_BANDS; b++) edge[b] = F_LO * powf(hi / F_LO, (float)b / VIZ_BANDS);
    int last_full = 0, last_dec = 0;
    for (unsigned b = 0; b < VIZ_BANDS; b++)
    {
        bool dec = edge[b + 1] <= LOW_MAX_HZ;
        float bin_hz = (float)rate / (float)N / (dec ? (float)DECIM : 1.0f);
        int lo = (int)(edge[b] / bin_hz + 0.5f), h = (int)(edge[b + 1] / bin_hz + 0.5f);
        int *last = dec ? &last_dec : &last_full;
        if (lo < 1) lo = 1;
        if (lo < *last) lo = *last;
        if (h <= lo) h = lo + 1;
        if (h > (int)(N / 2) + 1) h = (int)(N / 2) + 1;
        band_lo[b] = (uint16_t)lo;
        band_hi[b] = (uint16_t)h;
        band_dec[b] = dec;
        *last = h;
        float f = ((float)lo + (float)(h - 1)) * 0.5f * bin_hz;
        band_hz[b] = (uint16_t)(f > 65535.0f ? 65535.0f : f);
    }
    bands_rate = rate;
}

void viz_init(void)
{
    lock = sys_mutex_new();
    for (unsigned i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1));
    plan_bands(44100);
}

void viz_feed(const int16_t *stereo, uint32_t frames, uint32_t rate)
{
    sys_lock(lock);
    for (uint32_t i = 0; i < frames; i++)
    {
        int32_t l = stereo[2 * i], r = stereo[2 * i + 1];
        int32_t m = (l + r) / 2;
        ring[wpos] = (int16_t)m;
        wpos = (wpos + 1u) % RING;
        dacc += m;
        if (++dcount == DECIM)
        {
            dring[dpos] = (int16_t)(dacc / (int32_t)DECIM);
            dpos = (dpos + 1u) % N;
            dacc = 0;
            dcount = 0;
        }
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > vu_peak[0]) vu_peak[0] = l;
        if (r > vu_peak[1]) vu_peak[1] = r;
    }
    fed += frames;
    if (rate) last_rate = rate;
    sys_unlock(lock);
}

static void fft(void)
{
    for (unsigned i = 1, j = 0; i < N; i++)
    {
        unsigned bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (unsigned len = 2; len <= N; len <<= 1)
    {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (unsigned i = 0; i < N; i += len)
        {
            float cr = 1.0f, ci = 0.0f;
            for (unsigned k = 0; k < len / 2; k++)
            {
                unsigned a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

static uint8_t to_level(float power_rel)
{
    if (power_rel <= 1e-12f) return 0;
    float db = 10.0f * log10f(power_rel);
    int v = (int)((db + 2.0f * (VIZ_MAX + 1)) / 2.0f);
    if (v < 0) v = 0;
    if (v > VIZ_MAX) v = VIZ_MAX;
    return (uint8_t)v;
}

static void take_scope(bool silent)
{
    uint32_t p = (wpos + RING - VIZ_WAVE * 4u) % RING;
    for (unsigned i = 0; i < VIZ_WAVE; i++)
        scope[i] = silent ? 0 : (int8_t)(ring[(p + i * 4u) % RING] / 256);
}

bool viz_poll(bool playing, viz_data *out)
{
    uint32_t now = sys_millis();
    bool changed = false;
    sys_lock(lock);
    if (now - last_ms >= UPDATE_MS && !(playing && fed < N / 2))
    {
        last_ms = now;
        changed = true;
        bool fresh = playing;
        uint8_t now_lv[VIZ_BANDS];
        if (fresh)
        {
            if (last_rate != bands_rate) plan_bands(last_rate);
            const float ref = 32768.0f * (float)N / 4.0f;
            const float ref2 = 1.0f / (ref * ref);
            for (int pass = 0; pass < 2; pass++)
            {
                bool dec = pass == 1;
                if (dec)
                    for (unsigned i = 0; i < N; i++) { re[i] = (float)dring[(dpos + i) % N] * win[i]; im[i] = 0.0f; }
                else
                {
                    uint32_t p = (wpos + RING - N) % RING;
                    for (unsigned i = 0; i < N; i++) { re[i] = (float)ring[(p + i) % RING] * win[i]; im[i] = 0.0f; }
                }
                fft();
                for (unsigned b = 0; b < VIZ_BANDS; b++)
                {
                    if (band_dec[b] != dec) continue;
                    float pmax = 0.0f;
                    for (unsigned k = band_lo[b]; k < band_hi[b] && k <= N / 2; k++)
                    {
                        float pw = re[k] * re[k] + im[k] * im[k];
                        if (pw > pmax) pmax = pw;
                    }
                    now_lv[b] = to_level(pmax * ref2);
                }
            }
            for (int c = 0; c < 2; c++)
            {
                float rel = (float)vu_peak[c] / 32768.0f;
                uint8_t v = to_level(rel * rel);
                vu[c] = v >= vu[c] ? v : (uint8_t)(vu[c] - 1);
                vu_peak[c] = 0;
            }
            fed = 0;
        }
        else
        {
            memset(now_lv, 0, sizeof now_lv);
            if (vu[0]) vu[0]--;
            if (vu[1]) vu[1]--;
        }
        for (unsigned b = 0; b < VIZ_BANDS; b++)
        {
            if (now_lv[b] >= level[b]) level[b] = now_lv[b];
            else level[b]--;
            if (level[b] >= peak[b]) { peak[b] = level[b]; hold[b] = PEAK_HOLD; }
            else if (hold[b]) hold[b]--;
            else if (peak[b]) peak[b]--;
        }
        take_scope(!fresh);
    }
    if (out)
    {
        memcpy(out->level, level, sizeof level);
        memcpy(out->peak, peak, sizeof peak);
        memcpy(out->vu, vu, sizeof vu);
        memcpy(out->scope, scope, sizeof scope);
        memcpy(out->freq, band_hz, sizeof band_hz);
    }
    sys_unlock(lock);
    return changed;
}
