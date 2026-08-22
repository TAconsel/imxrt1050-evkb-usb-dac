/*
 * See roomcorr_stream.h.
 *
 * Everything large lives in SDRAM, handed out by a bump allocator from RC_SDRAM_BASE:
 *
 *   0x80000000  frequency-domain delay line   256 KiB
 *               input FIFO                     24 KiB  (3 blocks)
 *               output FIFO                    24 KiB  (3 blocks)
 *               block scratch in/out           16 KiB
 *
 * The FIFOs are CPU-only -- the SAI DMA never reads them, it is fed from a small
 * non-cacheable staging buffer in hardware_init.c -- so they can be plain cacheable
 * memory with no maintenance.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "fsl_debug_console.h"
#include "roomcorr.h"
#include "roomcorr_eq.h"
#include "roomcorr_filter.h"
#include "roomcorr_stream.h"

#define RCS_FIFO_BLOCKS  (3U)
#define RCS_BLOCK_BYTES  (RC_BLOCK * RC_CHANNELS * sizeof(int32_t))
#define RCS_FIFO_BYTES   (RCS_FIFO_BLOCKS * RCS_BLOCK_BYTES)

typedef struct
{
    uint8_t *buf;
    volatile uint32_t head; /* producer writes */
    volatile uint32_t tail; /* consumer writes */
} rcs_fifo_t;

static rcs_fifo_t s_in;   /* ISR produces, task consumes */
static rcs_fifo_t s_out;  /* task produces, ISR consumes */
static int32_t   *s_blkIn;
static int32_t   *s_blkOut;

static volatile uint32_t s_underruns;
static uint8_t *s_sdramCursor;

/* ------------------------------------------------------------------ fifo ---- */

static inline uint32_t fifo_used(const rcs_fifo_t *f)
{
    const uint32_t h = f->head, t = f->tail;
    return (h >= t) ? (h - t) : (RCS_FIFO_BYTES - t + h);
}

static inline uint32_t fifo_free(const rcs_fifo_t *f)
{
    /* one byte kept back so head == tail unambiguously means empty */
    return RCS_FIFO_BYTES - fifo_used(f) - 1U;
}

static void fifo_write(rcs_fifo_t *f, const uint8_t *src, uint32_t n)
{
    uint32_t h = f->head;
    const uint32_t first = ((h + n) > RCS_FIFO_BYTES) ? (RCS_FIFO_BYTES - h) : n;

    if (src != NULL)
    {
        memcpy(&f->buf[h], src, first);
        memcpy(&f->buf[0], &src[first], n - first);
    }
    else
    {
        memset(&f->buf[h], 0, first);
        memset(&f->buf[0], 0, n - first);
    }
    h += n;
    if (h >= RCS_FIFO_BYTES)
    {
        h -= RCS_FIFO_BYTES;
    }
    __DMB();
    f->head = h;
}

static void fifo_read(rcs_fifo_t *f, uint8_t *dst, uint32_t n)
{
    uint32_t t = f->tail;
    const uint32_t first = ((t + n) > RCS_FIFO_BYTES) ? (RCS_FIFO_BYTES - t) : n;

    memcpy(dst, &f->buf[t], first);
    memcpy(&dst[first], &f->buf[0], n - first);
    t += n;
    if (t >= RCS_FIFO_BYTES)
    {
        t -= RCS_FIFO_BYTES;
    }
    __DMB();
    f->tail = t;
}

/* ---------------------------------------------------------------- public ---- */

void RCS_Init(void)
{
    uint8_t *p = (uint8_t *)RC_SDRAM_BASE;

    p += RC_FDL_FLOATS * sizeof(float32_t); /* the convolver owns the first slab */

    s_in.buf  = p; p += RCS_FIFO_BYTES;
    s_out.buf = p; p += RCS_FIFO_BYTES;
    s_blkIn   = (int32_t *)(void *)p; p += RCS_BLOCK_BYTES;
    s_blkOut  = (int32_t *)(void *)p; p += RCS_BLOCK_BYTES;

    /* the filter store takes the rest of what we need: upload staging, decode scratch
     * and the two swappable partition slots */
    RC_FILTER_Init(p, (void **)&p);
    s_sdramCursor = p;

    s_in.head = s_in.tail = 0U;
    s_out.head = s_out.tail = 0U;
    s_underruns = 0U;

    memset(s_in.buf, 0, RCS_FIFO_BYTES);
    memset(s_out.buf, 0, RCS_FIFO_BYTES);

    RC_EQ_Init();
    RC_Init();

    PRINTF("room correction: SDRAM 0x%08x..0x%08x (%d KiB)\r\n",
             (unsigned)RC_SDRAM_BASE, (unsigned)(uintptr_t)p,
             (int)(((uintptr_t)p - RC_SDRAM_BASE) / 1024U));
}

void *RCS_SdramAlloc(uint32_t bytes)
{
    uint8_t *r = s_sdramCursor;
    s_sdramCursor += (bytes + 31U) & ~31U;
    return r;
}

void RCS_PushInput(const void *src, uint32_t bytes)
{
    if (fifo_free(&s_in) >= bytes)
    {
        fifo_write(&s_in, (const uint8_t *)src, bytes);
    }
    /* else: the task is not keeping up; dropping is better than corrupting indices */
}

void RCS_PushSilence(uint32_t bytes)
{
    if (fifo_free(&s_in) >= bytes)
    {
        fifo_write(&s_in, NULL, bytes);
    }
}

bool RCS_PopOutput(void *dst, uint32_t bytes)
{
    if (fifo_used(&s_out) >= bytes)
    {
        fifo_read(&s_out, (uint8_t *)dst, bytes);
        return true;
    }
    memset(dst, 0, bytes);
    s_underruns++;
    return false;
}

void RCS_Task(void)
{
    while ((fifo_used(&s_in) >= RCS_BLOCK_BYTES) && (fifo_free(&s_out) >= RCS_BLOCK_BYTES))
    {
        fifo_read(&s_in, (uint8_t *)s_blkIn, RCS_BLOCK_BYTES);
        RC_ProcessBlock(s_blkIn, s_blkOut);
        fifo_write(&s_out, (const uint8_t *)s_blkOut, RCS_BLOCK_BYTES);
    }
}

uint32_t RCS_Underruns(void) { return s_underruns; }

uint32_t RCS_OutputFill(void)
{
    return fifo_used(&s_out) / (RC_CHANNELS * sizeof(int32_t));
}

void RCS_PrintStats(void)
{
    const uint32_t budget = (RC_BLOCK * 1000000U) / RC_SAMPLE_RATE; /* us per block */
    const uint32_t us     = RC_PeakMicros();

    PRINTF("rc: blocks %d, peak %d us of %d us = %d%% CPU, "
             "out fill %d fr, underruns %d, clips %d, %s\r\n",
             (int)RC_BlockCount(), (int)us, (int)budget,
             (int)((us * 100U) / budget), (int)RCS_OutputFill(),
             (int)RCS_Underruns(), (int)RC_ClipCount(),
             RC_GetBypass() ? "BYPASS" : "active");
}
