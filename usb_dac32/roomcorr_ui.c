/*
 * See roomcorr_ui.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "roomcorr.h"
#include "roomcorr_ui.h"

/*
 * Debounce: the button must read the same way this many consecutive polls before the
 * state is believed. RCS_Task() makes the loop period lumpy -- a few microseconds when
 * there is no block to do, ~5 ms when there is -- so this is counted in polls, not
 * milliseconds, and the count is set high enough to cover the fast case.
 */
#define RC_UI_DEBOUNCE_POLLS (2000U)

#define RC_UI_PRESSED (0U) /* SW8 shorts the pin to ground */

static uint8_t  s_stable;      /* last believed level */
static uint8_t  s_candidate;   /* level currently being confirmed */
static uint32_t s_count;
static bool     s_ledOn;

static void ui_set_led(bool on)
{
    /* D18 is active low: driving the pin low lights it. */
    GPIO_PinWrite(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN, on ? LOGIC_LED_ON : LOGIC_LED_OFF);
    s_ledOn = on;
}

void RC_UI_Init(void)
{
    gpio_pin_config_t swConfig  = {kGPIO_DigitalInput, 0, kGPIO_NoIntmode};
    gpio_pin_config_t ledConfig = {kGPIO_DigitalOutput, LOGIC_LED_OFF, kGPIO_NoIntmode};

    /*
     * The audio example's pin_mux.c does not touch either pin, so mux them here rather
     * than editing the generated file.
     *
     * Note GPIO_AD_B0_09 is also JTAG_TDI. Driving the LED disturbs a JTAG/SWD session
     * on this board -- that is a property of the hardware, not a bug here.
     */
    IOMUXC_SetPinMux(IOMUXC_SNVS_WAKEUP_GPIO5_IO00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0x10B0U);

    GPIO_PinInit(BOARD_USER_BUTTON_GPIO, BOARD_USER_BUTTON_GPIO_PIN, &swConfig);
    GPIO_PinInit(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN, &ledConfig);

    s_stable    = (uint8_t)GPIO_PinRead(BOARD_USER_BUTTON_GPIO, BOARD_USER_BUTTON_GPIO_PIN);
    s_candidate = s_stable;
    s_count     = 0U;

    ui_set_led(!RC_GetBypass());

    PRINTF("%s toggles room correction, D18 lit = correction engaged\r\n",
           BOARD_USER_BUTTON_NAME);
}

void RC_UI_Task(void)
{
    const uint8_t level =
        (uint8_t)GPIO_PinRead(BOARD_USER_BUTTON_GPIO, BOARD_USER_BUTTON_GPIO_PIN);

    if (level != s_candidate)
    {
        s_candidate = level;
        s_count     = 0U;
    }
    else if (s_candidate != s_stable)
    {
        s_count++;
        if (s_count >= RC_UI_DEBOUNCE_POLLS)
        {
            s_stable = s_candidate;
            s_count  = 0U;

            if (s_stable == RC_UI_PRESSED) /* act on the press, not the release */
            {
                const bool bypass = !RC_GetBypass();
                RC_SetBypass(bypass);
                ui_set_led(!bypass);
                PRINTF("room correction %s\r\n", bypass ? "BYPASSED" : "engaged");
            }
        }
    }
    else
    {
        s_count = 0U;
    }

    /* keep the LED honest if anything else changes the mode */
    if (s_ledOn == RC_GetBypass())
    {
        ui_set_led(!RC_GetBypass());
    }
}
