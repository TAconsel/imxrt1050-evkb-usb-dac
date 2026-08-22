/*
 * Front-panel UI for the room correction: the board's one user button toggles the
 * correction, the board's one user LED shows whether it is engaged.
 *
 *   SW8  "USER_BUTTON"  = WAKEUP pin -> GPIO5_IO00, active low (external pull-up)
 *   D18  "USER_LED"     = GPIO_AD_B0_09 -> GPIO1_IO09, active low
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _ROOMCORR_UI_H_
#define _ROOMCORR_UI_H_

/*! Mux and configure SW8 and D18. Call once, after the clocks are up. */
void RC_UI_Init(void);

/*!
 * @brief Poll the button and drive the LED. Call from the main loop.
 *
 * Polled rather than interrupt driven: the main loop already turns over every few
 * milliseconds, which is exactly the timescale a button wants, and it keeps the
 * audio path free of another interrupt source.
 */
void RC_UI_Task(void);

#endif /* _ROOMCORR_UI_H_ */
