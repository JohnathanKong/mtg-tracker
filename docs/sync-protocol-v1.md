# MTG1 USB deck-profile sync protocol (v1)

This protocol is shared with [MTG Tracker Desktop](https://github.com/JohnathanKong/mtg-tracker-desktop).

The ESP32-S3 receiver is **native ESP-IDF C++**, not Arduino/PlatformIO.
Serial is exposed as a Windows COM port over the ESP32-S3's **native USB Serial/JTAG CDC** connection. The desktop configures the virtual COM port at 115200 baud for compatibility; actual USB throughput is not UART clocked.
USB-C orientation selects the connected processor: the **ESP32-S3** uses native USB Serial/JTAG, while the other orientation connects the secondary classic ESP32 through a USB-UART bridge. The live USB receiver still requires hardware validation.

## Messages

All ASCII commands and replies end with LF. Lines are at most 95 bytes.
The raw chunk body is **not line terminated**.

| Desktop sends | Firmware replies |
|---|---|
| `MTG1 HELLO\n` | `MTG1 READY 1\n` |
| `MTG1 BEGIN <length> <CRC32HEX>\n` | `MTG1 OK\n` |
| `MTG1 CHUNK <n>\n` then exactly n binary bytes | `MTG1 NEXT <total_received>\n` |
| Repeat until all bytes sent | Repeat acknowledgements |
| `MTG1 COMMIT\n` | `MTG1 STORED <deck_count> <CRC32HEX>\n` |
| `MTG1 STATUS\n` | `MTG1 STATUS <count>\n` |

Errors: `MTG1 ERROR <reason>\n`.

- Transfer payload length: 1–65536 UTF-8 bytes.
- Chunk length: 1–256 bytes; the desktop **waits for every acknowledgement**.
- CRC: standard IEEE CRC-32 (polynomial 0xEDB88320, initial 0xFFFFFFFF, final XOR 0xFFFFFFFF), 8 uppercase hexadecimal digits.
- A disconnection or incomplete transfer is **not successful**, regardless of progress bar.
- The firmware uses a 15-second receive timeout.

## JSON document

```json
{
  "version": 1,
  "decks": [
    {
      "id": "deck-123",
      "name": "Krenko Goblins",
      "commander": "Krenko, Mob Boss",
      "colors": ["R"],
      "trackers": ["life", "poison", "custom-abc"],
      "customTrackers": [
        {"id": "custom-abc", "label": "Goblins created"}
      ]
    }
  ]
}
```

Maximum 100 decks. Deck name, ID, commander name, tracker IDs and custom labels
are limited to 128 UTF-8 bytes. Colors use W/U/B/R/G.
The desktop intentionally **does not transfer images, deck-list URLs, or gameplay state**.

## Persistent storage

The receiver mounts ESP-IDF **SPIFFS** partition `storage` at `/mtg`.
It stages complete transfers to `/mtg/decks.tmp`, verifies JSON and CRC,
then promotes the validated document to `decks.json`, retaining
`decks.bak` as a recovery copy.

**Migration warning:** The earlier, never-hardware-verified Arduino prototype
used LittleFS. That on-flash format is incompatible with SPIFFS; initial
mount may format the `storage` partition and lose any prior device deck
library. Desktop copies of decks are unaffected. Do not depend on existing
on-device storage surviving the first ESP-IDF flash.

The desktop builds and syncs separately. This firmware is a headless
receiver: no touchscreen, rotary encoder, artwork or game UI yet.

## Building, flashing and monitoring

See the project [README](../README.md). Use the existing VS Code ESP-IDF
extension terminal, `build.bat`, **Flash Your Project**, **Monitor Device**.
Do **not** use GC8 paths or flash its unrelated firmware.
