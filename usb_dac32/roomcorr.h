/*
 * Room correction for the MIMXRT1050-EVKB USB DAC.
 *
 * Uniform-partitioned overlap-save FFT convolution, stereo, float32, running on the
 * Cortex-M7 FPU via CMSIS-DSP. The filter geometry lives in the generated
 * roomcorr_params.h; the coefficients live in roomcorr_data.c as rodata (HyperFlash).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_H_
#define _ROOMCORR_H_

#include <stdbool.h>
#include <stdint.h>
#include "arm_math.h"
#include "roomcorr_params.h"

/*
 * The frequency-domain delay line is RC_CHANNELS * RC_PARTITIONS * RC_FFT_SIZE floats
 * (256 KiB for a 341 ms filter), which does not fit on-chip alongside everything else,
 * so it lives in the 32 MB SEMC SDRAM at 0x80000000. The SDRAM is brought up by the
 * boot header's DCD and marked cacheable write-back by BOARD_EnableSdramCaching().
 */
#define RC_SDRAM_BASE  (0x80000000U)
#define RC_FDL_FLOATS  (RC_CHANNELS * RC_PARTITIONS * RC_FFT_SIZE)

/*! Set up the FFT instance, clear the delay line. Call after SDRAM is usable. */
void RC_Init(void);

/*!
 * @brief Convolve one block.
 * @param in   RC_BLOCK frames, stereo interleaved, 32-bit signed (I2S alignment)
 * @param out  RC_BLOCK frames, stereo interleaved, 32-bit signed
 * in and out may not overlap.
 */
void RC_ProcessBlock(const int32_t *in, int32_t *out);

/*! Cycles taken by the last RC_ProcessBlock() call (DWT CYCCNT). */
uint32_t RC_LastCycles(void);
/*! Peak cycles seen so far. */
uint32_t RC_PeakCycles(void);
/*! Samples clamped at the output since boot. */
uint32_t RC_ClipCount(void);
/*! Blocks processed since boot. */
uint32_t RC_BlockCount(void);

/*! When bypassed the block is copied through untouched. */
void RC_SetBypass(bool bypass);
bool RC_GetBypass(void);

/*!
 * @brief Push an impulse through the convolver and report the result.
 *
 * Prints the round-trip gain and the first taps of the recovered impulse response so
 * the FFT scaling convention and the filter data can be checked against the host
 * reference (tools/verify_filter.py) rather than assumed.
 */
void RC_SelfTest(void);

#endif /* _ROOMCORR_H_ */
