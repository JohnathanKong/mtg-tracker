# Device UI v0.3 — Ornate artwork-first round dashboard

The selected device reference is a **360×360 circular screen** with a real,
synced commander artwork banner, an ornate curved life crest, a curved
Casts / Tax sash, up to four configurable lower counter modules, page dots
and one small lower-right **+** control. The older portrait-left UI is
superseded.

## Always shown

- Life is the dominant number, dial-adjustable after tapping its crest.
- Commander Casts is shown in a fixed sash, and can be selected and dial-adjusted.
- Commander Tax is derived as `+2 × commander casts`; it cannot be edited.
- The banner displays the deck's **uploaded** image (not a built-in dragon).
  The current Windows app scales the complete image to fit the banner over
  a dimmed fill derived from that image. Re-sync once after changing the
  artwork conversion algorithm.

## Optional counters

- The desktop app determines the deck's *default* optional counters.
  New decks start with zero optional counters; none are fabricated.
- Only configured or game-added counters are drawn. **Empty tile modules
  are invisible**; the gold + control remains available.
- Exactly four optional counters can be displayed per page. Horizontal
  screen swipes navigate further groups of four, while Life, Casts and
  Tax remain stationary. No page-arrow controls.
- The + control opens a paged menu of unused standard counters and the
  deck's custom trackers. Picking one adds it for this game only.
- A tap selects a numeric counter. The mechanical rotary dial is the only
  way to increase/decrease numeric values; touch never increments them.

## Rendering

- `main/ui_ornate_hud_360.lz4` contains only decorative gold curves,
  dim glass gradients and empty capsule borders. It uses LZ4-compressed
  360×360 indexed bytes plus a 32-entry RGBA palette.
- The uploaded RGB565 art, all numbers, labels, selection highlights and
  page indicators are composed live by firmware.
- Text uses four-by-four subpixel coverage sampling of larger GFX fonts
  to avoid jagged scaled-up 5×7 bitmap glyphs.
- `tests/test_hud_asset.py` checks the binary palette and decoded pixel count
  on each CI run.

## Input mapping and saved data

- CST816D I2C touch coordinates now use the native X/Y axes, rather than
  the old 180° inversion that sent bottom-right + taps to the deck selector.
- Existing per-deck Life / Casts / Poison / Rad / Damage / Deaths NVS values
  are preserved. Configurable counters and temporary additions are kept
  in a separate per-deck NVS blob.
- The temporary counter blob's format has been advanced once to discard
  accidentally created slots from the previous inverted-touch version.
  Going forward, game-added counters persist across power cycles and are
  removed when a new game is explicitly confirmed.
- Previously saved desktop profiles retain their selected counters; the
  empty default applies to newly created profiles, not automatic migration
  of an existing saved deck.

## Validation

Source-level tests and firmware compilation run in GitHub Actions, but
touch mapping, screen geometry and on-device text rendering still need
verification on the physical Waveshare device. The firmware and desktop
manager are independent applications; pull/flash firmware and rebuild
desktop only when their own code has changed.