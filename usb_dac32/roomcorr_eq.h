/*
 * 16-band graphic EQ and preamp, applied after the room-correction convolution.
 *
 * Kept as a biquad cascade in the time domain rather than folded into the convolution
 * filter. Folding it in would be free at run time but wrong: multiplying each partition
 * spectrum by the EQ response is only equivalent to convolving with it when the EQ's
 * impulse response fits inside one partition (B = 1024 taps, 21 ms), and a 20 Hz bell
 * decays far more slowly than that. Sixteen biquads per channel costs about 15 Mflop/s
 * at 48 kHz, which is nothing next to the convolution, and it is exact.
 *
 * The EQ and preamp apply whether or not the correction is bypassed -- they are user
 * tone and level controls, not part of the calibration.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_EQ_H_
#define _ROOMCORR_EQ_H_

#include <stdbool.h>
#include <stdint.h>
#include "arm_math.h"
#include "roomcorr_params.h"

#define RC_EQ_BANDS      (16U)
#define RC_EQ_GAIN_MIN   (-12.0f)
#define RC_EQ_GAIN_MAX   (12.0f)
#define RC_EQ_PREAMP_MIN (-40.0f)
#define RC_EQ_PREAMP_MAX (12.0f)

/*! Flat, unity preamp. */
void RC_EQ_Init(void);

/*! ISO centre frequency of a band, Hz. */
uint32_t RC_EQ_BandFreq(uint32_t band);

/*! Band gain in dB, clamped to +/-12 dB. Takes effect at the next block boundary. */
void  RC_EQ_SetBand(uint32_t band, float32_t gainDb);
float32_t RC_EQ_GetBand(uint32_t band);

/*! Output gain in dB, clamped to -40..+12 dB. */
void  RC_EQ_SetPreamp(float32_t gainDb);
float32_t RC_EQ_GetPreamp(void);
/*! Linear equivalent of the preamp, for the mix stage. */
float32_t RC_EQ_PreampLinear(void);

/*! True if every band is at 0 dB, so the cascade can be skipped entirely. */
bool RC_EQ_IsFlat(void);

/*! Filter n samples of one channel in place. */
void RC_EQ_Process(uint32_t ch, float32_t *buf, uint32_t n);

#endif /* _ROOMCORR_EQ_H_ */
