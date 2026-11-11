# USB sync protocol v1

Connection: 115200 baud, USB serial to ESP32-S3. ASCII commands end in LF.
Maximum line length is 96 bytes. Data chunks are raw bytes, not line terminated.

Desktop sends / device replies:

    MTG1 HELLO\n                     MTG1 READY 1\n
    MTG1 BEGIN <bytes> <CRC32HEX>\n  MTG1 OK\n
    MTG1 CHUNK <n>\n + n raw bytes    MTG1 NEXT <total_received>\n
    ...repeat...
    MTG1 COMMIT\n                    MTG1 STORED <count> <CRC32HEX>\n

Other commands: MTG1 STATUS\n -> MTG1 STATUS <count>\n.
Errors: MTG1 ERROR <reason>\n.
Each chunk is limited to 256 bytes; the sender waits for NEXT before continuing.
Valid payload length 1–65536 bytes.
CRC32 is IEEE polynomial 0xEDB88320, initial and final XOR 0xFFFFFFFF,
eight uppercase hexadecimal digits, computed over the exact UTF-8 JSON bytes.
A missing final STORED reply means failure, never success.

## Payload schema

Example compact UTF-8 JSON:

    {"version":1,"decks":[{"id":"abc","name":"Krenko Goblins","commander":"Krenko, Mob Boss","colors":["R"],"trackers":["life","poison","custom-abc"],"customTrackers":[{"id":"custom-abc","label":"Goblins created"}]}]}

Up to 100 decks; UTF-8 deck and commander names and custom labels limited to
128 bytes each. Empty deck array is valid and intentionally removes all decks.

Do not send image paths, image data or deck-list URLs. Those remain on the PC
until a later artwork-sync protocol is introduced.

## Safety/persistence

Device receives into /decks.tmp in flash LittleFS, checks total size,
CRC-32, JSON schema and declared count, then promotes the temp file to
/decks.json with /decks.bak as a recovery copy. Interrupted or invalid
transfers leave the old profile library available. No saved profiles
are removed before the complete transfer is validated.

## Hardware

Waveshare has dual processors; USB-C plug orientation selects the connected
processor. Target ESP32-S3 and enable USB CDC. Flashing the receiver firmware
overwrites the factory UI; the display/knob are not configured in milestone 1.
