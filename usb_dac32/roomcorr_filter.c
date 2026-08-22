/*
 * See roomcorr_filter.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include "fsl_debug_console.h"
#include "roomcorr_filter.h"
#include "roomcorr_resample.h"

extern const float32_t rc_filter[RC_CHANNELS * RC_PARTITIONS * RC_FFT_SIZE];

#define RC_FILTER_FLOATS  (RC_CHANNELS * RC_PARTITIONS * RC_FFT_SIZE)
#define RC_SRC_MAX_FRAMES (32768U) /* 341 ms at 96 kHz, the longest IR worth taking */

static const float32_t *s_active;
static float32_t       *s_slot[2];
static uint8_t          s_slotNext;

static uint8_t   *s_upload;                 /* raw WAV bytes as received */
static float32_t *s_decoded;                /* de-interleaved, [ch][frame] */
static float32_t *s_resamp;                 /* decimation output, must not alias input */
static float32_t *s_seg;                    /* one zero-padded partition, pre-FFT */

static char     s_desc[64] = "built-in";
static uint32_t s_srcRate  = RC_SAMPLE_RATE;
static uint32_t s_taps     = RC_TAPS;

static arm_rfft_fast_instance_f32 s_fft;

/* ------------------------------------------------------------------ setup ---- */

void RC_FILTER_Init(void *sdramCursor, void **sdramEnd)
{
    uint8_t *p = (uint8_t *)sdramCursor;

    s_upload = p;                            p += RC_UPLOAD_MAX_BYTES;
    s_decoded = (float32_t *)(void *)p;      p += RC_CHANNELS * RC_SRC_MAX_FRAMES * sizeof(float32_t);
    s_slot[0] = (float32_t *)(void *)p;      p += RC_FILTER_FLOATS * sizeof(float32_t);
    s_slot[1] = (float32_t *)(void *)p;      p += RC_FILTER_FLOATS * sizeof(float32_t);
    s_resamp  = (float32_t *)(void *)p;      p += (RC_SRC_MAX_FRAMES / 2U) * sizeof(float32_t);
    s_seg     = (float32_t *)(void *)p;      p += RC_FFT_SIZE * sizeof(float32_t);

    s_active   = rc_filter;
    s_slotNext = 0U;
    (void)arm_rfft_fast_init_f32(&s_fft, RC_FFT_SIZE);

    *sdramEnd = p;
}

const float32_t *RC_FILTER_Active(void)  { return s_active; }
uint8_t         *RC_FILTER_UploadBuffer(void) { return s_upload; }
const char      *RC_FILTER_Describe(void)     { return s_desc; }
uint32_t         RC_FILTER_SourceRate(void)   { return s_srcRate; }
uint32_t         RC_FILTER_Taps(void)         { return s_taps; }

const char *RC_FILTER_StatusText(rc_filter_status_t st)
{
    switch (st)
    {
        case kRC_FilterOk:          return "ok";
        case kRC_FilterBadRiff:     return "not a RIFF/WAVE file";
        case kRC_FilterBadFormat:   return "need PCM 16/24/32-bit or IEEE float32";
        case kRC_FilterBadRate:     return "need 48000 or 96000 Hz";
        case kRC_FilterBadChannels: return "need mono or stereo";
        case kRC_FilterNoData:      return "no data chunk";
        case kRC_FilterTooLong:     return "impulse response is longer than the convolver allows";
        default:                    return "unknown";
    }
}

/* ------------------------------------------------------------------- wav ---- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static float32_t sample_to_float(const uint8_t *p, uint16_t fmt, uint16_t bits)
{
    if (fmt == 3U) /* IEEE float32 */
    {
        float32_t v;
        (void)memcpy(&v, p, sizeof(v));
        return v;
    }
    if (bits == 16U)
    {
        return (float32_t)(int16_t)rd16(p) / 32768.0f;
    }
    if (bits == 24U)
    {
        int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
        return (float32_t)v / 2147483648.0f;
    }
    return (float32_t)(int32_t)rd32(p) / 2147483648.0f;
}

/* -------------------------------------------------------------- resample ---- */

/*
 * Decimate by two: low-pass at 24 kHz, then keep every other sample. Output goes to a
 * separate buffer -- doing it in place would corrupt the input, because output i reads
 * as far back as 2i-63, which for small i is still ahead of the write cursor.
 *
 * The x2 is not a mistake: decimation halves sum(h), and for a filter it is the
 * frequency response that must be preserved, not the sample amplitude. make_filter.py
 * applies exactly the same dn/up correction on the host.
 */
static uint32_t decimate_half(const float32_t *x, uint32_t n, float32_t *y)
{
    const uint32_t out = n / 2U;

    for (uint32_t i = 0U; i < out; i++)
    {
        const int32_t centre = (int32_t)(i * 2U);
        float32_t acc = 0.0f;

        for (uint32_t k = 0U; k < RC_DECIM_TAPS; k++)
        {
            const int32_t idx = centre + (int32_t)k - (int32_t)RC_DECIM_DELAY;
            if ((idx >= 0) && (idx < (int32_t)n))
            {
                acc += rc_decim_fir[k] * x[idx];
            }
        }
        y[i] = acc * 2.0f;
    }
    return out;
}

