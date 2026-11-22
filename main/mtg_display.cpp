// Status-first firmware UI for Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// Round 360x360 SH8601 over QSPI, GPIO47 PWM backlight. No LVGL needed
// for this bring-up: the drawing code uses a small local bitmap font,
// RGB565 frame buffer, and DMA-friendly 16-row transfers.
//
// Panel pins/init values reference Apache-2.0 BlueKnob board configuration.
// See THIRD_PARTY_NOTICES.md. Screen is optional to the serial receiver.

#include "mtg_display.h"
#include "panel_init_commands.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSans18pt7b.h"
#include "fonts/FreeSerif24pt7b.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "mtg_display";
constexpr int kWidth = 360, kHeight = 360, kRowsPerTransfer = 16;
constexpr int kClk = 13, kCs = 14, kD0 = 15, kD1 = 16, kD2 = 17,
              kD3 = 18, kReset = 21, kBacklight = 47;

esp_lcd_panel_io_handle_t gIo = nullptr;
esp_lcd_panel_handle_t gPanel = nullptr;
SemaphoreHandle_t gTransferDone = nullptr;
TaskHandle_t gDisplayTask = nullptr;
uint16_t *gFrame = nullptr;     // 360x360 RGB565 in PSRAM
uint16_t *gDmaStrip = nullptr;  // 360x16 RGB565 in DMA-capable internal RAM
uint16_t *gArt = nullptr;       // native 360x360 RGB565 user-supplied commander art
uint8_t *gSkin = nullptr;      // native 360x360 indexed original fantasy asset
char gLoadedArtId[96] = {};
uint32_t gLoadedArtRevision = 0;
bool gHasArt = false;
extern "C" const uint8_t skin_start[] asm("_binary_ui_portrait_hud_360_lz4_start");
extern "C" const uint8_t skin_end[] asm("_binary_ui_portrait_hud_360_lz4_end");


// All USB callbacks update only this small pending status, never the LCD
// itself. The LCD has its own task, so failed SPI initialization or a stalled
// frame transfer cannot prevent USB handshakes / flash sync.
struct PendingStatus {
    MthStatus state = MthStatus::Booting;
    int decks = 0;
    int percent = 0;
    char detail[32] = {};
    uint32_t generation = 1;
    bool gameMode = false;
    MtgGameView game{};
};
portMUX_TYPE gStatusLock = portMUX_INITIALIZER_UNLOCKED;
PendingStatus gPending;


constexpr uint16_t color(unsigned red, unsigned green, unsigned blue) {
    // SH8601 reads RGB565 pixels MSB first. The framebuffer holds the values
    // byte-swapped so the host's little-endian memory is transmitted as RGB565.
    const uint16_t rgb = uint16_t(((red & 0xF8) << 8) |
                                  ((green & 0xFC) << 3) | (blue >> 3));
    return uint16_t((rgb >> 8) | (rgb << 8));
}
constexpr uint16_t kBg       = color(9, 14, 23);
constexpr uint16_t kSurface  = color(20, 31, 44);
constexpr uint16_t kGold     = color(229, 185, 107);
constexpr uint16_t kGoldDim  = color(83, 66, 47);
constexpr uint16_t kIvory    = color(250, 235, 210);
constexpr uint16_t kMuted    = color(143, 160, 181);
constexpr uint16_t kTeal     = color(108, 215, 184);
constexpr uint16_t kGreen    = color(140, 221, 166);
constexpr uint16_t kRed      = color(238, 140, 131);
constexpr uint16_t kTrack    = color(53, 69, 81);

inline void pixel(int x, int y, uint16_t ink);

// Actual image-generated portrait/list HUD asset (with empty text regions).
// Bytes: native 360x360 256-color palette and LZ4-compressed palette indices.
// User-supplied commander art and game numbers are drawn separately at runtime.
constexpr size_t kPaletteByteSize = 256 * 3;


// Small bounds-checked LZ4 block decoder, no host/image runtime dependencies.
// The generated skin is native 360x360 indexed pixels without lossy upscaling.
bool decodeSkin() {
    if (!gSkin) return false;
    if (size_t(skin_end - skin_start) < kPaletteByteSize + 2) return false;
    const uint8_t *src = skin_start + kPaletteByteSize;
    const uint8_t *const end = skin_end;
    uint8_t *out = gSkin;
    uint8_t *const outEnd = gSkin + 360 * 360;
    while (src < end) {
        const uint8_t token = *src++;
        size_t literal = token >> 4;
        if (literal == 15) {
            uint8_t extra = 0;
            do {
                if (src == end) return false;
                extra = *src++;
                literal += extra;
            } while (extra == 255);
        }
        if (size_t(end - src) < literal ||
            size_t(outEnd - out) < literal) return false;
        std::memcpy(out, src, literal);
        out += literal; src += literal;
        if (src == end) break;
        if (end - src < 2) return false;
        const size_t distance = size_t(src[0]) | (size_t(src[1]) << 8);
        src += 2;
        if (distance == 0 || size_t(out - gSkin) < distance) return false;
        size_t match = token & 0x0F;
        if (match == 15) {
            uint8_t extra = 0;
            do {
                if (src == end) return false;
                extra = *src++;
                match += extra;
            } while (extra == 255);
        }
        match += 4;
        if (size_t(outEnd - out) < match) return false;
        for (size_t i = 0; i < match; ++i)
            *(out + i) = *(out + i - distance);
        out += match;
    }
    return out == outEnd;
}

