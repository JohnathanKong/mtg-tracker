#!/usr/bin/env sh
# Create one ESP32-S3 factory image for browser-based initial flashing.
# Designed to run INSIDE the Docker/PlatformIO build environment only.
set -eu

cd /src
PIO_DIR=".pio/build/waveshare-knob-s3"
TOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
OUTPUT="build/MTG-Tracker-ESP32S3-full-flash.bin"

for part in "$PIO_DIR/bootloader.bin" "$PIO_DIR/partitions.bin" "$PIO_DIR/firmware.bin" "$BOOT_APP" "$TOOL"; do
    if [ ! -s "$part" ]; then
        echo "Missing required flashing component: $part" >&2
        exit 1
    fi
done

mkdir -p build
# ESP32-S3 bootloader is at 0x0, not the ESP32 classic boot offset.
# 0xe000 is OTA selection; including boot_app0 makes initial install
# deterministic on units that previously ran other firmware.
python "$TOOL" --chip esp32s3 merge_bin \
    --flash_size 16MB \
    -o "$OUTPUT" \
    0x0 "$PIO_DIR/bootloader.bin" \
    0x8000 "$PIO_DIR/partitions.bin" \
    0xe000 "$BOOT_APP" \
    0x10000 "$PIO_DIR/firmware.bin"

test -s "$OUTPUT"
printf "Combined browser flash image: %s\n" "$OUTPUT"
printf "Use one file at offset 0x0; ONLY for ESP32-S3.\n"
