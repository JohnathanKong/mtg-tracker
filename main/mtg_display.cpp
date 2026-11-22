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
#include <cmath>
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
uint16_t *gBackdrop = nullptr;  // cached art + ornate chrome (speeds up dial updates)
char gBackdropDeck[96]{};
uint32_t gBackdropRevision = 0;
int gBackdropTiles = -1;
int gBackdropOpponents[3]{-1, -1, -1};
bool gBackdropValid = false;
uint16_t *gDmaStrip = nullptr;  // 360x16 RGB565 in DMA-capable internal RAM
uint16_t *gArt = nullptr;       // native 360x360 RGB565 user-supplied commander art
uint8_t *gSkin = nullptr;      // 4-bit packed 360x360 antialiased ornate chrome overlay
uint8_t *gFontRaw = nullptr;    // unpacked 4-bit AA glyph atlas, PSRAM
bool gAaReady = false;
char gLoadedArtId[96] = {};
uint32_t gLoadedArtRevision = 0;
bool gHasArt = false;
extern "C" const uint8_t skin_start[] asm("_binary_ui_reference_chrome_256_lz4_start");
extern "C" const uint8_t skin_end[] asm("_binary_ui_reference_chrome_256_lz4_end");
extern "C" const uint8_t font_start[] asm("_binary_ui_font_atlas_lz4_start");
extern "C" const uint8_t font_end[] asm("_binary_ui_font_atlas_lz4_end");


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

// Asset: 360x360 antialiased 32-color RGBA palette and LZ4 pixels.
// Chrome and decoration contain no baked-in commander art or changing values.
constexpr size_t kPaletteByteSize = 256 * 3;


