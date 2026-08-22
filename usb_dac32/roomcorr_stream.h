/*
 * Glue between the USB audio ring (48-byte chunks, every 125 us) and the convolver
 * (1024-frame blocks, every 21.3 ms).
 *
 * The SAI DMA callback runs at 8 kHz and must not do FFT work, so it only moves bytes:
 * it pushes what it took from the USB ring into an input FIFO and pulls already
 * processed audio out of an output FIFO. RCS_Task() does the convolution from the main
 * loop. Both FIFOs are single-producer/single-consumer, one side ISR and one side
 * thread, which needs no locking as long as the indices are word-aligned and volatile.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_STREAM_H_
#define _ROOMCORR_STREAM_H_

#include <stdbool.h>
#include <stdint.h>

/*! Allocate the FIFOs in SDRAM and start the convolver. Call once SDRAM is up. */
void RCS_Init(void);

/*!
 * @brief Carve a block out of SDRAM, 32-byte aligned.
 *
 * Everything that is large but not latency critical lives here rather than in the
 * 128 KB DTCM, which lwIP and the USB stack have already largely spoken for.
 */
void *RCS_SdramAlloc(uint32_t bytes);

/*! ISR: hand over the bytes just consumed from the USB play buffer. */
void RCS_PushInput(const void *src, uint32_t bytes);
/*! ISR: push `bytes` of silence (used while the host is not streaming). */
void RCS_PushSilence(uint32_t bytes);
/*! ISR: fill dst with processed audio. Returns false (and zeroes dst) on underrun. */
bool RCS_PopOutput(void *dst, uint32_t bytes);

/*! Main loop: convolve whole blocks while input is available and output has room. */
void RCS_Task(void);

uint32_t RCS_Underruns(void);
/*! Zero the underrun counter. */
void RCS_ResetUnderruns(void);
/*! Frames of audio currently sitting in the output FIFO. */
uint32_t RCS_OutputFill(void);
/*! One line of load/health statistics on the debug console. */
void RCS_PrintStats(void);

#endif /* _ROOMCORR_STREAM_H_ */
