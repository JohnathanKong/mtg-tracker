# MTG Tracker — ESP32-S3 firmware

Firmware for the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8).
The desktop deck manager is a [separate repository](https://github.com/JohnathanKong/mtg-tracker-desktop).

**This repository is a native ESP-IDF v6.1 / C++ project, using the exact same VS Code development workflow as the GC8 gauge.** No PlatformIO, Arduino IDE, Docker, browser flasher, or manually invoked Python/esptool commands are needed for everyday firmware development.

> **Current milestone:** The firmware displays a live MTG Tracker status interface on the **360×360 round screen** and receives/stores deck profiles. It shows boot, ready, receiving progress, success/failure and saved deck count. The touchscreen, rotary knob and actual gameplay counters are not implemented yet. Flashing **replaces the existing Waveshare factory firmware** on the S3. Save the vendor's [factory firmware/recovery instructions](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8) before flashing.

## Windows 11 / VS Code — everyday build

Prerequisites: VS Code with the **Espressif IDF** extension already configured for **ESP-IDF v6.1**, and this repo cloned. You can reuse the very same ESP-IDF installation you use for your GC8 gauge. The MTG knob is a **different board/project**; nothing in the GC8 repository needs to be changed.

1. Open this folder in VS Code: `C:\Users\User\Projects\mtg-tracker` (adjust if you cloned elsewhere).
2. Press **Ctrl+Shift+P** and choose **ESP-IDF: Set Espressif Device Target** → **esp32s3** (NOT plain `esp32`). The VS Code ESP-IDF status bar should display **esp32s3**.
3. Open a **new** terminal via **Ctrl+Shift+P → ESP-IDF: Open ESP-IDF Terminal**. **Do not use ordinary PowerShell or Git Bash**: they will not have the ESP-IDF environment configured.
4. In that new ESP-IDF terminal, run **`build.bat`** (or `.\build.bat` if it is a PowerShell terminal).
5. Wait for the build to finish. **Do not flash if the build fails.** Copy the **first actual error** and last 20–30 output lines if troubleshooting is needed.

After a successful build, connect the MTG Waveshare by USB-C, select the correct **ESP32-S3** COM port and **UART** flashing method in the ESP-IDF extension, then run **ESP-IDF: Flash Your Project**. After flashing, run **ESP-IDF: Monitor Device** to inspect boot logs. Exit the monitor with **Ctrl+]**.

**That's the entire workflow: select `esp32s3` → open ESP-IDF terminal → `build.bat` → Flash Your Project → Monitor Device.**

**You do not need the physical device connected to build firmware.** Device detection and the COM-port choice are only needed for **flashing and monitoring after a successful build**.

You do not need to build a separate `.bin` with Docker or run Espressif's browser-based flashing tool. The ESP-IDF extension uses the app, partition table and bootloader from **this project's own ESP-IDF build**.

### First build after switching from the old PlatformIO prototype

Do this **once**, before building the converted project:

1. Stop running builds and serial monitors.
2. Run `git pull` in the MTG project folder.
3. If the project root contains `sdkconfig` from an unrelated ESP-IDF setup, rename it to `sdkconfig.backup`. (The previous PlatformIO version normally did not create one.)
4. **Delete only the generated root `build` directory from the old firmware build**, if present. ESP-IDF will refuse to clean an existing PlatformIO build directory because it is **not an ESP-IDF/CMake build directory**. From PowerShell **in the MTG repository root**, run:

   ```powershell
   Remove-Item -Recurse -Force .\build
   ```

   This deletes **only old generated compilation output**, not source code or desktop deck profiles. If the folder is already gone, skip this command.
5. In VS Code, run **ESP-IDF: Set Espressif Device Target → esp32s3**. Close old terminals and open a **fresh ESP-IDF terminal**.
6. Run `build.bat` (or `.\build.bat`). If `sdkconfig` does not exist, the script selects **esp32s3**, loads `sdkconfig.defaults`, and generates a new configuration. **The target in the VS Code environment must also match.**

This is comparable to the first-build cleanup you already did for the GC8 gauge. After this, you only need `git pull` and `build.bat` for normal development.

## Fix: `esp32s3` target conflicts with `esp32` in the environment

The first ESP-IDF build can stop with:

```text
Target 'esp32s3' specified on command line is not consistent with target 'esp32' in the environment.
```

This means the **VS Code/ESP-IDF environment is targeting the wrong chip**. It does **not** mean the USB device wasn't detected: the build has not reached flashing yet.

1. In VS Code press **Ctrl+Shift+P** → **ESP-IDF: Set Espressif Device Target** → choose **esp32s3**. Check that the ESP-IDF status bar says `esp32s3`, not `esp32`.
2. **Close the old ESP-IDF terminal**, and open a fresh one using **ESP-IDF: Open ESP-IDF Terminal**.
3. Run `.\build.bat` again.

If the same mismatch persists, in the **new ESP-IDF PowerShell terminal**, inspect the target and set it for that terminal session before rebuilding:

```powershell
$env:IDF_TARGET
$env:IDF_TARGET = "esp32s3"
.\build.bat
```

This environment variable change is **temporary and only affects that terminal**. Use the VS Code device-target selector for the persistent project choice. Do not change your GC8 files or globally change the target.

**The device does not need to be connected for this step.** After the build succeeds, connect the MTG knob and select its COM port for flashing.

## Connect the correct processor before flashing

This Waveshare knob has **two processors**: **ESP32-S3R8** (our MTG firmware target) and **ESP32-U4WDH** (a separate ESP32). Waveshare routes its reversible USB-C connector to different download channels depending on insertion orientation.

