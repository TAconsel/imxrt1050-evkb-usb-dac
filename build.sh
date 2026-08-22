#!/usr/bin/env bash
# Build (and optionally flash) firmware for the MIMXRT1050-EVKB.
#
#   ./build.sh                    build the default app (blinky)
#   ./build.sh blinky flash       build + flash + reset
#   ./build.sh usb_dac flash      USB Audio Class 2.0 speaker, 48k/16 -> WM8960 -> J12
#   ./build.sh usb_dac32 flash    same, 48 kHz / 32-bit USB (WM8960 uses the top 24)
#
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh

APP=${1:-blinky}
ACTION=${2:-}
CFG=${CFG:-flexspi_nor_debug}          # XIP from the on-board HyperFlash

case "$APP" in
  blinky)  SRC=/home/consel/IMXRT1050-DSP/blinky ;;
  usb_dac)   SRC=$MCUX/examples/usb_examples/usb_device_audio_speaker/bm ;;
  usb_dac32) SRC=/home/consel/IMXRT1050-DSP/usb_dac32 ;;
  *)       SRC=$APP ;;                 # or pass an absolute path to any app dir
esac
OUT=/home/consel/IMXRT1050-DSP/$APP/build

(cd "$MCUX" && west build -b evkbimxrt1050 "$SRC" \
     --toolchain armgcc --config "$CFG" -d "$OUT")

if [ "$ACTION" = flash ]; then
    ELF=$(ls "$OUT"/*.elf | head -1)
    pyocd flash -t mimxrt1050 -e sector "$ELF"
    pyocd reset -t mimxrt1050
fi
