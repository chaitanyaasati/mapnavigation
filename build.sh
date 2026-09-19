#!/bin/sh
# Build / flash helper. Usage: ./build.sh [maps|hw_probe] [upload]
set -e
cd "$(dirname "$0")"
SKETCH=${1:-maps}
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,DebugLevel=error"
PORT=${PORT:-/dev/cu.usbmodem1101}
# -O2 instead of the default -Os: the rasteriser is hot code and flash is plentiful
arduino-cli compile --fqbn "$FQBN" --warnings default --build-property "compiler.optimization_flags=-O2" "$SKETCH"
if [ "$2" = "upload" ]; then
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"
fi