uint32_t crcDeckId(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        crc ^= static_cast<uint8_t>(*s);
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void loadArtFor(const MtgGameView &view) {
    if (std::strcmp(gLoadedArtId, view.deckId) == 0 &&
        gLoadedArtRevision == view.artworkRevision) return;
    gLoadedArtRevision = view.artworkRevision;
    std::snprintf(gLoadedArtId, sizeof(gLoadedArtId), "%s", view.deckId);
    gHasArt = false;
    if (!gArt || !view.deckId[0]) return;
    char path[48];
    std::snprintf(path, sizeof(path), "/mtg/a%08lX.rgb",
                  static_cast<unsigned long>(crcDeckId(view.deckId)));
    FILE *file = std::fopen(path, "rb");
    if (!file) {
        ESP_LOGW(kTag, "Commander artwork missing for deck '%s': %s (sync art from desktop)",
                 view.deckName, path);
        return;
    }
    const size_t len = std::fread(gArt, sizeof(uint16_t), 360 * 360, file);
    const int trailing = std::fgetc(file);
    std::fclose(file);
    gHasArt = len == 360 * 360 && trailing == EOF;
    ESP_LOGI(kTag, "Deck art %s: %s", path, gHasArt ? "loaded" : "invalid");
}

// Compose the approved image-designed chrome with the *actual uploaded deck
// artwork*. The artist-rendered base has empty panels: no stale baked-in values.
void drawGameSkin() {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - 180, dy = y - 180;
            const int radius2 = dx * dx + dy * dy;
            if (radius2 >= 179 * 179) {
                pixel(x, y, kBg);
                continue;
            }
            const uint8_t idx = gSkin ? gSkin[y * kWidth + x] : 0;
            const uint8_t *swatch = skin_start + size_t(idx) * 3;
            int red = swatch[0], green = swatch[1], blue = swatch[2];

            // Restrict the commander portrait to the upper-left photo area.
            // The image is center-cropped instead of sampling its left edge,
            // which previously hid most commanders on the portrait layout.
            // Feather toward the life number and the panel edges.
            if (gHasArt && x >= 11 && x <= 170 && y >= 12 && y <= 236) {
                const int ringFade = std::clamp((177 * 177 - radius2) / 70, 0, 255);
                const int rightFade = std::clamp((171 - x) * 255 / 39, 0, 255);
                const int bottomFade = std::clamp((227 - y) * 255 / 67, 0, 255);
                const int topFade = std::clamp((y - 12) * 255 / 12, 0, 255);
                const int alpha = std::min({235, ringFade, rightFade,
                                            bottomFade, topFade});
                if (alpha > 0) {
                    const int imageX = std::clamp(
                        180 + (x - 88) * 5 / 4, 0, 359);
                    const int imageY = std::clamp(
                        153 + (y - 111) * 5 / 4, 0, 359);
                    const uint16_t swapped = gArt[imageY * 360 + imageX];
                    const uint16_t rgb =
                        uint16_t((swapped >> 8) | (swapped << 8));
                    const int artR = ((rgb >> 11) & 31) * 255 / 31;
                    const int artG = ((rgb >> 5) & 63) * 255 / 63;
                    const int artB = (rgb & 31) * 255 / 31;
                    red = (artR * alpha + red * (255 - alpha)) / 255;
                    green = (artG * alpha + green * (255 - alpha)) / 255;
                    blue = (artB * alpha + blue * (255 - alpha)) / 255;
                }
            }
            pixel(x, y, color(unsigned(red), unsigned(green), unsigned(blue)));
        }
    }
}

struct Glyph { char c; uint8_t rows[7]; };
// Original minimal 5x7 status font. Uppercase letters, digits and punctuation.
constexpr Glyph kFont[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {':',{0,4,4,0,4,4,0}}, {'-',{0,0,0,31,0,0,0}},
    {'.',{0,0,0,0,0,12,12}}, {'/',{1,1,2,4,8,16,16}},
    {'%',{25,25,2,4,8,19,19}}, {'!',{4,4,4,4,4,0,4}},
    {' ',{0,0,0,0,0,0,0}}
};

