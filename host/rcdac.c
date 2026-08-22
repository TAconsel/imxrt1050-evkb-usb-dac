/*
 * See rcdac.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _DEFAULT_SOURCE

#include <libusb-1.0/libusb.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "rcdac.h"

#define RC_TIMEOUT_MS 2000

#define RC_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT)
#define RC_IN  (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN)

struct rc_dev
{
    libusb_context       *ctx;
    libusb_device_handle *h;
};

rc_dev *rc_open(const char **err)
{
    rc_dev *d = calloc(1, sizeof(*d));
    if (d == NULL)
    {
        *err = "out of memory";
        return NULL;
    }
    if (libusb_init(&d->ctx) != 0)
    {
        *err = "libusb_init failed";
        free(d);
        return NULL;
    }
    d->h = libusb_open_device_with_vid_pid(d->ctx, RC_VID, RC_PID);
    if (d->h == NULL)
    {
        *err = "DAC not found (is it plugged in, and is the udev rule installed?)";
        libusb_exit(d->ctx);
        free(d);
        return NULL;
    }
    /*
     * Deliberately no libusb_claim_interface and no detach_kernel_driver: the audio
     * interfaces belong to snd-usb-audio and must stay there so playback keeps running.
     * Control transfers to endpoint 0 do not need a claim.
     */
    return d;
}

void rc_close(rc_dev *d)
{
    if (d != NULL)
    {
        libusb_close(d->h);
        libusb_exit(d->ctx);
        free(d);
    }
}

static bool ctrl_out(rc_dev *d, uint8_t req, uint16_t val, uint16_t idx,
                     void *data, uint16_t len, const char **err)
{
    int r = libusb_control_transfer(d->h, RC_OUT, req, val, idx,
                                    (unsigned char *)data, len, RC_TIMEOUT_MS);
    if (r < 0)
    {
        *err = libusb_strerror(r);
        return false;
    }
    return true;
}

bool rc_status(rc_dev *d, rc_usb_status_t *out, const char **err)
{
    int r = libusb_control_transfer(d->h, RC_IN, RC_REQ_STATUS, 0, 0,
                                    (unsigned char *)out, sizeof(*out), RC_TIMEOUT_MS);
    if (r < 0)
    {
        *err = libusb_strerror(r);
        return false;
    }
    if ((size_t)r < sizeof(*out) || out->magic != RC_USB_MAGIC)
    {
        *err = "unexpected status reply (firmware too old?)";
        return false;
    }
    return true;
}

static bool set_param(rc_dev *d, uint32_t param, int32_t index, float value, const char **err)
{
    rc_usb_set_t s = {.param = param, .index = index, .value = value};
    return ctrl_out(d, RC_REQ_SET, 0, 0, &s, sizeof(s), err);
}

bool rc_set_bypass(rc_dev *d, bool bypass, const char **err)
{
    return set_param(d, RC_PARAM_BYPASS, 0, bypass ? 1.0f : 0.0f, err);
}

bool rc_set_preamp(rc_dev *d, float db, const char **err)
{
    return set_param(d, RC_PARAM_PREAMP, 0, db, err);
}

bool rc_set_eq(rc_dev *d, int band, float db, const char **err)
{
    return set_param(d, RC_PARAM_EQ, band, db, err);
}

bool rc_upload_ir(rc_dev *d, const uint8_t *wav, uint32_t len,
                  void (*progress)(uint32_t, uint32_t, void *), void *user,
                  const char **err)
{
    /* the length rides in wValue:wIndex so RC_REQ_IR_BEGIN needs no data stage */
    if (!ctrl_out(d, RC_REQ_IR_BEGIN, (uint16_t)(len >> 16), (uint16_t)(len & 0xFFFFU),
                  NULL, 0, err))
    {
        return false;
    }
    for (uint32_t off = 0; off < len; off += RC_USB_CHUNK)
    {
        uint16_t n = (uint16_t)((len - off > RC_USB_CHUNK) ? RC_USB_CHUNK : (len - off));
        if (!ctrl_out(d, RC_REQ_IR_DATA, 0, 0, (void *)(uintptr_t)&wav[off], n, err))
        {
            return false;
        }
        if (progress != NULL)
        {
            progress(off + n, len, user);
        }
    }
    if (!ctrl_out(d, RC_REQ_IR_COMMIT, 0, 0, NULL, 0, err))
    {
        return false;
    }

    /*
     * The board rebuilds the filter from its main loop rather than from the USB
     * interrupt, so the answer is not ready when COMMIT returns. Poll until it stops
     * saying "in progress" -- a rebuild is tens of milliseconds, so this is quick.
     */
    for (int i = 0; i < 200; i++)
    {
        rc_usb_status_t s;
        if (!rc_status(d, &s, err))
        {
            return false;
        }
        if (s.irResult != 0xFF)
        {
            if (s.irResult != 0)
            {
                *err = rc_ir_result_text(s.irResult);
                return false;
            }
            return true;
        }
        usleep(10000);
    }
    *err = "timed out waiting for the board to rebuild the filter";
    return false;
}

const char *rc_ir_result_text(uint8_t code)
{
    switch (code)
    {
        case 0:    return "ok";
        case 1:    return "not a RIFF/WAVE file";
        case 2:    return "need PCM 16/24/32-bit or IEEE float32";
        case 3:    return "need 48000 or 96000 Hz";
        case 4:    return "need mono or stereo";
        case 5:    return "no data chunk";
        case 6:    return "impulse response longer than the convolver allows";
        case 0xFF: return "in progress";
        default:   return "none";
    }
}
