/*
 * Vendor control-request interface, shared verbatim with the host GTK app.
 *
 * Everything rides on endpoint 0 of the existing USB Audio device, as vendor requests.
 * No extra interface, no composite descriptor, no second cable: the host opens the same
 * 1fc9:0098 device with libusb and issues control transfers. snd-usb-audio keeps owning
 * the audio interfaces the whole time -- control transfers to endpoint 0 do not require
 * claiming one, so the DAC keeps playing while the app talks to it.
 *
 * All multi-byte fields are little-endian, which both ends are natively.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_USBCTL_H_
#define _ROOMCORR_USBCTL_H_

#include <stdint.h>

/* bRequest values, bmRequestType = vendor | device */
#define RC_REQ_STATUS    (0x01U) /* IN,  returns rc_usb_status_t */
#define RC_REQ_SET       (0x02U) /* OUT, rc_usb_set_t */
#define RC_REQ_IR_BEGIN  (0x10U) /* OUT, wValue:wIndex = total byte count (hi:lo) */
#define RC_REQ_IR_DATA   (0x11U) /* OUT, next chunk of the wav */
#define RC_REQ_IR_COMMIT (0x12U) /* OUT, no data: parse and swap the filter in */

#define RC_USB_MAGIC     (0x31534352U) /* "RCS1" */
#define RC_USB_CHUNK     (1024U)       /* bytes per RC_REQ_IR_DATA transfer */
#define RC_USB_DESC_LEN  (48U)

/* RC_REQ_SET parameter ids */
#define RC_PARAM_BYPASS  (0U) /* value: 0 or 1 */
#define RC_PARAM_PREAMP  (1U) /* value: dB */
#define RC_PARAM_EQ      (2U) /* index: band, value: dB */

typedef struct
{
    uint32_t magic;
    uint8_t  bypass;
    uint8_t  eqBands;
    uint8_t  irResult;   /* rc_filter_status_t of the last upload */
    uint8_t  reserved;
    float    preamp;
    float    eq[16];
    uint32_t taps;
    uint32_t srcRate;
    uint32_t cpuPercent;
    uint32_t underruns;
    uint32_t clips;
    uint32_t blocks;
    char     filter[RC_USB_DESC_LEN];
} rc_usb_status_t;

typedef struct
{
    uint32_t param;
    int32_t  index;
    float    value;
} rc_usb_set_t;

#ifdef RC_USBCTL_FIRMWARE /* the host app wants only the wire structs above */
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
/*!
 * @brief Handle one vendor control request.
 * @return kStatus_USB_Success if the request was ours, kStatus_USB_InvalidRequest to stall.
 */
usb_status_t RC_USBCTL_Handle(usb_device_control_request_struct_t *req);
#endif

#endif /* _ROOMCORR_USBCTL_H_ */
