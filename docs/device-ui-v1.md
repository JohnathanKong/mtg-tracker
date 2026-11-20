# Device UI v0.1 — Always Track

Firmware source: \`main/mtg_display.cpp\`, \`main/mtg_game.cpp\`.
The dashboard uses an original image-generated fantasy HUD skin embedded in the
firmware (\`main/ui_skin.lz4\`) instead of hand-drawn placeholder panels. It is
layered over **your own commander image** from MTG Tracker Desktop.

## On-device screen

The round 360×360 LCD always shows these game-long counters:

| Counter | Starting value | Notes |
| --- | --- | --- |
| Life | 40 | Can go below zero, for display/reference |
| Poison | 0 | Game-long |
| Rad | 0 | Game-long |
| Commander damage | 0 | Currently one aggregate slot; *per-opponent/per-commander breakdown is a future feature* |
| Commander casts | 0 | Tax displayed as **+2 × previous casts**, meaning the extra cost for your next cast |
| Commander deaths | 0 | Game-long |

The values are **per deck**, independently saved in NVS and restored after
reboot. A deck-library sync will not zero values, and each deck has a separate
saved game session. No automatic turn-based resets on this screen.

## Interaction

- **Tap a counter** to select it (its value turns gold).
- **Rotate the physical knob** clockwise to increase or counterclockwise to
  decrease the selected value. The board's GPIO8/GPIO7 produce distinct
  direction pulses, *not* quadrature encoder A/B.
- **Tap the top crown** (top 54 pixels) to open **Select Deck**.
- **Rotate the knob** on Select Deck to move through the stored deck library.
- **Tap the center** to load the highlighted deck and its saved counters.
- **Tap any counter** to go back to editing that counter.
- Deck selection is also possible with **touch**, via the onboard CST816D
  (SDA GPIO11, SCL GPIO12, I2C 0x15; touch coordinates rotate 180°).

Touch/knob behavior is written but **not hardware-verified**; incorrect
physical orientation or inverted rotation should be reported, not assumed.

**New Game / Reset** is intentionally not a destructive touch gesture in v0.1.
We will add an explicit confirmation screen before implementing it.

## Artwork and graphical assets

When you import artwork on the desktop and sync:

1. The desktop loads each deck's saved image with Qt.
2. It center-crops and scales to **180×180 RGB565** (64,800 bytes).
3. It sends the image separately over the MTG1 serial connection in
   acknowledged 256-byte chunks with a whole-file CRC-32.
4. Firmware stores it in SPIFFS with a stable deck-ID-derived filename, and
   keeps the previous image during a failed transfer.
5. The dashboard displays that art behind the decorative fantasy HUD asset.
   Dark overlays preserve the readability of all counters.

Image transfer intentionally happens **after** the deck metadata.
Old desktop builds will still sync deck names/counters but **not** the image.
Use the newest desktop build. The status bar will report verified artwork.
A missing photo falls back to the original dark fantasy background.

The bundled HUD asset is an original generated design that was compressed
to a 120×120 indexed image with LZ4 to keep the firmware small. It uses no
network images and needs no runtime download.

## Pending follow-up milestones

- Accurate per-opponent commander damage (not one aggregate slot).
- Deck-specific permanent mechanics (e.g., experience counters), then
  turn-limited mechanics (e.g., storm) with explicit new-turn reset rules.
- Multi-player mode, distinct player life totals, turn tracking, game reset.
- More refined touchscreen interactions and polished font/icon visibility.

## Verification

GitHub Actions compiles both firmware and desktop, but it does not simulate
USB transfer, touchscreen, NVS, or the physical LCD. After flashing this
version, verify the screen displays a 40-life dashboard, tap/knob changes,
deck selection, saving after restart, and artwork transfer in that order.
