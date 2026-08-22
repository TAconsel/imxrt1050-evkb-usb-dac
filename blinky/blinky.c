/*
 * Onboard user LED blinky - MIMXRT1050-EVKB
 *
 * D18 (USER_LED) is driven by GPIO_AD_B0_09 == GPIO1_IO09, active low
 * (see SPF-30168_B1 schematic, net USER_LED).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_debug_console.h"
#include "app.h"

/*******************************************************************************
 * Variables
 ******************************************************************************/
volatile uint32_t g_systickCounter;

/*******************************************************************************
 * Code
 ******************************************************************************/
void SysTick_Handler(void)
{
    if (g_systickCounter != 0U)
    {
        g_systickCounter--;
    }
}

/* LPUART1 -> the DAPLink USB VCOM (/dev/ttyACM*) at 115200 8N1 */
static void BOARD_InitDebugUartPins(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_12_LPUART1_TXD, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_13_LPUART1_RXD, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_12_LPUART1_TXD, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_13_LPUART1_RXD, 0x10B0U);
}

static void SysTick_DelayTicks(uint32_t n)
{
    g_systickCounter = n;
    while (g_systickCounter != 0U)
    {
    }
}

int main(void)
{
    gpio_pin_config_t led_config = {kGPIO_DigitalOutput, LOGIC_LED_OFF, kGPIO_NoIntmode};

    /* MPU, pin mux and clocks */
    BOARD_InitHardware();
    BOARD_InitDebugUartPins();
    BOARD_InitDebugConsole();

    PRINTF("\r\nMIMXRT1050-EVKB blinky - core @ %u Hz\r\n", (unsigned)SystemCoreClock);
    PRINTF("USER_LED D18 = GPIO_AD_B0_09 = GPIO1_IO09 (active low)\r\n");

    GPIO_PinInit(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN, &led_config);

    /* 1 ms SysTick */
    if (SysTick_Config(SystemCoreClock / 1000U))
    {
        while (1)
        {
        }
    }

    while (1)
    {
        SysTick_DelayTicks(500U);
        GPIO_PortToggle(BOARD_USER_LED_GPIO, 1U << BOARD_USER_LED_GPIO_PIN);
        PRINTF("LED %s\r\n",
               (GPIO_PinRead(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN) == LOGIC_LED_ON) ? "on " : "off");
    }
}
