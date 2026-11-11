# MTG Tracker firmware

Target: Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (**ESP32-S3 processor**, not the secondary ESP32).

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
    docker run --rm -v "${PWD}:/src" -w /src mtg-tracker-firmware

Build outputs:
- `build/MTG-Tracker-ESP32S3-full-flash.bin` — **complete factory image**, easiest for first-time installation via a browser at flash address `0x0`.
- `build/firmware.bin` — application **only**, not suitable for flashing at `0x0`.

Building does not flash anything.

## Flash from Windows (no development SDK installs)

1. Build using Docker above, or download and extract the latest successful GitHub Actions firmware artifact.
2. Connect the knob using a data-capable USB-C cable; make sure the port is wired to its **ESP32-S3**. Reversing the USB-C plug selects the secondary chip.
3. Open [Espressif's official ESP Tool](https://espressif.github.io/esptool-js/) in **Chrome or Edge** (Web Serial required).
4. Choose **Connect**, select the knob's new serial COM port, and verify the detected chip is **ESP32-S3**. If it reports ESP32 without S3, disconnect and reverse the USB-C connector. If the chip cannot enter download mode, hold its ESP32-S3 BOOT button while powering up.
5. In **Program**, select `MTG-Tracker-ESP32S3-full-flash.bin`, with starting flash address **`0x0`** (zero). This is a combined image, so no other files or addresses are required.
6. Choose 16MB flash size (the board has 16MB), leave flash erase-all disabled unless recovering from an incompatible image, and **Program/Flash**.
7. Disconnect the browser serial console and power-cycle the knob. The display may remain blank: this firmware is **only the headless receiver**, not our MTG screen.
8. Open MTG Tracker Desktop > Device Sync, refresh ports, select its COM port, and sync deck profiles. Success requires the device to acknowledge the saved CRC.

Only perform the flash when you are ready to replace the Waveshare factory UI.
Waveshare supplies a downloadable original factory BIN on its wiki if needed.
Do not flash an ordinary `firmware.bin` at `0x0`. Do not use the ESP32
(non-S3) firmware or port. No host Python/PlatformIO/esptool installation needed.

**Serial routing:** The device exposes the onboard USB-to-UART bridge through
its reversible USB-C connector; our firmware uses ESP32-S3 UART0 (`Serial`,
native USB CDC disabled). This must be confirmed on the physical board. 

## Recovery and verification

The vendor's [Waveshare flashing instructions](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)
describe holding the **ESP32-S3 BOOT** button, reconnecting power to enter
download mode, and restoring their original BIN. If the factory USB interface
is unreliable after flashing, use the button to re-enter the ESP32-S3 ROM
bootloader and retry.

If the browser flasher says the chip is not ESP32-S3, **do not program it**.
The separate ESP32 processor has its own firmware.



The desktop is separately developed at:
https://github.com/JohnathanKong/mtg-tracker-desktop

See docs/sync-protocol-v1.md for USB wire protocol and schema.
