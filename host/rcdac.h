/*
 * Host-side transport for the RT1050 room-correction DAC.
 *
 * Vendor control transfers on endpoint 0 of the audio device. snd-usb-audio keeps the
 * audio interfaces the whole time -- endpoint 0 needs no interface claim -- so the DAC
 * carries on playing while we talk to it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RCDAC_H
#define RCDAC_H

#include <stdbool.h>
#include <stdint.h>
#include "../usb_dac32/roomcorr_usbctl.h"

#define RC_VID 0x1fc9
#define RC_PID 0x0098

typedef struct rc_dev rc_dev;

/*! Open the first attached DAC. Returns NULL and sets *err on failure. */
rc_dev *rc_open(const char **err);
void    rc_close(rc_dev *d);

/*! Read the full device state. */
bool rc_status(rc_dev *d, rc_usb_status_t *out, const char **err);

bool rc_set_bypass(rc_dev *d, bool bypass, const char **err);
bool rc_set_preamp(rc_dev *d, float db, const char **err);
bool rc_set_eq(rc_dev *d, int band, float db, const char **err);

/*!
 * @brief Upload a .wav impulse response and rebuild the filter.
 *
 * progress(done, total, user) is called as chunks go out; it may be NULL.
 * On success the device's own parse result is in the next rc_status().
 */
bool rc_upload_ir(rc_dev *d, const uint8_t *wav, uint32_t len,
                  void (*progress)(uint32_t, uint32_t, void *), void *user,
                  const char **err);

/*! Zero the block, underrun, clip and peak-load counters. */
bool rc_reset_stats(rc_dev *d, const char **err);

/*! Human-readable form of rc_usb_status_t.irResult. */
const char *rc_ir_result_text(uint8_t code);

#endif
