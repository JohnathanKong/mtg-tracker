#pragma once

// Round 360 x 360 hardware status display for the Waveshare ESP32-S3 Knob.
// Headless serial sync continues even if LCD initialization fails.

enum class MthStatus {
    Booting,
    Ready,
    Receiving,
    Complete,
    Error,
    StorageError
};

// Does not throw/fatal-abort if the panel is unavailable.
bool mtg_display_init();
void mtg_display_show(MthStatus state, int storedDecks, int percent = 0,
                      const char *detail = nullptr);
