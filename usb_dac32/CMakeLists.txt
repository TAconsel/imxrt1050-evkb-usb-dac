# USB Audio Class 2.0 speaker, 48 kHz / 32-bit / 2 ch, for the MIMXRT1050-EVKB.
#
# Out-of-tree copy of the SDK's usb_examples/usb_device_audio_speaker/bm, including its
# board port (board_port/), so the format change lives here and not in the SDK tree.
#
# PROJECT_BOARD_PORT_PATH is given as an ABSOLUTE path: mcux.cmake joins it onto
# SdkRootDirPath with cmake_path(APPEND), and an absolute input replaces the base.
cmake_minimum_required(VERSION 3.22.0)

include("${SdkRootDirPath}/cmake/extension/mcux.cmake")

project(
  usb_dac32
  LANGUAGES "ASM" "C" "CXX"
  PROJECT_BOARD_PORT_PATH "${CMAKE_CURRENT_LIST_DIR}/board_port"
)

include("${SdkRootDirPath}/CMakeLists.txt")

mcux_add_macro(
  CC "-DUSB_STACK_BM -DHAL_AUDIO_ISR_PRIORITY=0 -DUSING_SAI"
)

# The audio buffers must not sit in cacheable memory for the non-SDRAM targets.
mcux_add_macro(
  CC "-DDATA_SECTION_IS_CACHEABLE=0"
  TARGETS  "debug" "release" "flexspi_nor_debug" "flexspi_nor_release"
  TOOLCHAINS "armgcc" "iar" "mdk"
)
mcux_remove_macro(
  CC "DATA_SECTION_IS_CACHEABLE=1"
)
mcux_add_macro(
  CC "-DUSB_DEVICE_CONFIG_BUFFER_PROPERTY_CACHEABLE=1"
  TARGETS  "sdram_debug" "sdram_release"
  TOOLCHAINS "armgcc" "iar" "mdk"
)

# 32-bit frames double the DMA/USB buffers over the stock 16-bit example.
mcux_add_armgcc_configuration(
  LD "-Xlinker --defsym=__heap_size__=0x2000"
)
mcux_add_armgcc_configuration(
  LD "-Xlinker --defsym=__stack_size__=0x2000"
)

mcux_add_include(
  BASE_PATH "${CMAKE_CURRENT_LIST_DIR}"
  INCLUDES "." "board_port"
)

# The DCD in the boot header is what brings up the SEMC and the 32 MB SDRAM before
# main() runs; without it 0x80000000 is dead and the convolver has nowhere to live.
# SKIP_SYSCLK_INIT is mandatory alongside the DCD: the DCD has already set System PLL
# (PLL2) and its PFDs to clock the SEMC, and letting BOARD_BootClockRUN() re-init them
# would pull the clock out from under the SDRAM that is already running. It also makes
# BOARD_ConfigMPU() mark 0x80000000 as Normal cacheable write-back, which the convolver
# needs -- streaming the delay line through uncached Device memory would be hopeless.
mcux_add_macro(
  CC "-DXIP_BOOT_HEADER_DCD_ENABLE=1 -DSKIP_SYSCLK_INIT"
)
mcux_add_source(
  BASE_PATH "${SdkRootDirPath}"
  SOURCES "${board_root}/${board}/dcd.c"
)

mcux_add_source(
  BASE_PATH "${CMAKE_CURRENT_LIST_DIR}"
  SOURCES
    "roomcorr.c"
    "roomcorr.h"
    "roomcorr_data.c"
    "roomcorr_params.h"
    "roomcorr_stream.c"
    "roomcorr_stream.h"
    "roomcorr_ui.c"
    "roomcorr_ui.h"
    "audio_speaker.c"
    "audio_speaker.h"
    "usb_device_descriptor.c"
    "usb_device_descriptor.h"
    "board_port/app.h"
    "board_port/hardware_init.c"
    "board_port/pin_mux.c"
    "board_port/pin_mux.h"
    "board_port/usb_audio_config.h"
)

mcux_convert_binary(
  BINARY "${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin"
)