const uint8_t *glyphFor(char character) {
    character = char(std::toupper(static_cast<unsigned char>(character)));
    for (const auto &glyph : kFont)
        if (glyph.c == character) return glyph.rows;
    return kFont[sizeof(kFont) / sizeof(kFont[0]) - 1].rows;
}

inline void pixel(int x, int y, uint16_t ink) {
    if (x >= 0 && x < kWidth && y >= 0 && y < kHeight)
        gFrame[y * kWidth + x] = ink;
}
void fillRect(int x, int y, int width, int height, uint16_t ink) {
    const int left = std::max(0, x), top = std::max(0, y);
    const int right = std::min(kWidth, x + width);
    const int bottom = std::min(kHeight, y + height);
    if (right <= left || bottom <= top) return;
    for (int row = top; row < bottom; ++row)
        std::fill(gFrame + row * kWidth + left,
                  gFrame + row * kWidth + right, ink);
}
void line(int y, int left, int right, uint16_t ink) {
    fillRect(left, y, right - left, 1, ink);
}
void borderBox(int x, int y, int w, int h, uint16_t border, uint16_t inside) {
    fillRect(x, y, w, h, border);
    fillRect(x + 1, y + 1, w - 2, h - 2, inside);
}

void printAt(int centerX, int y, const char *value, int scale, uint16_t ink) {
    const int len = int(std::strlen(value));
    const int textWidth = len * 6 * scale - scale;
    const int start = centerX - textWidth / 2;
    for (int c = 0; c < len; ++c) {
        const uint8_t *rows = glyphFor(value[c]);
        const int baseX = start + c * 6 * scale;
        for (int r = 0; r < 7; ++r) {
            for (int bit = 0; bit < 5; ++bit) {
                if (rows[r] & (1 << (4 - bit)))
                    fillRect(baseX + bit * scale, y + r * scale,
                             scale, scale, ink);
            }
        }
    }
}

void printCentered(int y, const char *value, int scale, uint16_t ink) {
    printAt(180, y, value, scale, ink);
}

// All text is measured by the actual GFX glyph bounds, then centered in
// the on-screen panel's safe rectangle. This avoids the previous overlapping
// "baseline by eye" layout. Static icons/borders live in ui_portrait_hud_360.
struct TextRect { int x, y, w, h; };
struct FontExtents {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool visible = false;
    int width() const { return visible ? x1 - x0 : 0; }
    int height() const { return visible ? y1 - y0 : 0; }
};

FontExtents getFontExtents(const GFXfont &font, const char *text) {
    FontExtents ext;
    int advance = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        if (*p < font.first || *p > font.last) continue;
        const GFXglyph &g = font.glyph[*p - font.first];
        if (g.width && g.height) {
            const int gx0 = advance + g.xOffset;
            const int gy0 = g.yOffset;
            const int gx1 = gx0 + g.width;
            const int gy1 = gy0 + g.height;
            if (!ext.visible) {
                ext.x0 = gx0; ext.y0 = gy0;
                ext.x1 = gx1; ext.y1 = gy1;
                ext.visible = true;
            } else {
                ext.x0 = std::min(ext.x0, gx0);
                ext.y0 = std::min(ext.y0, gy0);
                ext.x1 = std::max(ext.x1, gx1);
                ext.y1 = std::max(ext.y1, gy1);
            }
        }
        advance += g.xAdvance;
    }
    return ext;
}

