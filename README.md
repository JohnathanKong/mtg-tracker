# MTG Tracker — ESP32-S3 firmware

Firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8**, companion to [MTG Tracker Desktop](https://github.com/JohnathanKong/mtg-tracker-desktop).

> **Current status:** This is an early **USB deck-sync receiver**, not the finished MTG touchscreen/knob interface. It can receive and store deck metadata, but it does not yet display decks or run counters. Its serial connection has **not been verified on the physical board**.
>
> **Flashing replaces the factory firmware on the ESP32-S3.** Do not flash until you are comfortable losing the original display interface and have downloaded the [Waveshare factory firmware/recovery resources](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8). The other onboard ESP32 is a separate processor.

## What you need

- A Windows PC with Docker Desktop running **Linux containers**, and Git.
- A USB-C **data-capable** cable and the Waveshare knob.
- Chrome or Edge for [Espressif's browser-based ESP Tool](https://espressif.github.io/esptool-js/). No host installation of ESP-IDF, Arduino IDE, PlatformIO, Python, or esptool is necessary.
- **MTG Tracker Desktop v0.3 or later** for the separate deck-profile synchronization step.

**Deployment has three separate operations:** (1) build firmware in Docker, (2) flash firmware once using the browser, then (3) push deck profiles using the desktop application. **Building does not flash the device; syncing profiles does not update firmware.**

## 1. Build the firmware in Docker

Open **PowerShell** in your existing `mtg-tracker` repository, then run:

```powershell
git pull
docker build -t mtg-tracker-firmware .
docker run --rm -v "${PWD}:/src" -w /src mtg-tracker-firmware
```

If you have not cloned the firmware repository, first run:

```powershell
git clone git@github.com:JohnathanKong/mtg-tracker.git
cd mtg-tracker
```

The Docker container installs the compiler and dependencies **inside the container**, compiles for ESP32-S3 with PlatformIO, and merges the bootloader, partition table, OTA bootstrap, and application into one flashable image. Nothing is installed on the Windows host.

**Expected success output** looks similar to:

```text
===================== [SUCCESS] =====================
Wrote ... bytes to file build/MTG-Tracker-ESP32S3-full-flash.bin, ready to flash to offset 0x0
Combined browser flash image: build/MTG-Tracker-ESP32S3-full-flash.bin
```

Find the files here relative to the firmware repository:

| File | Purpose |
| --- | --- |
| `build/MTG-Tracker-ESP32S3-full-flash.bin` | **Use this for initial browser flashing**, at address `0x0`. Complete merged image. |
| `build/firmware.bin` | Application only. **Do not** flash at `0x0`. |
| `.pio/build/waveshare-knob-s3/` | Intermediate bootloader, partitions, ELF and other PlatformIO files. |

Alternatively, use the [firmware GitHub Actions builds](https://github.com/JohnathanKong/mtg-tracker/actions): open the latest **successful** run, download the `MTG-Tracker-ESP32S3-firmware` artifact, and extract the ZIP to find the same **full-flash** image. The CI job compiles inside Docker and verifies the merged file exists.

## 2. Flash the ESP32-S3 from Windows

**Before flashing:** the Waveshare contains an **ESP32-S3 and a separate ESP32**. The reversible USB-C connection selects different programming channels depending on insertion orientation. Never flash unless the browser positively identifies **ESP32-S3**. [Waveshare's instructions and FAQ](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8) cover the connector and BOOT procedure.

1. Connect the knob using the data-capable USB-C cable. In **Windows Device Manager → Ports (COM & LPT)**, identify its COM port. You may see a different COM number after reconnecting.
2. Open **[Espressif ESP Tool](https://espressif.github.io/esptool-js/)** in Chrome or Edge. Use the **serial port** connection. For a typical Windows COM port, leave the optional **WebUSB (CH340)** checkbox off.
3. Click **Connect** and choose the knob's COM port. **Confirm the chip is detected as ESP32-S3**. If it reports plain ESP32, fails to connect, or the wrong COM port appears, disconnect the cable, reverse the USB-C plug **180°**, and reconnect. Check the detected chip again.
4. If the S3 cannot enter download mode, disconnect USB, **hold the ESP32-S3R8 BOOT button**, reconnect power while holding it, then release BOOT. Retry Connect. This is the S3 BOOT button, not the secondary ESP32's.
5. In the **Program** section of ESP Tool, add the file `build/MTG-Tracker-ESP32S3-full-flash.bin` and set its **Flash Address to `0x0`**. This is one combined image; you do **not** need separate bootloader/partition offsets.
6. Select **16 MB** flash size (the board has 16 MB). Keep **Erase all off** for the normal procedure; erase-all removes stored data. If an incompatible old partition layout prevents booting, investigate recovery/erasure separately rather than blindly erasing.
7. Click **Program** and confirm the operation finishes successfully. Disconnect the browser from the COM port, then power-cycle the knob.

**Expected after flashing:** the display may appear blank. That is normal for this milestone because **we have not implemented the device's visual interface**. A successfully flashed image is not proof that serial syncing works; that needs a real-device test.

**Important:** The combined image writes the S3 bootloader, partition table, and application. Do not use it on the secondary ESP32, and do not flash the app-only `firmware.bin` at `0x0`.

## 3. Send your decks from MTG Tracker Desktop

After the receiver firmware is installed:

1. Close ESP Tool's serial connection (and any serial monitor) so the COM port is free.
2. Open **MTG Tracker Desktop → Device Sync**.
3. Click **Refresh**, choose the ESP32-S3 COM port, and click **Sync Deck Library**.
4. The desktop sends a handshake and acknowledged data chunks. It reports success only after the device validates the CRC and confirms the library is stored.
5. The device saves the deck profiles to its own internal flash. You can disconnect the desktop afterward; device UI/gameplay is a later milestone.

**Currently transferred:** deck name, commander name, colors, built-in tracker selections, custom tracker IDs and names. **Not transferred:** commander images, deck-list links, current life/counter values, or firmware updates. The desktop refuses an empty deck library to reduce accidental clearing.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Docker reports success but the knob did not change | Expected. **Build ≠ flash**; perform step 2. |
| No COM port or the wrong ESP32 detected | Use a data cable, reverse USB-C plug 180°, and reconnect. Confirm **ESP32-S3** before proceeding. |
| Browser cannot access serial port | Close MTG Tracker Desktop/serial monitor or another browser tab using that COM port. Use Chrome/Edge. |
| Cannot enter flash/download mode | Hold the **ESP32-S3 BOOT** button while reconnecting USB. Follow the Waveshare recovery documentation. |
| Firmware flashes but screen stays blank | Expected with the **headless receiver**; display and knob firmware is not yet implemented. |
| Device Sync times out / no READY response | Verify the S3 firmware was flashed, choose the correct COM port, close ESP Tool, and reconnect. **USB serial behavior is not yet hardware-tested**; record the COM number and error for debugging. |
| Need the original display back | Restore the Waveshare **factory firmware** for this exact board using the vendor's documented procedure. Do not use GC8 gauge or secondary-ESP32 firmware. |
| `firmware.bin` rejected or wrong boot address | Use **`MTG-Tracker-ESP32S3-full-flash.bin` at `0x0`**, not the app-only binary. |

## Updating firmware later

Repeat **Build** and **Flash** for a new firmware version. **Device Sync** only transfers data; it cannot flash a firmware update. Changing flash partitions or using erase-all can destroy the saved profile library, so do not assume it will survive a future firmware update. We can add backup/export or OTA upgrading in a later milestone.

## Technical reference

- **Target:** Waveshare ESP32-S3-Knob-Touch-LCD-1.8, 16 MB flash.
- **Build:** PlatformIO + Arduino/ESP32 in Docker (`platformio.ini`, `Dockerfile`).
- **Flash image:** `scripts/create-factory-bin.sh` merges into one file at offset `0x0`.
- **Sync transport:** 115200 baud serial; current firmware targets S3 UART0 via the onboard USB connection, **pending physical confirmation**.
- **Persistence:** LittleFS partition with staged write, CRC/schema verification, and backup.
- **Wire protocol:** [docs/sync-protocol-v1.md](docs/sync-protocol-v1.md).
- **Vendor hardware/flashing guide:** [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8).
- **Flashing software:** [Espressif ESP Tool](https://espressif.github.io/esptool-js/).

**The GC8 gauge is a separate project and is not involved in this build or deployment process.**
