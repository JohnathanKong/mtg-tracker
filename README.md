# MTG Tracker firmware

Target: Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (ESP32-S3 processor).

Current milestone: USB serial deck sync receiver, **not yet a touchscreen UI**.

- Reads and validates compact deck metadata, including named custom trackers.
- Uses a versioned protocol, CRC-32 and acknowledgement for each block.
- Saves to internal LittleFS, preserving the last good deck library on failure.
- No Wi-Fi provisioning, gameplay tracking, deck artwork, or remote sync yet.

**Flashing will overwrite the original factory Waveshare firmware.** Do not
flash until you are ready to replace the original on-device UI. Keep the
Waveshare factory BIN/recovery instructions.

The USB-C port can address either the ESP32-S3 or the secondary ESP32,
depending on plug orientation. If the expected COM port is missing,
disconnect, flip the plug 180 degrees and reconnect.

## Build without host installations

Docker from project root:

    docker build -t mtg-tracker-firmware .
    docker run --rm -v "\${PWD}:/src" -w /src mtg-tracker-firmware

Output: build/firmware.bin. Building does not flash anything.

The desktop is separately developed at:
https://github.com/JohnathanKong/mtg-tracker-desktop

See docs/sync-protocol-v1.md for USB wire protocol and schema.