void textInPanel(TextRect rect, const char *text,
                 const GFXfont &preferred, uint16_t ink) {
    const GFXfont *font = &preferred;
    FontExtents e = getFontExtents(*font, text);
    if (e.width() > rect.w || e.height() > rect.h) {
        font = &FreeSans9pt7b;
        e = getFontExtents(*font, text);
    }
    if (!e.visible) return;
    if (e.width() > rect.w || e.height() > rect.h) {
        // Small fallback, mainly for 4-digit poison/damage values.
        printAt(rect.x + rect.w / 2,
                rect.y + (rect.h - 7) / 2, text, 1, ink);
        return;
    }
    int originX = rect.x + (rect.w - e.width()) / 2 - e.x0;
    const int baseline = rect.y + (rect.h - e.height()) / 2 - e.y0;

    // The screen is round. A value can fit a rectangular capsule yet still
    // cross the physical circular edge, especially the top-right POISON row.
    // Reposition the full rendered glyph bounds *before* drawing instead of
    // relying on frame clipping (which chops off the digits).
    constexpr int kTextSafeRadius = 168; // 12 px inside the 180 px edge
    auto fitsRoundScreen = [&](int shift) {
        const int l = originX + e.x0 + shift;
        const int r = originX + e.x1 - 1 + shift;
        const int t = baseline + e.y0;
        const int b = baseline + e.y1 - 1;
        if (l < rect.x || r >= rect.x + rect.w) return false;
        const int xs[] = {l, r};
        const int ys[] = {t, b};
        for (int x : xs) for (int y : ys) {
            const int dx = x - 180, dy = y - 180;
            if (dx * dx + dy * dy > kTextSafeRadius * kTextSafeRadius)
                return false;
        }
        return true;
    };
    if (!fitsRoundScreen(0)) {
        const int inward = rect.x + rect.w / 2 >= 180 ? -1 : 1;
        for (int px = 1; px <= rect.w / 2; ++px) {
            if (fitsRoundScreen(inward * px)) {
                originX += inward * px;
                break;
            }
        }
    }
    int pen = originX;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        if (*p < font->first || *p > font->last) continue;
        const GFXglyph &g = font->glyph[*p - font->first];
        const uint8_t *data = font->bitmap + g.bitmapOffset;
        int bit = 0;
        for (int y = 0; y < g.height; ++y) {
            for (int x = 0; x < g.width; ++x, ++bit) {
                if (data[bit >> 3] & (0x80 >> (bit & 7))) {
                    pixel(pen + g.xOffset + x,
                          baseline + g.yOffset + y, ink);
                }
            }
        }
        pen += g.xAdvance;
    }
}