1. Use a **data-capable USB-C cable**.
2. Find the COM port under Windows **Device Manager → Ports (COM & LPT)**.
3. In the ESP-IDF VS Code extension choose the MTG COM port and target **esp32s3**, **UART** flashing mode.
4. If you reach the wrong processor or flashing fails, disconnect USB, rotate the USB-C plug **180°**, reconnect, and confirm **ESP32-S3**. If required, use the **ESP32-S3 BOOT** button while connecting power to enter download mode.

**Never flash unless the connected chip is ESP32-S3.** Disconnect your GC8 gauge or other ESP32 boards while setting up the port to avoid accidentally selecting the wrong device. Flashing replaces the S3's existing program.

## After flashing: synchronize from the desktop application

Firmware flashing and deck synchronization are separate operations.

1. Close the ESP-IDF **Monitor Device** terminal, which holds the COM port open.
2. Open **MTG Tracker Desktop → Device Sync**.
3. Click **Refresh** and select the ESP32-S3 COM port.
4. Click **Sync Deck Library**.
5. A successful transfer requires the device to acknowledge the checksum and saved profile count. The device stores deck data in flash and can be disconnected afterward.

**Transferred now:** deck and commander names, mana-color identity, selected counters, named custom trackers. **Not yet transferred:** commander images, deck-list URLs, live life totals/counters, or firmware updates.

The first hardware test may expose UART/USB routing issues; the protocol and firmware have not been verified on your physical unit yet.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| `ERROR: ESP-IDF environment is not active` | Open **ESP-IDF: Open ESP-IDF Terminal** in VS Code, not a normal shell. |
| `build` directory doesn't seem to be a CMake build directory | Run `Remove-Item -Recurse -Force .\build` **from the MTG repo root** once, then rebuild. This is stale PlatformIO output, not a device-detection error. |
| `Target 'esp32s3' ... not consistent with target 'esp32' in the environment` | Select **ESP-IDF: Set Espressif Device Target → esp32s3** and open a fresh ESP-IDF terminal. If necessary set `$env:IDF_TARGET = "esp32s3"` in that terminal before rebuilding. |
| Build shows stale files or old PlatformIO output | Delete the generated root `build` folder once and rebuild. |
| The wrong chip or COM port is selected | Disconnect other ESP32 boards; flip the knob's USB-C plug 180°; select **ESP32-S3**. |
| Flash/download mode cannot be entered | Hold the **ESP32-S3 BOOT** button while reconnecting USB as documented by Waveshare. |
| Screen is blank after flashing | **Not expected with the new status UI.** Confirm the new firmware compiled and was flashed to the S3, check Monitor Device for `mtg_display` errors, and confirm the ESP-IDF target is `esp32s3`. |
| Device Sync cannot open the COM port | Exit **Monitor Device**, any serial terminal, and anything else using that port. |
| Sync reports no reply or times out | Check that MTG firmware, not Waveshare factory software, was flashed to the **ESP32-S3**; confirm USB orientation and COM port. Hardware testing is still pending. |
| Want the factory interface back | Use Waveshare's factory firmware for **this exact knob** and its recovery instructions. |

### Storage compatibility warning

The original untested **Arduino/PlatformIO prototype used LittleFS**. This ESP-IDF conversion uses built-in **SPIFFS** in the storage partition. On first boot, a non-SPIFFS partition may be formatted, so **any deck data previously saved on the device may be lost**. Desktop deck profiles are stored separately and are not affected. Treat the first ESP-IDF flash as a migration; it is **not a guaranteed in-place upgrade**.

## Project structure

| Path | Purpose |
| --- | --- |
| `build.bat` | One-command build from an ESP-IDF terminal. |
| `CMakeLists.txt`, `main/CMakeLists.txt` | ESP-IDF application configuration. |
| `sdkconfig.defaults` | ESP32-S3, 16 MB flash, UART and partition defaults. |
| `partitions.csv` | NVS, OTA application slots and SPIFFS storage. |
| `main/mtg_receiver.cpp` | Native ESP-IDF UART receiver, JSON validation and flash persistence. |
| `main/mtg_display.cpp`, `.h` | Round 360×360 SH8601 QSPI display, PWM backlight, original text/status renderer. |
| `main/panel_init_commands.h` | SH8601 panel power-up sequence adapted from an Apache-2.0 hardware reference; see `THIRD_PARTY_NOTICES.md`. |
| `main/idf_component.yml` | ESP-IDF-managed cJSON dependency. |
| `docs/sync-protocol-v1.md` | Desktop/device USB handshake and deck JSON format. |
| `.github/workflows/build.yml` | ESP-IDF v6.1 compilation in GitHub Actions. |

**What appears on the display:** `MTG TRACKER`, `READY TO SYNC`, `DECKS STORED: 0`, and connection guidance. When data arrives it changes to `RECEIVING` with progress; then `SYNC COMPLETE` or `SYNC FAILED` with a reason. The on-device status display is designed to keep working without the desktop. It is a diagnostic UI, not yet the deck picker/gameplay interface.

**Display hardware:** 360×360 round SH8601 panel over QSPI using GPIO 13–18 and 21, with GPIO47 PWM brightness. The firmware uses PSRAM and Espressif's managed `esp_lcd_sh8601` driver, not a web browser or an additional host toolchain. The first physical LCD test is still pending.

The firmware uses the same **MTG1** deck-sync protocol as the desktop app. It validates each block, CRC-32, and JSON before replacing previously saved profiles. No Wi-Fi, NFC or additional external hardware is required for this stage.

**GC8 remains separate and untouched.** This repository's `build.bat`, firmware and flash settings apply only to your MTG knob.