/* ---------------------------------------------------------------- public ---- */

rc_filter_status_t RC_FILTER_LoadWav(uint32_t bytes)
{
    const uint8_t *d = s_upload;
    uint16_t fmt = 0U, channels = 0U, bits = 0U;
    uint32_t rate = 0U, dataOff = 0U, dataLen = 0U;
    uint32_t i;

    if ((bytes < 44U) || (memcmp(d, "RIFF", 4) != 0) || (memcmp(&d[8], "WAVE", 4) != 0))
    {
        return kRC_FilterBadRiff;
    }

    for (i = 12U; (i + 8U) <= bytes;)
    {
        const uint32_t sz = rd32(&d[i + 4U]);

        if (memcmp(&d[i], "fmt ", 4) == 0)
        {
            fmt      = rd16(&d[i + 8U]);
            channels = rd16(&d[i + 10U]);
            rate     = rd32(&d[i + 12U]);
            bits     = rd16(&d[i + 22U]);
            if ((fmt == 0xFFFEU) && (sz >= 40U)) /* WAVE_FORMAT_EXTENSIBLE */
            {
                fmt = rd16(&d[i + 32U]); /* first two bytes of the sub-format GUID */
            }
        }
        else if (memcmp(&d[i], "data", 4) == 0)
        {
            dataOff = i + 8U;
            dataLen = (sz <= (bytes - dataOff)) ? sz : (bytes - dataOff);
            break;
        }
        else
        {
            /* skip */
        }
        i += 8U + sz + (sz & 1U);
    }

    if ((fmt != 1U) && (fmt != 3U))                       { return kRC_FilterBadFormat; }
    if ((bits != 16U) && (bits != 24U) && (bits != 32U))  { return kRC_FilterBadFormat; }
    if ((fmt == 3U) && (bits != 32U))                     { return kRC_FilterBadFormat; }
    if ((channels != 1U) && (channels != 2U))             { return kRC_FilterBadChannels; }
    if ((rate != 48000U) && (rate != 96000U))             { return kRC_FilterBadRate; }
    if ((dataOff == 0U) || (dataLen == 0U))               { return kRC_FilterNoData; }

    const uint32_t bytesPerSample = bits / 8U;
    uint32_t frames = dataLen / (bytesPerSample * channels);
    if (frames > RC_SRC_MAX_FRAMES)
    {
        frames = RC_SRC_MAX_FRAMES;
    }

    /* de-interleave into [ch][frame] */
    for (uint32_t f = 0U; f < frames; f++)
    {
        for (uint32_t c = 0U; c < RC_CHANNELS; c++)
        {
            const uint32_t src = (channels == 1U) ? 0U : c;
            const uint8_t *p   = &d[dataOff + (((f * channels) + src) * bytesPerSample)];
            s_decoded[(c * RC_SRC_MAX_FRAMES) + f] = sample_to_float(p, fmt, bits);
        }
    }

    if (rate == 96000U)
    {
        for (uint32_t c = 0U; c < RC_CHANNELS; c++)
        {
            float32_t *chan = &s_decoded[c * RC_SRC_MAX_FRAMES];
            const uint32_t out = decimate_half(chan, frames, s_resamp);
            (void)memcpy(chan, s_resamp, out * sizeof(float32_t));
        }
        frames /= 2U;
    }

    if (frames > RC_TAPS)
    {
        return kRC_FilterTooLong;
    }

    /* partition and transform into the spare slot */
    float32_t *slot = s_slot[s_slotNext];

    for (uint32_t c = 0U; c < RC_CHANNELS; c++)
    {
        for (uint32_t p = 0U; p < RC_PARTITIONS; p++)
        {
            memset(s_seg, 0, RC_FFT_SIZE * sizeof(float32_t));
            for (uint32_t k = 0U; k < RC_BLOCK; k++)
            {
                const uint32_t t = (p * RC_BLOCK) + k;
                s_seg[k] = (t < frames) ? s_decoded[(c * RC_SRC_MAX_FRAMES) + t] : 0.0f;
            }
            arm_rfft_fast_f32(&s_fft, s_seg, &slot[(((c * RC_PARTITIONS) + p) * RC_FFT_SIZE)], 0);
        }
    }

    /* single pointer store: the convolver snapshots it once per block */
    s_active   = slot;
    s_slotNext ^= 1U;
    s_srcRate  = rate;
    s_taps     = frames;

    (void)snprintf(s_desc, sizeof(s_desc), "uploaded, %u taps @%u Hz src",
                   (unsigned)frames, (unsigned)rate);
    PRINTF("filter: loaded %s\r\n", s_desc);
    return kRC_FilterOk;
}