// New artwork-first round HUD. The uploaded deck image is RGB565 pixel data,
// not part of the chrome. All numbers, counter names and selection highlights
// are rendered live, so none of the example dragon/numbers are baked in.
void fillRound(int x, int y, int w, int h, int radius, uint16_t ink) {
    if (w <= 0 || h <= 0) return;
    radius = std::clamp(radius, 1, std::max(1, (std::min(w, h) - 1) / 2));
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const int ox = px - std::clamp(px, radius, w - radius - 1);
            const int oy = py - std::clamp(py, radius, h - radius - 1);
            if (ox * ox + oy * oy <= radius * radius)
                pixel(x + px, y + py, ink);
        }
    }
}
void chromePanel(int x, int y, int w, int h, int radius,
                 uint16_t border, uint16_t surface) {
    fillRound(x, y, w, h, radius, border);
    fillRound(x + 1, y + 1, w - 2, h - 2, radius - 1, surface);
    if (w > 12) fillRect(x + radius, y + 2, w - 2 * radius, 1, kGoldDim);
}
void drawHeroArt() {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - 180, dy = y - 180;
            const int d2 = dx * dx + dy * dy;
            if (d2 > 179 * 179) { pixel(x, y, kBg); continue; }
            if (d2 > 175 * 175) { pixel(x, y, kGold); continue; }
            if (d2 > 172 * 172) { pixel(x, y, kGoldDim); continue; }

            int red = 7, green = y < 220 ? 20 : 13, blue = y < 220 ? 26 : 18;
            if (gHasArt && y < 213) {
                const uint16_t swapped = gArt[y * 360 + x];
                const uint16_t rgb = uint16_t((swapped >> 8) | (swapped << 8));
                const int ar = ((rgb >> 11) & 31) * 255 / 31;
                const int ag = ((rgb >> 5) & 63) * 255 / 63;
                const int ab = (rgb & 31) * 255 / 31;
                int opacity = y < 138 ? 255 : std::max(0, (212 - y) * 255 / 74);
                if (d2 > 164 * 164) opacity = opacity * 65 / 100;
                red = (ar * opacity + red * (255 - opacity)) / 255;
                green = (ag * opacity + green * (255 - opacity)) / 255;
                blue = (ab * opacity + blue * (255 - opacity)) / 255;
            }
            pixel(x, y, color(unsigned(red), unsigned(green), unsigned(blue)));
        }
    }
    // A thin second inner ring, kept off the pixels needed by the tiles.
    for (int y = 6; y < 354; ++y)
        for (int x = 6; x < 354; ++x) {
            const int dx = x - 180, dy = y - 180;
            const int r2 = dx * dx + dy * dy;
            if (r2 >= 171 * 171 && r2 < 172 * 172)
                pixel(x, y, kGoldDim);
        }
}
void drawPagination(int current, int total, int y) {
    if (total <= 1) return;
    const int visible = std::min(total, 7);
    const int start = 180 - (visible - 1) * 10 / 2;
    for (int n = 0; n < visible; ++n) {
        const int cx = start + n * 10;
        fillRound(cx - 3, y, 6, 6, 3,
                  n == current ? kGold : color(85, 92, 103));
    }
}
void drawPlusNavigation(int cx, int cy) {
    fillRound(cx - 13, cy - 13, 26, 26, 13, kGold);
    fillRound(cx - 11, cy - 11, 22, 22, 11, kBg);
    fillRect(cx - 6, cy - 1, 13, 2, kGold);
    fillRect(cx - 1, cy - 6, 2, 13, kGold);
}
uint16_t accentFor(uint8_t type) {
    switch (type) {
    case 0: return color(229, 170, 96); // Commander deaths
    case 1: return color(222, 102, 80); // Damage
    case 2: return color(82, 199, 180); // Experience / poison
    case 3: return color(105, 170, 224); // Treasure / rad
    default: return color(179, 136, 220);
    }
}
void drawTinyMark(int centerX, int centerY, uint16_t accent) {
    // Diamond glyph without fragile Unicode font assumptions.
    for (int dy = -5; dy <= 5; ++dy) {
        const int span = 5 - std::abs(dy);
        pixel(centerX - span, centerY + dy, accent);
        pixel(centerX + span, centerY + dy, accent);
    }
}
void drawTileLabel(int x, int y, int w, const char *full, uint16_t ink) {
    // The 5x7 pixel font is deliberate: long "COMMANDER DAMAGE" labels can
    // use two rows inside the 34px capsule without overlapping the value.
    char text[36]{};
    std::snprintf(text, sizeof(text), "%s", full);
    for (char &c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    const int len = int(std::strlen(text));
    const int maxLetters = std::max(3, std::min(15, w / 6));
    if (len <= maxLetters) {
        printAt(x + w / 2, y + 10, text, 1, ink);
        return;
    }
    int split = -1;
    for (int i = 1; i < len; ++i)
        if (text[i] == ' ' && i <= maxLetters &&
            len - i - 1 <= maxLetters) { split = i; break; }
    if (split < 0) {
        char cut[20]{};
        std::snprintf(cut, sizeof(cut), "%.*s", maxLetters, text);
        printAt(x + w / 2, y + 10, cut, 1, ink);
        return;
    }
    text[split] = '\0';
    printAt(x + w / 2, y + 6, text, 1, ink);
    printAt(x + w / 2, y + 17, text + split + 1, 1, ink);
}
void drawCounterTile(int x, int y, int w, int h, const MtgTile &tile) {
    const uint16_t accent = tile.selected ? color(113, 235, 239) : accentFor(tile.accent);
    chromePanel(x, y, w, h, 9, accent, color(17, 20, 27));
    fillRect(x + 2, y + 5, 3, h - 10, accent);
    drawTinyMark(x + 16, y + h / 2, accent);
    char value[20];
    std::snprintf(value, sizeof(value), "%d", tile.value);
    const int valueWidth = h < 35 ? 28 : 31;
    drawTileLabel(x + 26, y, w - 29 - valueWidth, tile.label, kIvory);
    textInPanel({x + w - valueWidth - 4, y + 3, valueWidth, h - 6},
                value, FreeSans18pt7b, tile.selected ? kTeal : kIvory);
}

void drawDashboard(const MtgGameView &view) {
    loadArtFor(view);
    drawHeroArt();

    if (view.deckCount == 0) {
        chromePanel(58, 132, 244, 103, 16, kGold, kSurface);
        textInPanel({72, 147, 216, 32}, "NO DECKS", FreeSans18pt7b, kIvory);
        textInPanel({68, 188, 224, 24}, "SYNC FROM DESKTOP", FreeSans9pt7b, kTeal);
        return;
    }

    if (view.screen == MtgScreen::ChooseDeck) {
        chromePanel(44, 115, 272, 156, 17, kGold, kBg);
        textInPanel({60, 124, 240, 25}, "SELECT DECK", FreeSans18pt7b, kGold);
        char position[32];
        std::snprintf(position, sizeof(position), "%d / %d",
                      view.deckIndex + 1, view.deckCount);
        textInPanel({80, 160, 200, 33}, position, FreeSans18pt7b, kTeal);
        char name[32];
        std::snprintf(name, sizeof(name), "%.28s", view.deckName);
        textInPanel({62, 210, 236, 25}, name, FreeSans9pt7b, kIvory);
        drawPagination(view.deckIndex, view.deckCount, 292);
        return;
    }

    if (view.screen == MtgScreen::ConfirmReset) {
        chromePanel(45, 110, 270, 164, 17, kGold, kBg);
        textInPanel({64, 126, 232, 32}, "NEW GAME?", FreeSans18pt7b, kGold);
        textInPanel({56, 162, 248, 25}, "RESET ALL COUNTERS", FreeSans9pt7b, kIvory);
        chromePanel(73, 196, 102, 51, 11, kGold, kGoldDim);
        chromePanel(184, 196, 102, 51, 11, kTeal, kSurface);
        textInPanel({78, 204, 92, 33}, "RESET", FreeSans9pt7b, kIvory);
        textInPanel({189, 204, 92, 33}, "CANCEL", FreeSans9pt7b, kIvory);
        return;
    }

    if (view.screen == MtgScreen::AddCounter) {
        chromePanel(36, 72, 288, 244, 19, kGold, kBg);
        textInPanel({69, 79, 222, 23}, "ADD COUNTER", FreeSans18pt7b, kGold);
        for (int i = 0; i < view.choiceCount; ++i) {
            const int y = 104 + i * 48;
            chromePanel(55, y, 250, 40, 10,
                        color(93, 160, 158), kSurface);
            textInPanel({66, y + 6, 227, 28},
                        view.choices[i].label, FreeSans9pt7b, kIvory);
        }
        drawPagination(view.addPage, view.addPageCount, 310);
        textInPanel({135, 318, 90, 18}, "BACK", FreeSans9pt7b, kGold);
        return;
    }

    if (!gHasArt)
        textInPanel({91, 61, 178, 30}, "SYNC ART", FreeSans18pt7b, kMuted);

    // The permanent numbers never change location when swiping the tile pages.
    // Casts is editable, Tax is derived as +2 mana per previous cast.
    chromePanel(28, 184, 304, 39, 15, kGoldDim, color(9, 26, 25));
    fillRect(179, 198, 1, 17, kGoldDim);
    textInPanel({49, 193, 70, 19}, "CASTS", FreeSans9pt7b, kIvory);
    char number[24];
    std::snprintf(number, sizeof(number), "%d", view.casts);
    textInPanel({124, 189, 53, 28}, number, FreeSans18pt7b,
                view.selected == MtgCounter::Casts ? kTeal : kIvory);
    textInPanel({201, 193, 55, 19}, "TAX", FreeSans9pt7b, kIvory);
    std::snprintf(number, sizeof(number), "+%d", view.casts * 2);
    textInPanel({264, 189, 56, 28}, number, FreeSans18pt7b, kGold);

    // Life crest sits over the lower part of the artwork as in the reference.
    chromePanel(110, 111, 140, 84, 21,
                view.selected == MtgCounter::Life ? kTeal : kGold, color(8, 23, 25));
    textInPanel({130, 117, 100, 19}, "LIFE", FreeSans9pt7b, kGold);
    std::snprintf(number, sizeof(number), "%d", view.life);
    textInPanel({116, 137, 128, 50}, number, FreeSerif24pt7b,
                view.selected == MtgCounter::Life ? kTeal : kIvory);

    // Exactly four optional counters per page. Their order is defined by
    // saved desktop preferences and game-only additions.
    if (view.tileCount > 0) drawCounterTile(31, 230, 146, 33, view.tiles[0]);
    if (view.tileCount > 1) drawCounterTile(183, 230, 146, 33, view.tiles[1]);
    if (view.tileCount > 2) drawCounterTile(56, 269, 121, 31, view.tiles[2]);
    if (view.tileCount > 3) drawCounterTile(183, 269, 121, 31, view.tiles[3]);

    drawPagination(view.page, view.pageCount, 320);
    drawPlusNavigation(266, 315);
}
void drawShell() {
    std::fill(gFrame, gFrame + kWidth * kHeight, kBg);
    // Rings live at the edge of the round screen, not in the corners.
    for (int y = 0; y < kHeight; ++y) {
        const int dy = y - 180;
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - 180;
            const int radius2 = dx * dx + dy * dy;
            if (radius2 >= 169 * 169 && radius2 <= 171 * 171)
                pixel(x, y, kGoldDim);
            else if (radius2 >= 162 * 162 && radius2 <= 163 * 163 &&
                     (x + y) % 23 < 13)
                pixel(x, y, kGoldDim);
        }
    }
    // Small golden diamond crown.
    for (int y = 45; y <= 61; ++y) {
        const int radius = 8 - std::abs(y - 53);
        fillRect(180 - radius, y, radius * 2 + 1, 1, kGold);
    }

    printCentered(80, "MTG TRACKER", 2, kIvory);
    printCentered(110, "COMMANDER COMPANION", 1, kGold);
    line(141, 66, 294, kGoldDim);
    line(142, 114, 246, kGold);
}

