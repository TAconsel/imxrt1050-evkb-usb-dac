/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*${header:start}*/
#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_ch9.h"
#include "usb_device_descriptor.h"
#include "usb_device_audio.h"
#include "audio_speaker.h"
#include "pin_mux.h"
#include "usb_phy.h"
#include "clock_config.h"
#include "board.h"
#include "app.h"
#include "fsl_sai.h"
#include "fsl_dmamux.h"
#include "fsl_sai_edma.h"
#include "fsl_codec_common.h"
#include "fsl_wm8960.h"
#include "fsl_adapter_audio.h"
#include "fsl_codec_adapter.h"
#include "roomcorr.h"
#include "roomcorr_stream.h"
#include "roomcorr_ui.h"
/*${header:end}*/
/*${variable:start}*/
extern usb_audio_speaker_struct_t g_UsbDeviceAudioSpeaker;
extern uint8_t audioPlayDataBuff[AUDIO_SPEAKER_DATA_WHOLE_BUFFER_COUNT_NORMAL * AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME];
HAL_AUDIO_HANDLE_DEFINE(audioTxHandle);
hal_audio_config_t audioTxConfig;
hal_audio_dma_config_t dmaTxConfig;
hal_audio_ip_config_t ipTxConfig;
hal_audio_dma_mux_config_t dmaMuxTxConfig;
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t audioPlayDMATempBuff[AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME];
/*
 * The SAI DMA is fed from these instead of straight out of the USB ring, because the
 * audio now goes through the convolver first. Ping-pong so the engine is never reading
 * the buffer we are refilling. Non-cacheable, like every other DMA-visible buffer here.
 */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t audioPlayStageBuff[2][AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME];
static uint8_t audioPlayStageIdx;
uint32_t masterClockHz = 0U;
codec_handle_t codecHandle;

wm8960_config_t wm8960Config = {
    .i2cConfig = {.codecI2CInstance = BOARD_CODEC_I2C_INSTANCE, .codecI2CSourceClock = BOARD_CODEC_I2C_CLOCK_FREQ},
    .route     = kWM8960_RoutePlaybackandRecord,
    .leftInputSource  = kWM8960_InputDifferentialMicInput3,
    .rightInputSource = kWM8960_InputDifferentialMicInput2,
    .playSource       = kWM8960_PlaySourceDAC,
    .slaveAddress     = WM8960_I2C_ADDR,
    .bus              = kWM8960_BusI2S,
    .format           = {.mclk_HZ    = 12288000U,
               .sampleRate = kWM8960_AudioSampleRate48KHz,
               .bitWidth   = kWM8960_AudioBitWidth32bit},
    .master_slave     = false,
};
codec_config_t boardCodecConfig = {.codecDevType = kCODEC_WM8960, .codecDevConfig = &wm8960Config};

/*
 * AUDIO PLL setting: Frequency = Fref * (DIV_SELECT + NUM / DENOM)
 *                              = 24 * (32 + 77/100)
 *                              = 786.48 MHz
 */
const clock_audio_pll_config_t audioPllConfig = {
    .loopDivider = 32,  /* PLL loop divider. Valid range for DIV_SELECT divider value: 27~54. */
    .postDivider = 1,   /* Divider after the PLL, should only be 1, 2, 4, 8, 16. */
    .numerator   = 77,  /* 30 bit numerator of fractional loop divider. */
    .denominator = 100, /* 30 bit denominator of fractional loop divider */
};
extern void WM8960_USB_Audio_Init(void *I2CBase, void *i2cHandle);
extern void WM8960_Config_Audio_Formats(uint32_t samplingRate);
extern uint32_t USB_AudioSpeakerBufferSpaceUsed(void);
extern void USB_DeviceCalculateFeedback(void);
/*${variable:end}*/
/*${function:start}*/

void BOARD_EnableSaiMclkOutput(bool enable)
{
    if (enable)
    {
        IOMUXC_GPR->GPR1 |= IOMUXC_GPR_GPR1_SAI1_MCLK_DIR_MASK;
    }
    else
    {
        IOMUXC_GPR->GPR1 &= (~IOMUXC_GPR_GPR1_SAI1_MCLK_DIR_MASK);
    }
}

