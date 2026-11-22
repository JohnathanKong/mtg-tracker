# Device UI v0.2 — Portrait / Always Track

This is the selected design for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8
(360 × 360 circular screen), superseding the segmented-wheel UI.

## Layout

- **Left half:** the user's *own* synced commander image, center-cropped and
  feathered at the edges, with a large **LIFE 40** overlay in the lower-left.
- **Right column, always visible:** **Poison**, **Rad**, **Commander Damage**
  and **Commander Deaths**, each with a separate icon, label and dynamic number.
- **Bottom plate:** an image-generated chrome panel labelled **COMMANDER**,
  with a stacked-card icon and dynamic **CASTS** count on the left, and gold
  chevrons with dynamic **TAX +n** on the right. Tax is calculated from the
  number of commander casts (two additional mana per prior cast).
- The graphic is a native **360 × 360** indexed, LZ4-compressed image asset:
  `main/ui_portrait_hud_360.lz4`. It contains the border, four empty
  counter capsules, four icons, and the casts/tax plate. **No pre-rendered
  numbers, labels or commander portrait are baked into it.** All current
  values and captions are drawn live by the firmware using measured GFX font
  metrics; the graphic remains visually stable while values change.
- **You do not select a background on the ESP32-S3.** The portrait uses the
  artwork saved with the active deck in MTG Tracker Desktop. The illustrative
  character in design previews is only sample art; it is not shipped in the
  firmware.
- If that deck has no valid 360×360 image stored on the device, the portrait
  shows a dark smoky blue fallback with **SYNC ART**. In the desktop app,
  open the deck editor, confirm its imported artwork is visible, save the deck,
  then open **Device Synchronization → Sync Deck Library**. A successful
  transfer reports the **number of commander images actually stored**, so a
  message saying `0 commander images` means nothing was uploaded.
- Right-side numeric values and casts/tax labels are inset from both their
  graphic panels and the physical round edge, with actual font bounds used
  for positioning. Larger numbers automatically use a smaller font.

## Interaction

- Tap **LIFE**, or any one of the four list rows, to select the editable
  counter; turn the physical knob to increment or decrement its value.
- Tap either half of the **COMMANDER** bottom plate to select **CASTS**.
  **TAX** itself is calculated, not independently editable.
- Tap the **upper-left commander portrait** to open the deck selector.
  Rotate the knob to browse decks; tap to select.
- Hold the **LIFE** area for about 2 seconds to show the **New Game?**
  confirmation. Only tapping **RESET** resets the saved game to 40 life and
  zeroes the other five counters. Tap **CANCEL** to dismiss.
- Values remain saved independently per deck in NVS; transferring the deck
  library does not reset current game progress.

## Implementation / limits

- `main/mtg_display.cpp` blends the transferred **360 × 360 RGB565**
  commander art with the native HUD asset and centers dynamic text from
  measured font pixel bounds inside fixed, non-overlapping panel rectangles.
- `main/mtg_game.cpp` implements the portrait/list/footer touchscreen hit
  targets and per-deck saved counter state.
- The screen displays one **aggregate** commander damage value. Tracking
  individual opposing commanders, deck mechanics and temporary turn counters
  will be handled in later pages; those features aren't yet implemented.
- The existing working MTG1 serial synchronization protocol and saved deck
  library are unchanged by this UI-only update.

To deploy, use VS Code's **ESP-IDF terminal** in this repo: `git pull`,
`./build.bat` (or `.\build.bat` in PowerShell), then **ESP-IDF: Flash Your
Project** selecting the **ESP32-S3** serial port. Flashing normally preserves
stored art and NVS game state. If the left artwork area is empty because a
previous transfer failed, use the up-to-date desktop app to sync your deck
again.

This is a firmware implementation, not yet a verified photograph
of the physical LCD. Confirm touch alignment and visual readability on
the actual device before calling the layout final.
