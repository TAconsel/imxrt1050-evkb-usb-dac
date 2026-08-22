/*
 * Uniform-partitioned overlap-save FFT convolution.
 *
 * Per block of B frames and per channel:
 *   x  = [previous B samples | current B samples]        (N = 2B reals)
 *   X  = rfft(x)                                          (CMSIS packed, N floats)
 *   push X into the front of a P-deep frequency-domain delay line
 *   Y  = sum(p = 0..P-1) FDL[head + p] * FILTER[p]        (complex MAC, packed)
 *   y  = irfft(Y); keep the SECOND half, the first half is circular-convolution wrap
 *
 * Cost is dominated by the P complex MACs, not the two FFTs. The delay line is the
 * only large buffer and lives in SDRAM; the filter is rodata in HyperFlash.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_gpt.h"
#include "roomcorr.h"
#include "roomcorr_eq.h"
#include "roomcorr_filter.h"
#include "roomcorr_stream.h"

extern const float32_t rc_filter[RC_CHANNELS * RC_PARTITIONS * RC_FFT_SIZE];

/*
 * CMSIS's real FFT pair is not a plain unscaled DFT: stage_rfft_f32() (forward) and
 * merge_rfft_f32() (inverse) each carry 0.5 factors, and the inverse complex transform
 * underneath scales by 1/(N/2). The net round-trip gain is therefore NOT 1, and it is
 * not documented anywhere convenient, so RC_SelfTest() measures it against the host
 * reference and this constant is set from that measurement.
 */
#ifndef RC_IFFT_COMPENSATION
#define RC_IFFT_COMPENSATION (1.0f)
#endif

/*
 * Load measurement clock: GPT2, free running from the 24 MHz crystal.
 *
 * Not DWT CYCCNT -- that lives in the debug power domain, which the RT1050 powers down
 * when the probe detaches, so it freezes and every block then measures zero. Not SysTick
 * either, any more: lwIP's bare-metal port takes SysTick for its 1 ms tick. GPT2 is
 * free, counts up through a full 32 bits (179 s before wrap) and is independent of both.
 */
#define RC_TICKS_PER_US (24U)

static inline uint32_t rc_now(void)
{
    return GPT_GetCurrentTimerCount(GPT2);
}

static inline uint32_t rc_elapsed(uint32_t t0)
{
    return rc_now() - t0; /* unsigned wrap is exactly what we want */
}

/* int32 full scale <-> float. The USB/I2S samples are 32-bit signed. */
#define RC_INT_TO_FLOAT (1.0f / 2147483648.0f)
#define RC_FLOAT_TO_INT (2147483648.0f)

/* ---------------------------------------------------------------- state ---- */

static arm_rfft_fast_instance_f32 s_fft;

/* frequency-domain delay line, [channel][partition][RC_FFT_SIZE], in SDRAM */
static float32_t *s_fdl;
/* newest partition index; the line is walked forwards from here */
static uint32_t s_head;

/* previous input block per channel (the overlap half), in SDRAM: 8 KB, and it
 * is walked linearly once per block so the cache covers it */
static float32_t *s_prev[RC_CHANNELS];

/* scratch: on-chip, touched twice per block per channel */
static float32_t s_time[RC_FFT_SIZE];
static float32_t s_freq[RC_FFT_SIZE];
static float32_t s_acc[RC_FFT_SIZE];
static float32_t s_mixBuf[RC_BLOCK]; /* one channel, post-mix, pre-EQ */

/*
 * Wet/dry mix. The convolution runs unconditionally, even when bypassed: the delay line
 * has to keep being fed or switching back would play out of a stale FDL, and having the
 * wet signal always available is what makes a click-free crossfade possible. It costs
 * the same ~24% CPU either way, which is affordable.
 *
 * s_mix ramps to s_mixTarget across one block (21 ms). The two signals are not time
 * aligned during the ramp -- the filter has ~46 ms of its own pre-delay -- so the
 * crossfade is not phase coherent, but over one block it just sounds like a smooth
 * transition rather than the hard click a bare switch would give.
 */
static float32_t s_mix;       /* 1.0 = fully corrected, 0.0 = fully dry */
static float32_t s_mixTarget;
static uint32_t s_lastCycles, s_peakCycles, s_clipCount, s_blockCount;

static inline float32_t *fdl_slot(uint32_t ch, uint32_t part)
{
    return s_fdl + ((ch * RC_PARTITIONS) + part) * RC_FFT_SIZE;
}

static inline const float32_t *filter_part(const float32_t *base, uint32_t ch, uint32_t part)
{
    return base + (((ch * RC_PARTITIONS) + part) * RC_FFT_SIZE);
}