void BOARD_InitHardware(void)
{
    BOARD_ConfigMPU();

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    CLOCK_InitAudioPll(&audioPllConfig);
    BOARD_InitDebugConsole();

    /*Clock setting for LPI2C*/
    CLOCK_SetMux(kCLOCK_Lpi2cMux, DEMO_LPI2C_CLOCK_SOURCE_SELECT);
    CLOCK_SetDiv(kCLOCK_Lpi2cDiv, DEMO_LPI2C_CLOCK_SOURCE_DIVIDER);

    /*Clock setting for SAI1*/
    CLOCK_SetMux(kCLOCK_Sai1Mux, DEMO_SAI1_CLOCK_SOURCE_SELECT);
    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, DEMO_SAI1_CLOCK_SOURCE_PRE_DIVIDER);
    CLOCK_SetDiv(kCLOCK_Sai1Div, DEMO_SAI1_CLOCK_SOURCE_DIVIDER);

    /*Enable MCLK clock*/
    BOARD_EnableSaiMclkOutput(true);

    dmaMuxTxConfig.dmaMuxConfig.dmaMuxInstance   = DEMO_DMAMUX_INDEX;
    dmaMuxTxConfig.dmaMuxConfig.dmaRequestSource = DEMO_SAI_TX_SOURCE;
    dmaTxConfig.dmaMuxConfig                     = &dmaMuxTxConfig;
    dmaTxConfig.instance                         = DEMO_DMA_INDEX;
    dmaTxConfig.channel                          = DEMO_DMA_TX_CHANNEL;
    dmaTxConfig.priority                         = kHAL_AudioDmaChannelPriorityDefault;
    dmaTxConfig.dmaChannelMuxConfig              = NULL;
    ipTxConfig.sai.lineMask                      = 1U << 0U;
    ipTxConfig.sai.syncMode                      = kHAL_AudioSaiModeAsync;
    audioTxConfig.dmaConfig                      = &dmaTxConfig;
    audioTxConfig.ipConfig                       = &ipTxConfig;
    audioTxConfig.instance                       = DEMO_SAI_INSTANCE_INDEX;
    audioTxConfig.srcClock_Hz                    = DEMO_SAI_CLK_FREQ;
    audioTxConfig.sampleRate_Hz                  = (uint32_t)kHAL_AudioSampleRate48KHz;
    audioTxConfig.masterSlave                    = kHAL_AudioMaster;
    audioTxConfig.bclkPolarity                   = kHAL_AudioSampleOnRisingEdge;
    audioTxConfig.frameSyncWidth                 = kHAL_AudioFrameSyncWidthHalfFrame;
    audioTxConfig.frameSyncPolarity              = kHAL_AudioBeginAtFallingEdge;
    audioTxConfig.dataFormat                     = kHAL_AudioDataFormatI2sClassic;
    audioTxConfig.fifoWatermark                  = (uint8_t)(FSL_FEATURE_SAI_FIFO_COUNTn(DEMO_SAI) - 1);
    audioTxConfig.bitWidth                       = (uint8_t)kHAL_AudioWordWidth32bits;
    audioTxConfig.lineChannels                   = kHAL_AudioStereo;

    /* SDRAM is up (boot-header DCD) and BOARD_ConfigMPU() has already marked
     * 0x80000000 Normal cacheable, because SKIP_SYSCLK_INIT is defined. */
    RCS_Init();
    RC_UI_Init();
}

/*
 * Hardware volume, done in the WM8960 rather than by scaling samples on the M7.
 *
 * kWM8960_ModuleHP is the ANALOG headphone output amplifier (LOUT1VOL/ROUT1VOL):
 * register 0x30..0x7F == -73 dB..+6 dB in 1 dB steps, 0x00..0x2F mutes, so 0 dB is
 * 0x79. Attenuating after the DAC keeps the full 24-bit converter range at every
 * volume setting, which digital attenuation would not.
 *
 * Mute uses kWM8960_ModuleDAC (LDACVOL/RDACVOL) on purpose: WM8960_SetMute() with
 * kWM8960_ModuleHP would write a fixed 0x6F back into the analog volume register and
 * silently discard the host's setting.
 */