void drawState(MthStatus state, int storedDecks, int progress, const char *detail) {
    drawShell();
    const char *label = "STARTING";
    uint16_t accent = kGold;
    switch (state) {
    case MthStatus::Booting:      label = "STARTING";       break;
    case MthStatus::Ready:        label = "READY TO SYNC";  accent = kTeal; break;
    case MthStatus::Receiving:    label = "RECEIVING";      break;
    case MthStatus::Complete:     label = "SYNC COMPLETE";  accent = kGreen; break;
    case MthStatus::Error:        label = "SYNC FAILED";    accent = kRed; break;
    case MthStatus::StorageError: label = "STORAGE ERROR";  accent = kRed; break;
    }

    borderBox(52, 158, 256, 48, accent, kSurface);
    printCentered(175, label, 2, accent);
    printCentered(221, "DECKS STORED", 1, kMuted);

    char number[24] = {};
    std::snprintf(number, sizeof(number), "%d", storedDecks);
    printCentered(240, number, 3, kIvory);

    if (state == MthStatus::Receiving) {
        const int clamped = std::clamp(progress, 0, 100);
        borderBox(78, 285, 204, 12, kGoldDim, kTrack);
        fillRect(80, 287, clamped * 2, 8, kGold);
        std::snprintf(number, sizeof(number), "%d%%", clamped);
        printCentered(308, number, 1, kGold);
    } else if (state == MthStatus::Error && detail && detail[0]) {
        // The ASCII status labels are intentionally short enough for 360px.
        char reason[30]{};
        for (int i = 0; i < 26 && detail[i]; ++i)
            reason[i] = char(std::toupper(static_cast<unsigned char>(detail[i])));
        printCentered(291, reason, 1, kRed);
    } else if (state == MthStatus::Complete) {
        printCentered(291, "SAVED TO DEVICE", 1, kGreen);
    } else if (state == MthStatus::StorageError) {
        printCentered(291, "CHECK FLASH LOGS", 1, kRed);
    } else {
        printCentered(291, "CONNECT TO DESKTOP", 1, kMuted);
    }
    line(327, 115, 245, kGoldDim);
}