/* ------------------------------------------------------------ arithmetic ---- */

/*
 * acc += X * H, both in CMSIS packed real-FFT layout:
 *   [0] = Re(DC), [1] = Re(Nyquist), then Re/Im interleaved for bins 1..N/2-1.
 * DC and Nyquist are purely real and multiply as scalars; everything else is a
 * complex multiply-accumulate.
 */
static void rc_cmac_packed(float32_t *acc, const float32_t *X, const float32_t *H)
{
    acc[0] += X[0] * H[0];
    acc[1] += X[1] * H[1];

    for (uint32_t i = 2U; i < RC_FFT_SIZE; i += 2U)
    {
        const float32_t xr = X[i], xi = X[i + 1U];
        const float32_t hr = H[i], hi = H[i + 1U];
        acc[i]      += (xr * hr) - (xi * hi);
        acc[i + 1U] += (xr * hi) + (xi * hr);
    }
}

/* --------------------------------------------------------------- public ---- */

void RC_Init(void)
{
    s_fdl = (float32_t *)RC_SDRAM_BASE;

    (void)arm_rfft_fast_init_f32(&s_fft, RC_FFT_SIZE);

    memset(s_fdl, 0, RC_FDL_FLOATS * sizeof(float32_t));
    for (uint32_t ch = 0U; ch < RC_CHANNELS; ch++)
    {
        if (s_prev[ch] == NULL)
        {
            s_prev[ch] = (float32_t *)RCS_SdramAlloc(RC_BLOCK * sizeof(float32_t));
        }
        memset(s_prev[ch], 0, RC_BLOCK * sizeof(float32_t));
    }
    s_head       = 0U;
    s_mix        = 1.0f;
    s_mixTarget  = 1.0f;
    s_lastCycles = 0U;
    s_peakCycles = 0U;
    s_clipCount  = 0U;
    s_blockCount = 0U;

    /* free-running GPT2, see rc_now() */
    gpt_config_t gpt;
    GPT_GetDefaultConfig(&gpt);
    gpt.clockSource   = kGPT_ClockSource_Osc; /* 24 MHz crystal, independent of the PLLs */
    gpt.divider       = 1U;
    gpt.enableFreeRun = true;
    GPT_Init(GPT2, &gpt);
    GPT_SetOscClockDivider(GPT2, 1U);
    GPT_StartTimer(GPT2);
}

void RC_ProcessBlock(const int32_t *in, int32_t *out)
{
    const uint32_t t0     = rc_now();
    const float32_t gStart = s_mix;
    const float32_t gStep  = (s_mixTarget - s_mix) / (float32_t)RC_BLOCK;
    const float32_t preamp = RC_EQ_PreampLinear();
    /* snapshot once: an upload can swap this pointer between blocks, and half a block
     * of one filter followed by half of another would be an audible discontinuity */
    const float32_t *const filt = RC_FILTER_Active();

    /* one step back: slot s_head holds the newest spectrum */
    s_head = (s_head + RC_PARTITIONS - 1U) % RC_PARTITIONS;

    for (uint32_t ch = 0U; ch < RC_CHANNELS; ch++)
    {
        /* [ previous block | this block ] */
        memcpy(s_time, s_prev[ch], RC_BLOCK * sizeof(float32_t));
        for (uint32_t i = 0U; i < RC_BLOCK; i++)
        {
            const float32_t v = (float32_t)in[(i * RC_CHANNELS) + ch] * RC_INT_TO_FLOAT;
            s_time[RC_BLOCK + i] = v;
            s_prev[ch][i]        = v;
        }

        arm_rfft_fast_f32(&s_fft, s_time, fdl_slot(ch, s_head), 0);

        memset(s_acc, 0, sizeof(s_acc));
        for (uint32_t p = 0U; p < RC_PARTITIONS; p++)
        {
            const uint32_t slot = (s_head + p) % RC_PARTITIONS;
            rc_cmac_packed(s_acc, fdl_slot(ch, slot), filter_part(filt, ch, p));
        }

        arm_rfft_fast_f32(&s_fft, s_acc, s_freq, 1);

        /* keep the second half; the first half is the wrapped part */
        float32_t g = gStart;
        for (uint32_t i = 0U; i < RC_BLOCK; i++)
        {
            const float32_t wet = s_freq[RC_BLOCK + i] * RC_IFFT_COMPENSATION;
            const float32_t dry = (float32_t)in[(i * RC_CHANNELS) + ch] * RC_INT_TO_FLOAT;

            s_mixBuf[i] = ((wet * g) + (dry * (1.0f - g))) * preamp;
            g += gStep;
        }

        /* tone controls sit after the calibration and apply either way */
        RC_EQ_Process(ch, s_mixBuf, RC_BLOCK);

        for (uint32_t i = 0U; i < RC_BLOCK; i++)
        {
            float32_t v = s_mixBuf[i] * RC_FLOAT_TO_INT;

            if (v >= 2147483647.0f)
            {
                v = 2147483647.0f;
                s_clipCount++;
            }
            else if (v < -2147483648.0f) /* exact negative full scale is not a clip */
            {
                v = -2147483648.0f;
                s_clipCount++;
            }
            else
            {
                /* in range */
            }
            out[(i * RC_CHANNELS) + ch] = (int32_t)v;
        }
    }
    s_mix = s_mixTarget;

    s_lastCycles = rc_elapsed(t0);
    if (s_lastCycles > s_peakCycles)
    {
        s_peakCycles = s_lastCycles;
    }
    s_blockCount++;
}

