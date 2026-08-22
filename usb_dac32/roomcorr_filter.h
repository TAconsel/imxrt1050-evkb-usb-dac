/*
 * Runtime filter store: turns an uploaded WAV impulse response into the partitioned
 * frequency-domain filter the convolver runs, and keeps the settings.
 *
 * The built-in filter (roomcorr_data.c) stays in flash as the fallback. An upload is
 * built into one of two SDRAM slots and swapped in atomically by pointer, so the
 * convolver never reads a half-written filter.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_FILTER_H_
#define _ROOMCORR_FILTER_H_

#include <stdbool.h>
#include <stdint.h>
#include "arm_math.h"
#include "roomcorr_params.h"

/*! Largest WAV we will accept, sized for 32768 frames of 96 kHz stereo float32. */
#define RC_UPLOAD_MAX_BYTES (512U * 1024U)

typedef enum
{
    kRC_FilterOk = 0,
    kRC_FilterBadRiff,      /*!< not a RIFF/WAVE file */
    kRC_FilterBadFormat,    /*!< not PCM or IEEE float, or an unsupported bit depth */
    kRC_FilterBadRate,      /*!< sample rate is neither 48000 nor 96000 */
    kRC_FilterBadChannels,  /*!< not mono or stereo */
    kRC_FilterNoData,       /*!< no data chunk, or it is empty */
    kRC_FilterTooLong,      /*!< more taps than the convolver geometry allows */
} rc_filter_status_t;

/*! Point the convolver at the built-in filter and lay out the SDRAM slots. */
void RC_FILTER_Init(void *sdramCursor, void **sdramEnd);

/*! The partition set the convolver should use right now. Never NULL. */
const float32_t *RC_FILTER_Active(void);

/*! Staging buffer an upload should be written into, and its capacity. */
uint8_t *RC_FILTER_UploadBuffer(void);

/*!
 * @brief Convert a WAV in the staging buffer into a filter and switch to it.
 *
 * Accepts 48 kHz directly and decimates 96 kHz by two through a half-band FIR. Mono is
 * duplicated to both channels. Longer IRs than RC_TAPS are rejected rather than silently
 * truncated -- a truncated room correction is a different filter, not a worse one.
 */
rc_filter_status_t RC_FILTER_LoadWav(uint32_t bytes);

/*! Human readable form of a status code. */
const char *RC_FILTER_StatusText(rc_filter_status_t st);

/*! Description of what is loaded, e.g. "built-in" or "uploaded 16384 taps @48k". */
const char *RC_FILTER_Describe(void);
/*! Source rate of the loaded IR before any resampling. */
uint32_t RC_FILTER_SourceRate(void);
/*! Taps actually in use. */
uint32_t RC_FILTER_Taps(void);

#endif /* _ROOMCORR_FILTER_H_ */
