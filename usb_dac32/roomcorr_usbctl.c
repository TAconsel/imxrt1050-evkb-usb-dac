/*
 * See roomcorr_usbctl.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define RC_USBCTL_FIRMWARE

#include <string.h>
#include "fsl_debug_console.h"
#include "roomcorr.h"
#include "roomcorr_eq.h"
#include "roomcorr_filter.h"
#include "roomcorr_stream.h"
#include "roomcorr_usbctl.h"

/*
 * These buffers are handed to the USB stack as control-transfer targets, so they must
 * outlive the callback. They are small enough to stay on-chip.
 */
static rc_usb_status_t s_status;
static rc_usb_set_t    s_set;
static uint8_t         s_chunk[RC_USB_CHUNK];

static uint32_t s_irExpected; /* bytes announced by RC_REQ_IR_BEGIN */
static uint32_t s_irGot;
static uint8_t  s_irResult;
static volatile bool s_irPending;

static void usbctl_fill_status(void)
{
    const uint32_t budgetUs = (RC_BLOCK * 1000000U) / RC_SAMPLE_RATE;

    (void)memset(&s_status, 0, sizeof(s_status));
    s_status.magic      = RC_USB_MAGIC;
    s_status.bypass     = RC_GetBypass() ? 1U : 0U;
    s_status.eqBands    = (uint8_t)RC_EQ_BANDS;
    s_status.irResult   = s_irResult;
    s_status.preamp     = RC_EQ_GetPreamp();
    s_status.taps       = RC_FILTER_Taps();
    s_status.srcRate    = RC_FILTER_SourceRate();
    s_status.cpuPercent     = (RC_LastMicros() * 100U) / budgetUs;
    s_status.cpuPeakPercent = (RC_PeakMicros() * 100U) / budgetUs;
    s_status.underruns  = RCS_Underruns();
    s_status.clips      = RC_ClipCount();
    s_status.blocks     = RC_BlockCount();

    for (uint32_t b = 0U; b < RC_EQ_BANDS; b++)
    {
        s_status.eq[b] = RC_EQ_GetBand(b);
    }
    RC_TakePeaks(s_status.peak);
    (void)strncpy(s_status.filter, RC_FILTER_Describe(), RC_USB_DESC_LEN - 1U);
}

static void usbctl_apply_set(void)
{
    switch (s_set.param)
    {
        case RC_PARAM_BYPASS:
            RC_SetBypass(s_set.value != 0.0f);
            break;
        case RC_PARAM_PREAMP:
            RC_EQ_SetPreamp(s_set.value);
            break;
        case RC_PARAM_EQ:
            RC_EQ_SetBand((uint32_t)s_set.index, s_set.value);
            break;
        default:
            /* ignore unknown parameters rather than stalling the pipe */
            break;
    }
}

void RC_USBCTL_Task(void)
{
    if (s_irPending)
    {
        s_irPending = false;
        s_irResult  = (uint8_t)RC_FILTER_LoadWav(s_irGot);
        /* the rebuild itself is not steady-state load, so do not let it skew the peak */
        RC_ResetPeak();
        PRINTF("usbctl: IR upload finished, %u bytes, result %d\r\n",
               (unsigned)s_irGot, (int)s_irResult);
    }
}

usb_status_t RC_USBCTL_Handle(usb_device_control_request_struct_t *req)
{
    const uint8_t bRequest = req->setup->bRequest;

    switch (bRequest)
    {
        case RC_REQ_STATUS:
            if (req->isSetup == 1U)
            {
                usbctl_fill_status();
                req->buffer = (uint8_t *)&s_status;
                req->length = sizeof(s_status);
            }
            return kStatus_USB_Success;

        case RC_REQ_SET:
            if (req->isSetup == 1U)
            {
                req->buffer = (uint8_t *)&s_set;
                req->length = sizeof(s_set);
            }
            else
            {
                usbctl_apply_set();
            }
            return kStatus_USB_Success;

        case RC_REQ_RESET:
            if (req->isSetup == 1U)
            {
                req->buffer = NULL;
                req->length = 0U;
                RC_ResetStats();
                RCS_ResetUnderruns();
            }
            return kStatus_USB_Success;

        case RC_REQ_IR_BEGIN:
            if (req->isSetup == 1U)
            {
                /* length arrives split across wValue:wIndex so no data stage is needed */
                s_irExpected = ((uint32_t)req->setup->wValue << 16) | (uint32_t)req->setup->wIndex;
                s_irGot      = 0U;
                s_irResult   = 0xFFU; /* in progress */
                req->buffer  = NULL;
                req->length  = 0U;
                PRINTF("usbctl: IR upload starting, %u bytes\r\n", (unsigned)s_irExpected);
            }
            return (s_irExpected <= RC_UPLOAD_MAX_BYTES) ? kStatus_USB_Success
                                                         : kStatus_USB_InvalidRequest;

        case RC_REQ_IR_DATA:
            if (req->isSetup == 1U)
            {
                req->buffer = s_chunk;
                req->length = (req->setup->wLength > RC_USB_CHUNK) ? RC_USB_CHUNK
                                                                   : req->setup->wLength;
            }
            else
            {
                const uint32_t n    = req->length;
                const uint32_t room = RC_UPLOAD_MAX_BYTES - s_irGot;
                const uint32_t take = (n < room) ? n : room;

                (void)memcpy(&RC_FILTER_UploadBuffer()[s_irGot], s_chunk, take);
                s_irGot += take;
            }
            return kStatus_USB_Success;

        case RC_REQ_IR_COMMIT:
            if (req->isSetup == 1U)
            {
                req->buffer = NULL;
                req->length = 0U;
                /* hand the work to the main loop: see RC_USBCTL_Task() */
                s_irPending = true;
            }
            return kStatus_USB_Success;

        default:
            return kStatus_USB_InvalidRequest;
    }
}