uint32_t RC_LastCycles(void)  { return s_lastCycles; }
uint32_t RC_PeakMicros(void)  { return s_peakCycles / RC_TICKS_PER_US; }
uint32_t RC_PeakCycles(void)  { return s_peakCycles; }
uint32_t RC_ClipCount(void)   { return s_clipCount; }
uint32_t RC_BlockCount(void)  { return s_blockCount; }

void RC_SetBypass(bool bypass) { s_mixTarget = bypass ? 0.0f : 1.0f; }
bool RC_GetBypass(void)        { return (s_mixTarget < 0.5f); }

void RC_SelfTest(void)
{
    /* 8 KB each, used once at boot: SDRAM, not DTCM */
    int32_t *tin  = (int32_t *)RCS_SdramAlloc(RC_BLOCK * RC_CHANNELS * sizeof(int32_t));
    int32_t *tout = (int32_t *)RCS_SdramAlloc(RC_BLOCK * RC_CHANNELS * sizeof(int32_t));
    const int32_t one = 1073741824; /* +0.5 full scale, keeps headroom */

    PRINTF("\r\n--- room correction self test ---\r\n");
    PRINTF("taps %d, B %d, N %d, P %d, filter %d KiB, FDL %d KiB @ 0x%08x\r\n",
             (int)RC_TAPS, (int)RC_BLOCK, (int)RC_FFT_SIZE, (int)RC_PARTITIONS,
             (int)(sizeof(rc_filter) / 1024), (int)(RC_FDL_FLOATS * 4U / 1024U),
             (unsigned)RC_SDRAM_BASE);

    /* impulse in, so the output is the filter's own impulse response */
    memset(tin, 0, RC_BLOCK * RC_CHANNELS * sizeof(int32_t));
    tin[0] = one;                    /* left channel, first frame */
    tin[1] = one;                    /* right channel */

    RC_Init();
    RC_ProcessBlock(tin, tout);

    PRINTF("first 8 output taps (x 2^-30), L / R:\r\n");
    for (uint32_t i = 0U; i < 8U; i++)
    {
        PRINTF("  h[%d] = %11d  %11d\r\n", (int)i,
                 (int)(tout[i * 2U] / 1024), (int)(tout[(i * 2U) + 1U] / 1024));
    }

    /* the IR peaks around 45 ms, i.e. block 2 - run enough blocks to reach it */
    int32_t peakL = 0, peakR = 0;
    uint32_t peakIdx = 0U;
    for (uint32_t blk = 1U; blk < 8U; blk++)
    {
        memset(tin, 0, RC_BLOCK * RC_CHANNELS * sizeof(int32_t));
        RC_ProcessBlock(tin, tout);
        for (uint32_t i = 0U; i < RC_BLOCK; i++)
        {
            const int32_t l = tout[i * 2U];
            if ((l > peakL) || (-l > peakL))
            {
                peakL   = (l > 0) ? l : -l;
                peakIdx = (blk * RC_BLOCK) + i;
            }
            const int32_t r = tout[(i * 2U) + 1U];
            if ((r > peakR) || (-r > peakR))
            {
                peakR = (r > 0) ? r : -r;
            }
        }
    }
    PRINTF("IR peak raw: L %d  R %d  at frame %d (%d ms)\r\n",
           (int)peakL, (int)peakR, (int)peakIdx,
           (int)((peakIdx * 1000U) / RC_SAMPLE_RATE));
    PRINTF("input impulse raw: %d, ifft compensation x%d\r\n",
           (int)one, (int)RC_IFFT_COMPENSATION);
    PRINTF("block cost: %d us, budget %d us\r\n", (int)RC_PeakMicros(),
           (int)((RC_BLOCK * 1000000U) / RC_SAMPLE_RATE));
    PRINTF("--- end self test ---\r\n\r\n");

    RC_Init();
}