#define WM8960_HP_VOL_0DB    (0x79)
#define WM8960_HP_VOL_MIN_DB (-73)
#define WM8960_HP_VOL_MAX_DB (6)
/* UAC 2.0 reserves 0x8000 for "silence" */
#define USB_AUDIO_VOLUME_SILENCE ((int16_t)0x8000)

static wm8960_handle_t *BOARD_Codec_Wm8960Handle(void)
{
    return (wm8960_handle_t *)((uintptr_t)codecHandle.codecDevHandle);
}

/* volume is the UAC volume control value: signed dB in 1/256 steps. */
void BOARD_Codec_SetVolume(int16_t volume)
{
    int32_t db;

    if (volume == USB_AUDIO_VOLUME_SILENCE)
    {
        (void)WM8960_SetVolume(BOARD_Codec_Wm8960Handle(), kWM8960_ModuleHP, 0U);
        return;
    }

    db = ((int32_t)volume + 128) >> 8; /* round to the nearest whole dB */
    if (db < WM8960_HP_VOL_MIN_DB)
    {
        db = WM8960_HP_VOL_MIN_DB;
    }
    else if (db > WM8960_HP_VOL_MAX_DB)
    {
        db = WM8960_HP_VOL_MAX_DB;
    }
    else
    {
        /* in range */
    }

    (void)WM8960_SetVolume(BOARD_Codec_Wm8960Handle(), kWM8960_ModuleHP,
                           (uint32_t)(WM8960_HP_VOL_0DB + db));
}

void BOARD_Codec_SetMute(bool mute)
{
    (void)WM8960_SetMute(BOARD_Codec_Wm8960Handle(), kWM8960_ModuleDAC, mute);
}

void BOARD_Codec_Init()
{
    if (CODEC_Init(&codecHandle, &boardCodecConfig) != kStatus_Success)
    {
        assert(false);
    }

    /* Start from the descriptor's default so the codec and the host agree. */
    BOARD_Codec_SetVolume((int16_t)((uint16_t)g_UsbDeviceAudioSpeaker.curVolume20[1] << 8U |
                                    (uint16_t)g_UsbDeviceAudioSpeaker.curVolume20[0]));
    BOARD_Codec_SetMute(g_UsbDeviceAudioSpeaker.curMute20 != 0U);
}