// Small bounds-checked LZ4 block decoder, no host/image runtime dependencies.
// The generated skin is native 360x360 indexed pixels without lossy upscaling.
// Strict LZ4 block decoder. The chrome and antialiased fonts have separate,
// checked buffers so corrupt assets can never overrun our pixel framebuffer.
bool decodeLz4(const uint8_t *src, const uint8_t *end,
               uint8_t *out, size_t outLen) {
    if (!src || !end || !out || src > end) return false;
    uint8_t *const base = out;
    uint8_t *const outEnd = out + outLen;
    while (src < end) {
        const uint8_t token = *src++;
        size_t literal = token >> 4;
        if (literal == 15) {
            uint8_t extra;
            do {
                if (src == end) return false;
                extra = *src++;
                literal += extra;
            } while (extra == 255);
        }
        if (size_t(end - src) < literal || size_t(outEnd - out) < literal)
            return false;
        std::memcpy(out, src, literal);
        out += literal;
        src += literal;
        if (src == end) break;
        if (end - src < 2) return false;
        const size_t distance = size_t(src[0]) | (size_t(src[1]) << 8);
        src += 2;
        if (distance == 0 || size_t(out - base) < distance) return false;
        size_t match = token & 15u;
        if (match == 15) {
            uint8_t extra;
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

bool decodeSkin() {
    return gSkin && size_t(skin_end - skin_start) > kPaletteByteSize &&
           decodeLz4(skin_start + kPaletteByteSize, skin_end, gSkin, 360u * 360u * 2u);
}

// Noto Serif Display glyphs were rasterized with FreeType at 4x resolution,
// Lanczos-downsampled and stored as 4-bit alpha. Unlike the old GFX fonts,
// these stay smooth at 360px and need no on-device TrueType renderer.
struct AaGlyph {
    unsigned char character = 0, width = 0, height = 0;
    int8_t xBearing = 0, yBearing = 0;
    unsigned char advance = 0;
    const uint8_t *bitmap = nullptr;
};
struct AaFace {
    unsigned char nominalSize = 0, count = 0;
    AaGlyph glyph[48]{};
};
AaFace gAaFaces[3];
bool initAaFonts() {
    if (size_t(font_end - font_start) < 8) return false;
    const size_t length = size_t(font_start[0]) | (size_t(font_start[1]) << 8) |
                          (size_t(font_start[2]) << 16) | (size_t(font_start[3]) << 24);
    if (length < 20 || length > 65536) return false;
    gFontRaw = static_cast<uint8_t *>(heap_caps_malloc(
        length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!gFontRaw || !decodeLz4(font_start + 4, font_end, gFontRaw, length))
        return false;
    const uint8_t *cursor = gFontRaw;
    const uint8_t *const end = gFontRaw + length;
    if (end - cursor < 5 || std::memcmp(cursor, "MFA1", 4) != 0) return false;
    cursor += 4;
    if (*cursor++ != 3) return false;
    for (AaFace &face : gAaFaces) {
        if (end - cursor < 2) return false;
        face.nominalSize = *cursor++;
        face.count = *cursor++;
        if (face.count > 48) return false;
        for (unsigned n = 0; n < face.count; ++n) {
            if (end - cursor < 6) return false;
            AaGlyph &glyph = face.glyph[n];
            glyph.character = *cursor++;
            glyph.width = *cursor++;
            glyph.height = *cursor++;
            glyph.xBearing = static_cast<int8_t>(*cursor++);
            glyph.yBearing = static_cast<int8_t>(*cursor++);
            glyph.advance = *cursor++;
            const size_t size = (size_t(glyph.width) * glyph.height + 1) / 2;
            if (size_t(end - cursor) < size) return false;
            glyph.bitmap = cursor;
            cursor += size;
        }
    }
    gAaReady = cursor == end;
    return gAaReady;
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



// The artwork and all numbers remain live. This indexed decorative layer
// contains ONLY antialiased gold curves, ornamental crests and blank counter
// capsules. The 32-entry palette is RGBA; index zero is fully transparent.
void blendInk(int x, int y, uint8_t red, uint8_t green, uint8_t blue, int alpha) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight || alpha <= 0) return;
    if (alpha >= 255) {
        pixel(x, y, color(red, green, blue));
        return;
    }
    const uint16_t stored = gFrame[y * kWidth + x];
    const uint16_t current = uint16_t((stored >> 8) | (stored << 8));
    const int r = ((current >> 11) & 31) * 255 / 31;
    const int g = ((current >> 5) & 63) * 255 / 63;
    const int b = (current & 31) * 255 / 31;
    pixel(x, y, color(unsigned((red * alpha + r * (255 - alpha) + 127) / 255),
                      unsigned((green * alpha + g * (255 - alpha) + 127) / 255),
                      unsigned((blue * alpha + b * (255 - alpha) + 127) / 255)));
}
void applyOrnateSkin(const MtgGameView &view) {
    if (!gSkin) return;
    for (int y = 0; y < 360; ++y) {
        for (int x = 0; x < 360; ++x) {
            // Discard the old oversized tracker tiles. They were
            // flattened into the source image with rectangular text masks
            // that visibly broke the gold curves at native resolution.
            // Compact AA counters and the Edit control are drawn live.
            if (y >= 248 && y <= 292 && x >= 43 && x <= 319)
                continue;
            const int player = y >= 211 && y <= 245 ?
                (x >= 75 && x <= 140 ? 0 :
                 x >= 144 && x <= 215 ? 1 :
                 x >= 217 && x <= 286 ? 2 : -1) : -1;
            if (player >= 0 && !view.damageIndices[player]) continue;
            const size_t off = size_t(y) * 360u + size_t(x);
            const uint8_t index = gSkin[off * 2u];
            const uint8_t alpha = gSkin[off * 2u + 1u];
            if (!alpha) continue;
            const uint8_t *rgb = skin_start + size_t(index) * 3u;
            blendInk(x, y, rgb[0], rgb[1], rgb[2], alpha);
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
// Small, independent Add Counter control in the lower-right. Unlike the
// opponent '+' in the damage strip, this opens the optional counter chooser.
// Soft RGB565 blending keeps the ring round at 360px rather than pixel-stepped.
void drawPlusNavigation(int cx, int cy) {
    constexpr int outerRadius = 14;
    constexpr int innerRadius = 12;
    for (int yy = -outerRadius-1; yy <= outerRadius+1; ++yy) {
        for (int xx = -outerRadius-1; xx <= outerRadius+1; ++xx) {
            const int r2 = xx * xx + yy * yy;
            if (r2 > (outerRadius+1) * (outerRadius+1)) continue;
            if (r2 <= innerRadius * innerRadius) {
                blendInk(cx + xx, cy + yy, 8, 23, 23, 195);
            } else if (r2 <= outerRadius * outerRadius) {
                blendInk(cx + xx, cy + yy, 239, 191, 105, 235);
            } else {
                blendInk(cx + xx, cy + yy, 239, 191, 105, 55);
            }
        }
    }
    for (int dx = -6; dx <= 6; ++dx)
        blendInk(cx + dx, cy, 255, 218, 152, 255);
    for (int dy = -6; dy <= 6; ++dy)
        blendInk(cx, cy + dy, 255, 218, 152, 255);
}

// Antialias by integrating sixteen subpixel samples from the larger bundled
// GFX glyphs. Labels are downsampled from 18pt shapes rather than rendering
// jagged 5x7 uppercase blocks or stretching small text.
void aaText(TextRect rect, const char *text, const GFXfont &font,
            int desiredHeight, uint16_t ink) {
    const FontExtents ext = getFontExtents(font, text);
    if (!ext.visible || rect.w <= 0 || rect.h <= 0) return;
    const float scale = std::min({0.90f, float(desiredHeight) / float(ext.height()),
                                   float(rect.h) / float(ext.height()),
                                   float(rect.w) / float(ext.width())});
    if (scale <= 0.0f) return;
    const int outW = std::max(1, int(ext.width() * scale + 0.5f));
    const int outH = std::max(1, int(ext.height() * scale + 0.5f));
    const int offsetX = rect.x + (rect.w - outW) / 2;
    const int offsetY = rect.y + (rect.h - outH) / 2;
    const uint16_t actual = uint16_t((ink >> 8) | (ink << 8));
    const uint8_t red = uint8_t(((actual >> 11) & 31) * 255 / 31);
    const uint8_t green = uint8_t(((actual >> 5) & 63) * 255 / 63);
    const uint8_t blue = uint8_t((actual & 31) * 255 / 31);
    int pen = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        if (*p < font.first || *p > font.last) continue;
        const GFXglyph &glyph = font.glyph[*p - font.first];
        const uint8_t *bits = font.bitmap + glyph.bitmapOffset;
        const float posX = (pen + glyph.xOffset - ext.x0) * scale;
        const float posY = (glyph.yOffset - ext.y0) * scale;
        const int left = std::max(0, int(posX));
        const int top = std::max(0, int(posY));
        const int right = std::min(outW, int(posX + glyph.width * scale + 1.0f));
        const int bottom = std::min(outH, int(posY + glyph.height * scale + 1.0f));
        for (int dy = top; dy < bottom; ++dy) {
            for (int dx = left; dx < right; ++dx) {
                int coverage = 0;
                for (int sy = 0; sy < 4; ++sy) {
                    const int sourceY = int((dy + (sy + 0.5f) * 0.25f - posY) / scale);
                    if (sourceY < 0 || sourceY >= glyph.height) continue;
                    for (int sx = 0; sx < 4; ++sx) {
                        const int sourceX = int((dx + (sx + 0.5f) * 0.25f - posX) / scale);
                        if (sourceX < 0 || sourceX >= glyph.width) continue;
                        const int bit = sourceY * glyph.width + sourceX;
                        if ((bits[bit >> 3] & (0x80u >> (bit & 7))) != 0)
                            ++coverage;
                    }
                }
                if (coverage) {
                    const int screenX = offsetX + dx, screenY = offsetY + dy;
                    if (screenX >= rect.x && screenX < rect.x + rect.w &&
                        screenY >= rect.y && screenY < rect.y + rect.h)
                        blendInk(screenX, screenY, red, green, blue,
                                 coverage * 255 / 16);
                }
            }
        }
        pen += glyph.xAdvance;
    }
}

const AaGlyph *glyphForAa(const AaFace &font, unsigned char c) {
    for (unsigned i = 0; i < font.count; ++i)
        if (font.glyph[i].character == c) return &font.glyph[i];
    return nullptr;
}
void atlasText(TextRect rect, const char *text, int fontSize,
               uint16_t ink) {
    if (!gAaReady) {
        aaText(rect, text, fontSize == 2 ? FreeSerif24pt7b : FreeSans18pt7b,
               fontSize == 2 ? 44 : fontSize == 1 ? 20 : 11, ink);
        return;
    }
    const AaFace &face = gAaFaces[std::clamp(fontSize, 0, 2)];
    int pen = 0, minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool hasGlyphs = false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(
            std::toupper(static_cast<unsigned char>(*p)));
        const AaGlyph *g = glyphForAa(face, ch);
        if (!g) continue;
        if (g->width && g->height) {
            minX = hasGlyphs ? std::min(minX, pen + int(g->xBearing)) : pen + int(g->xBearing);
            maxX = std::max(maxX, pen + int(g->xBearing) + int(g->width));
            minY = hasGlyphs ? std::min(minY, int(g->yBearing)) : int(g->yBearing);
            maxY = std::max(maxY, int(g->yBearing) + int(g->height));
            hasGlyphs = true;
        }
        pen += g->advance;
    }
    if (!hasGlyphs) return;
    if ((maxX - minX > rect.w || maxY - minY > rect.h) && fontSize > 0) {
        atlasText(rect, text, fontSize - 1, ink);
        return;
    }
    const int x0 = rect.x + (rect.w - (maxX - minX)) / 2 - minX;
    const int baseline = rect.y + (rect.h - (maxY - minY)) / 2 - minY;
    const uint16_t rgb = uint16_t((ink >> 8) | (ink << 8));
    const uint8_t red = uint8_t(((rgb >> 11) & 31u) * 255u / 31u);
    const uint8_t green = uint8_t(((rgb >> 5) & 63u) * 255u / 63u);
    const uint8_t blue = uint8_t((rgb & 31u) * 255u / 31u);
    pen = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(
            std::toupper(static_cast<unsigned char>(*p)));
        const AaGlyph *g = glyphForAa(face, ch);
        if (!g) continue;
        for (int gy = 0; gy < g->height; ++gy) {
            for (int gx = 0; gx < g->width; ++gx) {
                const size_t off = size_t(gy) * size_t(g->width) + size_t(gx);
                const uint8_t packed = g->bitmap[off >> 1];
                const int opacity = int((off & 1) ? (packed & 15u) : (packed >> 4)) * 17;
                if (opacity) {
                    const int px = x0 + pen + g->xBearing + gx;
                    const int py = baseline + g->yBearing + gy;
                    if (px >= rect.x && px < rect.x + rect.w &&
                        py >= rect.y && py < rect.y + rect.h)
                        blendInk(px, py, red, green, blue, opacity);
                }
            }
        }
        pen += g->advance;
    }
}

// Soft distance-field rounded capsules avoid the stair-stepped 1px edges of
// pixel-box drawing. All geometry is native 360x360 and remains dynamic.
void softCapsule(int x, int y, int w, int h, int radius,
                 uint8_t borderR, uint8_t borderG, uint8_t borderB,
                 bool selected = false) {
    const float r = float(radius);
    const float halfW = float(w) * 0.5f;
    const float halfH = float(h) * 0.5f;
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const float qx = std::fabs(float(px) + 0.5f - halfW) - (halfW - r);
            const float qy = std::fabs(float(py) + 0.5f - halfH) - (halfH - r);
            const float outsideX = std::max(0.0f, qx);
            const float outsideY = std::max(0.0f, qy);
            const float dist = std::sqrt(outsideX * outsideX +
                                         outsideY * outsideY) +
                               std::min(0.0f, std::max(qx, qy)) - r;
            const float coverage = std::clamp(0.65f - dist, 0.0f, 1.0f);
            if (coverage <= 0) continue;
            blendInk(x + px, y + py,
                     selected ? 15 : 8, selected ? 49 : 30, 31,
                     int(195.0f * coverage));
            const float stroke = std::clamp(1.05f - std::fabs(dist + 0.55f),
                                             0.0f, 1.0f);
            if (stroke > 0.0f)
                blendInk(x + px, y + py, borderR, borderG, borderB,
                         int(245.0f * stroke));
        }
    }
}
void drawCheckBox(int x, int y, bool checked) {
    softCapsule(x, y, 20, 20, 5,
                checked ? 104 : 114, checked ? 230 : 119,
                checked ? 196 : 117, checked);
    if (checked) {
        // Distinct, non-touch-adjusting check mark.
        for (int t = 0; t < 7; ++t) {
            blendInk(x + 4 + t, y + 10 + t / 2, 189, 253, 228, 255);
            blendInk(x + 4 + t, y + 11 + t / 2, 189, 253, 228, 200);
        }
        for (int t = 0; t < 9; ++t) {
            blendInk(x + 10 + t, y + 13 - t, 189, 253, 228, 255);
            blendInk(x + 10 + t, y + 14 - t, 189, 253, 228, 200);
        }
    }
}
void drawSmallCounter(int x, int y, const MtgTile &tile) {
    softCapsule(x, y, 106, 25, 12,
                tile.selected ? 114 : 190,
                tile.selected ? 230 : 152,
                tile.selected ? 209 : 91, tile.selected);
    char label[36];
    std::snprintf(label, sizeof(label), "%s", tile.label);
    if (std::strncmp(label, "Commander ", 10) == 0) {
        std::snprintf(label, sizeof(label), "%s", tile.label + 10);
    }
    // Keep long labels legible inside a small pill. Show the complete
    // caption across two small lines when the name contains a word break.
    int separator = -1;
    const int len = int(std::strlen(label));
    if (len > 11) for (int i = 1; i < len; ++i)
        if (label[i] == ' ' && i <= 10 && len - i - 1 <= 10) {
            separator = i;
            break;
        }
    if (separator >= 0) {
        label[separator] = '\0';
        atlasText({x + 5, y + 1, 69, 12}, label, 0, kIvory);
        atlasText({x + 5, y + 12, 69, 11}, label + separator + 1, 0, kIvory);
    } else {
        atlasText({x + 5, y + 4, 70, 16}, label, 0, kIvory);
    }
    char value[20];
    std::snprintf(value, sizeof(value), "%d", tile.value);
    atlasText({x + 78, y + 3, 26, 20}, value, 1,
              tile.selected ? kTeal : kIvory);
}
void drawEditCounterButton() {
    softCapsule(283, 259, 33, 25, 12, 214, 180, 111);
    atlasText({285, 264, 29, 16}, "EDIT", 0, kGold);
}

void drawDashboard(const MtgGameView &view) {
    loadArtFor(view);
    bool reuse = gBackdrop && gBackdropValid &&
                 std::strcmp(gBackdropDeck, view.deckId) == 0 &&
                 gBackdropRevision == view.artworkRevision &&
                 gBackdropTiles == view.tileCount;
    for (int i = 0; i < 3; ++i)
        if (gBackdropOpponents[i] != view.damageIndices[i]) reuse = false;
    if (reuse) {
        std::memcpy(gFrame, gBackdrop, 360u * 360u * 2u);
    } else {
        drawHeroArt();
        applyOrnateSkin(view);
        if (gBackdrop) {
            std::memcpy(gBackdrop, gFrame, 360u * 360u * 2u);
            std::snprintf(gBackdropDeck, sizeof(gBackdropDeck), "%s", view.deckId);
            gBackdropRevision = view.artworkRevision;
            gBackdropTiles = view.tileCount;
            for (int i = 0; i < 3; ++i)
                gBackdropOpponents[i] = view.damageIndices[i];
            gBackdropValid = true;
        }
    }

    if (view.deckCount == 0) {
        chromePanel(58, 132, 244, 103, 16, kGold, kSurface);
        aaText({72, 147, 216, 32}, "NO DECKS", FreeSans18pt7b, 24, kIvory);
        aaText({68, 188, 224, 24}, "SYNC FROM DESKTOP", FreeSans18pt7b, 13, kTeal);
        return;
    }

    if (view.screen == MtgScreen::ChooseDeck) {
        chromePanel(44, 115, 272, 156, 17, kGold, kBg);
        aaText({60, 124, 240, 25}, "SELECT DECK", FreeSans18pt7b, 19, kGold);
        char position[32];
        std::snprintf(position, sizeof(position), "%d / %d",
                      view.deckIndex + 1, view.deckCount);
        aaText({80, 160, 200, 33}, position, FreeSans18pt7b, 25, kTeal);
        char name[32];
        std::snprintf(name, sizeof(name), "%.28s", view.deckName);
        aaText({62, 210, 236, 25}, name, FreeSans18pt7b, 16, kIvory);
        drawPagination(view.deckIndex, view.deckCount, 292);
        return;
    }

    if (view.screen == MtgScreen::ConfirmReset) {
        chromePanel(45, 110, 270, 164, 17, kGold, kBg);
        aaText({64, 126, 232, 32}, "NEW GAME?", FreeSans18pt7b, 22, kGold);
        aaText({56, 162, 248, 25}, "RESET ALL COUNTERS", FreeSans18pt7b, 12, kIvory);
        chromePanel(73, 196, 102, 51, 11, kGold, kGoldDim);
        chromePanel(184, 196, 102, 51, 11, kTeal, kSurface);
        aaText({78, 204, 92, 33}, "RESET", FreeSans18pt7b, 15, kIvory);
        aaText({189, 204, 92, 33}, "CANCEL", FreeSans18pt7b, 15, kIvory);
        return;
    }

    if (view.screen == MtgScreen::EditCounters) {
        chromePanel(36, 72, 288, 244, 19, kGold, kBg);
        atlasText({73, 78, 213, 23}, "EDIT COUNTERS", 0, kGold);
        char count[24];
        std::snprintf(count, sizeof(count), "%d SELECTED", view.selectedTrackerCount);
        atlasText({91, 96, 178, 14}, count, 0, kMuted);
        for (int i = 0; i < view.choiceCount; ++i) {
            const int y = 110 + i * 46;
            softCapsule(55, y, 250, 39, 9,
                        view.choices[i].checked ? 104 : 79,
                        view.choices[i].checked ? 190 : 117,
                        view.choices[i].checked ? 162 : 130);
            drawCheckBox(69, y + 10, view.choices[i].checked);
            atlasText({101, y + 7, 186, 25},
                      view.choices[i].label, 0,
                      view.choices[i].checked ? kIvory : kMuted);
        }
        drawPagination(view.addPage, view.addPageCount, 306);
        atlasText({136, 318, 88, 17}, "DONE", 0, kGold);
        return;
    }

    if (!gHasArt)
        atlasText({88, 55, 184, 21}, "SYNC ART", 0, kMuted);

    // Static LIFE / CASTS / TAX / COMMANDER DAMAGE headings and gold filigree
    // are part of the screenshot-derived chrome. Only live values are drawn.
    char number[24];
    atlasText({152, 102, 56, 20}, "LIFE", 0, kGold);
    std::snprintf(number, sizeof(number), "%d", view.life);
    atlasText({134, 120, 92, 45}, number, 2,
              view.selected == MtgCounter::Life ? kTeal : kIvory);
    std::snprintf(number, sizeof(number), "%d", view.casts);
    atlasText({124, 159, 31, 27}, number, 1,
              view.selected == MtgCounter::Casts ? kTeal : kIvory);
    std::snprintf(number, sizeof(number), "+%d", view.casts * 2);
    atlasText({265, 160, 45, 25}, number, 1, kIvory);

    // Three opponent slots are visible at once, independent of optional
    // counter pagination. Touch selects one opponent, dial adjusts damage;
    // the ornate +/- controls add/remove opponents, never adjust a value.
    const int labelX[3] = {84, 159, 232};
    const int valueX[3] = {109, 184, 258};
    for (int slot = 0; slot < 3; ++slot) {
        const int opponent = view.damageIndices[slot];
        if (!opponent) continue;
        char player[12];
        std::snprintf(player, sizeof(player), "P%d", opponent);
        atlasText({labelX[slot], 215, 25, 23}, player, 0, kGold);
        std::snprintf(number, sizeof(number), "%d", view.damageValues[slot]);
        atlasText({valueX[slot], 211, 29, 29}, number, 1,
                  view.selectedDamage == opponent - 1 ? kTeal : kIvory);
    }
    // Compact counters leave room for an always-present EDIT button.
    // The older oversized bitmap panels are removed from the backdrop.
    if (view.tileCount > 0) drawSmallCounter(50, 259, view.tiles[0]);
    if (view.tileCount > 1) drawSmallCounter(165, 259, view.tiles[1]);
    drawEditCounterButton();

    // Only counter pages move on a horizontal swipe. Life, Casts, Tax and
    // Commander Damage always remain visible.
    const int dots = std::min(5, std::max(1, view.pageCount));
    const int dotStart = 180 - (dots - 1) * 10 / 2;
    for (int i = 0; i < dots; ++i) {
        fillRound(dotStart + i * 10 - 2, 302, 5, 5, 2,
                  i == view.page ? kGold : kGoldDim);
    }
    // Optional counter selection is handled by EDIT, not a plus icon.
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
    gBackdrop = static_cast<uint16_t *>(heap_caps_malloc(
        kWidth * kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    gArt = static_cast<uint16_t *>(heap_caps_malloc(
        360 * 360 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    gSkin = static_cast<uint8_t *>(heap_caps_malloc(
        360 * 360 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!initAaFonts())
        ESP_LOGW(kTag, "Antialiased serif font atlas unavailable; using GFX fallback");
    if (gSkin && !decodeSkin()) {
        ESP_LOGE(kTag, "Bundled antialiased ornate chrome asset invalid");
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