bool onColorDone(esp_lcd_panel_io_handle_t /*io*/, esp_lcd_panel_io_event_data_t* /*event*/,
                 void *context) {
    BaseType_t awoken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &awoken);
    return awoken == pdTRUE;
}

bool check(esp_err_t error, const char *step) {
    if (error == ESP_OK) return true;
    ESP_LOGE(kTag, "%s: %s", step, esp_err_to_name(error));
    return false;
}

void setBacklight(unsigned brightness) {
    if (check(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness),
              "LED backlight duty"))
        check(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), "LED backlight update");
}

bool initBacklight() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_0;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (!check(ledc_timer_config(&timer), "Backlight PWM timer")) return false;

    ledc_channel_config_t channel = {};
    channel.gpio_num = kBacklight;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    return check(ledc_channel_config(&channel), "Backlight PWM channel");
}

bool present() {
    for (int y = 0; y < kHeight; y += kRowsPerTransfer) {
        const int rows = std::min(kRowsPerTransfer, kHeight - y);
        std::memcpy(gDmaStrip, gFrame + y * kWidth, size_t(rows * kWidth) * 2);
        if (!check(esp_lcd_panel_draw_bitmap(gPanel, 0, y, kWidth, y + rows,
                                             gDmaStrip), "LCD draw"))
            return false;
        if (xSemaphoreTake(gTransferDone, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGE(kTag, "LCD DMA transfer timeout");
            return false;
        }
    }
    return true;
}

bool initDisplayHardware() {
    ESP_LOGI(kTag, "Initialize 360x360 SH8601 status display");

    gTransferDone = xSemaphoreCreateBinary();
    gDmaStrip = static_cast<uint16_t *>(heap_caps_malloc(
        kWidth * kRowsPerTransfer * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    gFrame = static_cast<uint16_t *>(heap_caps_malloc(
        kWidth * kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    gArt = static_cast<uint16_t *>(heap_caps_malloc(
        360 * 360 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    gSkin = static_cast<uint8_t *>(heap_caps_malloc(
        360 * 360, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (gSkin && !decodeSkin()) {
        ESP_LOGE(kTag, "Bundled fantasy image asset invalid");
        heap_caps_free(gSkin);
        gSkin = nullptr;
    }
    if (!gTransferDone || !gDmaStrip || !gFrame) {
        ESP_LOGE(kTag, "No RAM for display (PSRAM must be enabled)");
        return false;
    }
    if (!initBacklight()) return false;

    spi_bus_config_t bus = {};
    bus.sclk_io_num = kClk;
    bus.data0_io_num = kD0;
    bus.data1_io_num = kD1;
    bus.data2_io_num = kD2;
    bus.data3_io_num = kD3;
    bus.max_transfer_sz = kWidth * kRowsPerTransfer * 2;
    if (!check(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "QSPI bus"))
        return false;

    // Use zero-initialized fields instead of the upstream QSPI aggregate macro:
    // ESP-IDF 6.1 builds C++ with -Werror=missing-field-initializers.
    esp_lcd_panel_io_spi_config_t ioConfig = {};
    ioConfig.cs_gpio_num = static_cast<gpio_num_t>(kCs);
    ioConfig.dc_gpio_num = static_cast<gpio_num_t>(-1);
    ioConfig.spi_mode = 0;
    ioConfig.pclk_hz = 40 * 1000 * 1000;
    ioConfig.trans_queue_depth = 10;
    ioConfig.on_color_trans_done = onColorDone;
    ioConfig.user_ctx = gTransferDone;
    ioConfig.lcd_cmd_bits = 32;
    ioConfig.lcd_param_bits = 8;
    ioConfig.flags.quad_mode = true;
    if (!check(esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &ioConfig, &gIo),
        "SH8601 QSPI panel IO"))
        return false;

    sh8601_vendor_config_t vendor = {};
    vendor.init_cmds = mtg_panel::init_commands;
    vendor.init_cmds_size = sizeof(mtg_panel::init_commands) /
                            sizeof(mtg_panel::init_commands[0]);
    vendor.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panelConfig = {};
    panelConfig.reset_gpio_num = static_cast<gpio_num_t>(kReset);
    panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panelConfig.bits_per_pixel = 16;
    panelConfig.vendor_config = &vendor;

    if (!check(esp_lcd_new_panel_sh8601(gIo, &panelConfig, &gPanel),
               "SH8601 panel driver"))
        return false;
    if (!check(esp_lcd_panel_reset(gPanel), "Panel reset") ||
        !check(esp_lcd_panel_init(gPanel), "Panel power-up") ||
        !check(esp_lcd_panel_disp_on_off(gPanel, true), "Panel display on"))
        return false;

    // Light the panel before attempting a frame transfer so the backlight
    // is visible even when the SPI transfer path isn't functioning.
    setBacklight(210);
    ESP_LOGI(kTag, "MTG status LCD hardware initialized");
    return true;
}

void displayTask(void *) {
    if (!initDisplayHardware()) {
        ESP_LOGE(kTag, "LCD initialization failed; USB receiver remains active");
        gDisplayTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint32_t drawnGeneration = 0;
    while (true) {
        PendingStatus snapshot;
        taskENTER_CRITICAL(&gStatusLock);
        snapshot = gPending;
        taskEXIT_CRITICAL(&gStatusLock);

        if (snapshot.generation != drawnGeneration) {
            if (snapshot.gameMode)
                drawDashboard(snapshot.game);
            else
                drawState(snapshot.state, snapshot.decks, snapshot.percent,
                          snapshot.detail);
            if (!present()) {
                ESP_LOGE(kTag, "LCD transfer failed; USB sync remains active");
                break;
            }
            drawnGeneration = snapshot.generation;
        }
        // Wake immediately for a changed counter instead of polling every 40 ms.
        // Updates received during a full-frame DMA transfer remain pending.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    gDisplayTask = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

bool mtg_display_init() {
    // Never initialize the LCD synchronously from app_main. Full LCD
    // initialization and DMA flushes can block. The USB sync receiver
    // must be serviced independently, even if the screen is broken.
    return xTaskCreate(displayTask, "mtg_lcd", 10240, nullptr,
                       tskIDLE_PRIORITY + 1, &gDisplayTask)
           == pdPASS;
}

void mtg_display_show(MthStatus state, int storedDecks, int percent,
                      const char *detail) {
    // This function is deliberately nonblocking so even progress updates
    // can't hold up the USB receive loop.
    taskENTER_CRITICAL(&gStatusLock);
    gPending.gameMode = false;
    gPending.state = state;
    gPending.decks = storedDecks;
    gPending.percent = percent;
    if (detail) {
        std::strncpy(gPending.detail, detail, sizeof(gPending.detail) - 1);
        gPending.detail[sizeof(gPending.detail) - 1] = '\0';
    } else {
        gPending.detail[0] = '\0';
    }
    ++gPending.generation;
    taskEXIT_CRITICAL(&gStatusLock);
    if (gDisplayTask) xTaskNotifyGive(gDisplayTask);
}


void mtg_display_showGame(const MtgGameView &view) {
    taskENTER_CRITICAL(&gStatusLock);
    gPending.gameMode = true;
    gPending.game = view;
    ++gPending.generation;
    taskEXIT_CRITICAL(&gStatusLock);
    if (gDisplayTask) xTaskNotifyGive(gDisplayTask);
}
