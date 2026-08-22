/*
 * See roomcorr_eq.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <math.h>
#include <string.h>
#include "roomcorr_eq.h"

/* ISO 1/3-octave-ish centres spanning 20 Hz .. 20 kHz in 16 steps */
static const uint32_t s_freq[RC_EQ_BANDS] = {
    20U,   31U,   50U,   80U,   125U,  200U,  315U,   500U,
    800U,  1250U, 2000U, 3150U, 5000U, 8000U, 12500U, 20000U,
};

/*
 * Q for a constant-Q graphic EQ at this spacing. The centres step by roughly 2/3 of an
 * octave, and Q = 1.4 makes adjacent bands overlap at about -3 dB, so a row of equal
 * gains adds up to a flat shelf instead of a scalloped one.
 */
#define RC_EQ_Q (1.4f)

/* CMSIS df2T wants 5 coefficients per stage: b0 b1 b2 a1 a2, with a1/a2 negated. */
static float32_t s_coeffs[RC_EQ_BANDS * 5U];
static float32_t s_state[RC_CHANNELS][RC_EQ_BANDS * 2U];
static arm_biquad_cascade_df2T_instance_f32 s_inst[RC_CHANNELS];

static float32_t s_gainDb[RC_EQ_BANDS];
static float32_t s_preampDb;
static float32_t s_preampLin;
static bool      s_flat;

static void eq_design_band(uint32_t band)
{
    /* RBJ peaking EQ */
    const float32_t A     = powf(10.0f, s_gainDb[band] / 40.0f);
    const float32_t w0    = 2.0f * PI * (float32_t)s_freq[band] / (float32_t)RC_SAMPLE_RATE;
    const float32_t cosw0 = cosf(w0);
    const float32_t alpha = sinf(w0) / (2.0f * RC_EQ_Q);

    const float32_t b0 = 1.0f + (alpha * A);
    const float32_t b1 = -2.0f * cosw0;
    const float32_t b2 = 1.0f - (alpha * A);
    const float32_t a0 = 1.0f + (alpha / A);
    const float32_t a1 = -2.0f * cosw0;
    const float32_t a2 = 1.0f - (alpha / A);

    float32_t *c = &s_coeffs[band * 5U];
    c[0] = b0 / a0;
    c[1] = b1 / a0;
    c[2] = b2 / a0;
    c[3] = -a1 / a0; /* CMSIS convention */
    c[4] = -a2 / a0;
}

static void eq_refresh_flat(void)
{
    s_flat = true;
    for (uint32_t b = 0U; b < RC_EQ_BANDS; b++)
    {
        if ((s_gainDb[b] > 0.01f) || (s_gainDb[b] < -0.01f))
        {
            s_flat = false;
            break;
        }
    }
}

void RC_EQ_Init(void)
{
    memset(s_gainDb, 0, sizeof(s_gainDb));
    memset(s_state, 0, sizeof(s_state));
    s_preampDb  = 0.0f;
    s_preampLin = 1.0f;

    for (uint32_t b = 0U; b < RC_EQ_BANDS; b++)
    {
        eq_design_band(b);
    }
    for (uint32_t ch = 0U; ch < RC_CHANNELS; ch++)
    {
        arm_biquad_cascade_df2T_init_f32(&s_inst[ch], RC_EQ_BANDS, s_coeffs, s_state[ch]);
    }
    eq_refresh_flat();
}

uint32_t RC_EQ_BandFreq(uint32_t band)
{
    return (band < RC_EQ_BANDS) ? s_freq[band] : 0U;
}

void RC_EQ_SetBand(uint32_t band, float32_t gainDb)
{
    if (band >= RC_EQ_BANDS)
    {
        return;
    }
    if (gainDb < RC_EQ_GAIN_MIN)
    {
        gainDb = RC_EQ_GAIN_MIN;
    }
    else if (gainDb > RC_EQ_GAIN_MAX)
    {
        gainDb = RC_EQ_GAIN_MAX;
    }
    else
    {
        /* in range */
    }
    s_gainDb[band] = gainDb;
    eq_design_band(band);
    eq_refresh_flat();
}

float32_t RC_EQ_GetBand(uint32_t band)
{
    return (band < RC_EQ_BANDS) ? s_gainDb[band] : 0.0f;
}

void RC_EQ_SetPreamp(float32_t gainDb)
{
    if (gainDb < RC_EQ_PREAMP_MIN)
    {
        gainDb = RC_EQ_PREAMP_MIN;
    }
    else if (gainDb > RC_EQ_PREAMP_MAX)
    {
        gainDb = RC_EQ_PREAMP_MAX;
    }
    else
    {
        /* in range */
    }
    s_preampDb  = gainDb;
    s_preampLin = powf(10.0f, gainDb / 20.0f);
}

float32_t RC_EQ_GetPreamp(void)     { return s_preampDb; }
float32_t RC_EQ_PreampLinear(void)  { return s_preampLin; }
bool      RC_EQ_IsFlat(void)        { return s_flat; }

void RC_EQ_Process(uint32_t ch, float32_t *buf, uint32_t n)
{
    if ((ch < RC_CHANNELS) && !s_flat)
    {
        arm_biquad_cascade_df2T_f32(&s_inst[ch], buf, buf, n);
    }
}
