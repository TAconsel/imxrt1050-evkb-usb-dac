/*
 * Ethernet + lwIP + a small HTTP control surface.
 *
 * Bare-metal lwIP (NO_SYS=1) driven from the same main loop as the audio, so nothing
 * preempts the convolver. RC_NET_Task() must be called often; it never blocks.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_NET_H_
#define _ROOMCORR_NET_H_

#include <stdbool.h>
#include <stdint.h>

/*! Bring up the PHY, lwIP and DHCP, and start listening on port 80. */
void RC_NET_Init(void);

/*! Poll the MAC and lwIP timers. Call from the main loop. */
void RC_NET_Task(void);

/*! True once DHCP (or the static fallback) has given us an address. */
bool RC_NET_IsUp(void);

/*! Dotted-quad of the current address, or "0.0.0.0". */
const char *RC_NET_AddressText(void);

#endif /* _ROOMCORR_NET_H_ */