static void txCallback(hal_audio_handle_t handle, hal_audio_status_t completionStatus, void *callbackParam)
{
    uint32_t audioSpeakerPreReadDataCount = 0U;
    uint32_t preAudioSendCount            = 0U;
    hal_audio_transfer_t xfer             = {0};

    if ((USB_AudioSpeakerBufferSpaceUsed() < (g_UsbDeviceAudioSpeaker.audioPlayTransferSize)) &&
        (g_UsbDeviceAudioSpeaker.startPlayFlag == 1U))
    {
        g_UsbDeviceAudioSpeaker.startPlayFlag          = 0;
        g_UsbDeviceAudioSpeaker.speakerDetachOrNoInput = 1;
    }
    if (0U != g_UsbDeviceAudioSpeaker.startPlayFlag)
    {
#if defined(USB_DEVICE_AUDIO_USE_SYNC_MODE) && (USB_DEVICE_AUDIO_USE_SYNC_MODE > 0U)
#else
        USB_DeviceCalculateFeedback();
#endif
        xfer.dataSize = g_UsbDeviceAudioSpeaker.audioPlayTransferSize;
        /* Hand this chunk to the convolver rather than to the DMA. Everything else in
         * this function -- the counters the asynchronous feedback loop is built on --
         * is untouched, so the USB side still sees data being consumed at exactly the
         * same rate. tdReadNumberPlay never straddles the end of the ring, so a single
         * contiguous read is safe. */
        RCS_PushInput(audioPlayDataBuff + g_UsbDeviceAudioSpeaker.tdReadNumberPlay,
                      xfer.dataSize);
        preAudioSendCount = g_UsbDeviceAudioSpeaker.audioSendCount[0];
        g_UsbDeviceAudioSpeaker.audioSendCount[0] += g_UsbDeviceAudioSpeaker.audioPlayTransferSize;
        if (preAudioSendCount > g_UsbDeviceAudioSpeaker.audioSendCount[0])
        {
            g_UsbDeviceAudioSpeaker.audioSendCount[1] += 1U;
        }
        g_UsbDeviceAudioSpeaker.audioSendTimes++;
        g_UsbDeviceAudioSpeaker.tdReadNumberPlay += g_UsbDeviceAudioSpeaker.audioPlayTransferSize;
        if (g_UsbDeviceAudioSpeaker.tdReadNumberPlay >= g_UsbDeviceAudioSpeaker.audioPlayBufferSize)
        {
            g_UsbDeviceAudioSpeaker.tdReadNumberPlay = 0;
        }
        audioSpeakerPreReadDataCount = g_UsbDeviceAudioSpeaker.audioSpeakerReadDataCount[0];
        g_UsbDeviceAudioSpeaker.audioSpeakerReadDataCount[0] += g_UsbDeviceAudioSpeaker.audioPlayTransferSize;
        if (audioSpeakerPreReadDataCount > g_UsbDeviceAudioSpeaker.audioSpeakerReadDataCount[0])
        {
            g_UsbDeviceAudioSpeaker.audioSpeakerReadDataCount[1] += 1U;
        }
    }
    else
    {
        if (0U != g_UsbDeviceAudioSpeaker.audioPlayTransferSize)
        {
            xfer.dataSize = g_UsbDeviceAudioSpeaker.audioPlayTransferSize;
        }
        else
        {
            xfer.dataSize = AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME / 8U;
        }
        /* Not streaming: keep clocking silence into the convolver so the filter tail
         * rings out properly instead of being chopped off. */
        RCS_PushSilence(xfer.dataSize);
    }

    /* Whatever happened above, the DMA is always served from the processed FIFO. */
    audioPlayStageIdx ^= 1U;
    (void)RCS_PopOutput(audioPlayStageBuff[audioPlayStageIdx], xfer.dataSize);
    xfer.data = audioPlayStageBuff[audioPlayStageIdx];

    HAL_AudioTransferSendNonBlocking((hal_audio_handle_t)&audioTxHandle[0], &xfer);
}

void AUDIO_DMA_EDMA_Start()
{
    usb_echo("Init Audio SAI and CODEC\r\n");
    hal_audio_transfer_t xfer = {0};
    memset(audioPlayDMATempBuff, 0, AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME);
    xfer.dataSize = AUDIO_PLAY_BUFFER_SIZE_ONE_FRAME / 8U;
    xfer.data     = audioPlayDMATempBuff;
    HAL_AudioTxInstallCallback((hal_audio_handle_t)&audioTxHandle[0], txCallback, NULL);
    HAL_AudioTransferSendNonBlocking((hal_audio_handle_t)&audioTxHandle[0], &xfer);
}

void USB_OTG1_IRQHandler(void)
{
    USB_DeviceEhciIsrFunction(g_UsbDeviceAudioSpeaker.deviceHandle);
}

void USB_OTG2_IRQHandler(void)
{
    USB_DeviceEhciIsrFunction(g_UsbDeviceAudioSpeaker.deviceHandle);
}

void USB_DeviceClockInit(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };

    if (CONTROLLER_ID == kUSB_ControllerEhci0)
    {
        CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
        CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, 480000000U);
    }
    else
    {
        CLOCK_EnableUsbhs1PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
        CLOCK_EnableUsbhs1Clock(kCLOCK_Usb480M, 480000000U);
    }
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}
void USB_DeviceIsrEnable(void)
{
    uint8_t irqNumber;

    uint8_t usbDeviceEhciIrq[] = USBHS_IRQS;
    irqNumber                  = usbDeviceEhciIrq[CONTROLLER_ID - kUSB_ControllerEhci0];

    /* Install isr, set priority, and enable IRQ. */
    NVIC_SetPriority((IRQn_Type)irqNumber, USB_DEVICE_INTERRUPT_PRIORITY);
    EnableIRQ((IRQn_Type)irqNumber);
}
#if USB_DEVICE_CONFIG_USE_TASK
void USB_DeviceTaskFn(void *deviceHandle)
{
    USB_DeviceEhciTaskFunction(deviceHandle);
}
#endif
/*${function:end}*/
