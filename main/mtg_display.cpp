// Status-first firmware UI for Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// Round 360x360 SH8601 over QSPI, GPIO47 PWM backlight. No LVGL needed
// for this bring-up: the drawing code uses a small local bitmap font,
// RGB565 frame buffer, and DMA-friendly 16-row transfers.
//
// Panel pins/init values reference Apache-2.0 BlueKnob board configuration.
// See THIRD_PARTY_NOTICES.md. Screen is optional to the serial receiver.

#include "mtg_display.h"
#include "panel_init_commands.h"

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
uint16_t *gFrame = nullptr;     // 360x360 RGB565 in PSRAM
uint16_t *gDmaStrip = nullptr;  // 360x16 RGB565 in DMA-capable internal RAM
uint16_t *gArt = nullptr;       // 180x180 RGB565 user-supplied commander art
uint8_t *gSkin = nullptr;      // original fantasy overlay, indexed 120x120
char gLoadedArtId[96] = {};
uint32_t gLoadedArtRevision = 0;
bool gHasArt = false;
extern "C" const uint8_t skin_start[] asm("_binary_ui_skin_lz4_start");
extern "C" const uint8_t skin_end[] asm("_binary_ui_skin_lz4_end");


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

// Indexed palette of our original, image-generated fantasy dashboard asset.
// Artwork source in the repo: main/ui_skin.lz4. NOT a procedural wireframe.
constexpr uint8_t kPalette[16][3] = {
    {156,180,146}, {92,113,88}, {55,71,60}, {45,37,33},
    {2,33,35}, {28,17,19}, {8,17,22}, {2,17,21},
    {1,18,20}, {1,13,15}, {11,8,11}, {2,6,7},
    {0,9,11}, {0,4,7}, {0,0,2}, {0,0,0}
};

// Small bounds-checked LZ4 block decoder, no host/image runtime dependencies.
// The generated skin is 120x120 indexed pixels and scales 3x on the 360 LCD.
bool decodeSkin() {
    if (!gSkin) return false;
    const uint8_t *src = skin_start;
    const uint8_t *const end = skin_end;
    uint8_t *out = gSkin;
    uint8_t *const outEnd = gSkin + 120 * 120;
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
            out[i] = out[i - distance];
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
    if (!file) return;
    const size_t len = std::fread(gArt, sizeof(uint16_t), 180 * 180, file);
    const int trailing = std::fgetc(file);
    std::fclose(file);
    gHasArt = len == 180 * 180 && trailing == EOF;
    ESP_LOGI(kTag, "Deck art %s: %s", path, gHasArt ? "loaded" : "invalid");
}

// Palette skin is a real image asset, blended over the user-supplied commander
// image; gold/cyan edges stay vivid while the center shows more commander art.
void drawGameSkin() {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - 180, dy = y - 180;
            if (dx*dx + dy*dy > 177*177) {
                pixel(x, y, kBg);
                continue;
            }
            const uint8_t index = gSkin ? gSkin[(y / 3) * 120 + (x / 3)] : 8;
            const uint8_t *palette = kPalette[index < 16 ? index : 15];
            int r = palette[0], g = palette[1], b = palette[2];
            if (gHasArt) {
                const uint16_t swapped = gArt[(y / 2) * 180 + (x / 2)];
                const uint16_t rgb = uint16_t((swapped >> 8) | (swapped << 8));
                const int photoR = ((rgb >> 11) & 31) * 255 / 31;
                const int photoG = ((rgb >> 5) & 63) * 255 / 63;
                const int photoB = (rgb & 31) * 255 / 31;
                // Keep the artwork subtle under the HUD, most prominent in
                // the center life panel and the gap above it.
                const bool focus = ((dx*dx + (dy-5)*(dy-5) < 86*86) ||
                                    (y < 106 && x > 142 && x < 218));
                const int opacity = (r + g + b > 245) ? 242 : (focus ? 76 : 213);
                r = (r*opacity + photoR*(255-opacity)*2/3)/255;
                g = (g*opacity + photoG*(255-opacity)*2/3)/255;
                b = (b*opacity + photoB*(255-opacity)*2/3)/255;
            }
            pixel(x, y, color(unsigned(r),unsigned(g),unsigned(b)));
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

void drawDashboard(const MtgGameView &view) {
    loadArtFor(view);
    drawGameSkin();

    if (view.deckCount == 0) {
        printCentered(154, "NO DECKS", 3, kIvory);
        printCentered(201, "SYNC FROM DESKTOP", 1, kGold);
        return;
    }

    if (view.screen == MtgScreen::ChooseDeck) {
        printCentered(51, "SELECT DECK", 2, kGold);
        // Translucent, deliberately dark central overlay on the art asset.
        fillRect(59, 115, 242, 147, kBg);
        char number[32];
        std::snprintf(number, sizeof(number), "%d / %d",
                      view.deckIndex + 1, view.deckCount);
        printCentered(126, number, 2, kTeal);
        char deck[25]{};
        std::snprintf(deck, sizeof(deck), "%.21s", view.deckName);
        printCentered(164, deck, 2, kIvory);
        printCentered(220, "TAP TO PLAY", 2, kGold);
        printCentered(286, "TURN KNOB TO CHOOSE", 1, kMuted);
        return;
    }

    const uint16_t lifeInk = view.selected == MtgCounter::Life ? kGold : kIvory;
    const uint16_t poisonInk = view.selected == MtgCounter::Poison ? kGold : kIvory;
    const uint16_t radInk = view.selected == MtgCounter::Rad ? kGold : kIvory;
    const uint16_t damageInk = view.selected == MtgCounter::Damage ? kGold : kIvory;
    const uint16_t castsInk = view.selected == MtgCounter::Casts ? kGold : kIvory;
    const uint16_t deathsInk = view.selected == MtgCounter::Deaths ? kGold : kIvory;

    char number[24]{};
    std::snprintf(number, sizeof(number), "%d", view.life);
    int scale = std::strlen(number) > 3 ? 7 : (std::strlen(number) > 2 ? 9 : 12);
    printCentered(132, number, scale, lifeInk);
    printCentered(226, "LIFE", 2, lifeInk);

    std::snprintf(number, sizeof(number), "%d", view.poison);
    printAt(97, 85, number, 4, poisonInk);
    printAt(97, 119, "POISON", 1, poisonInk);

    std::snprintf(number, sizeof(number), "%d", view.rad);
    printAt(263, 85, number, 4, radInk);
    printAt(263, 119, "RAD", 1, radInk);

    std::snprintf(number, sizeof(number), "%d", view.damage);
    printAt(78, 177, number, 4, damageInk);
    printAt(78, 215, "CMD", 1, damageInk);
    printAt(78, 225, "DAMAGE", 1, damageInk);

    std::snprintf(number, sizeof(number), "%d", view.deaths);
    printAt(282, 177, number, 4, deathsInk);
    printAt(282, 215, "CMD", 1, deathsInk);
    printAt(282, 225, "DEATHS", 1, deathsInk);

    // Commander tax after n casts is 2*n extra for the NEXT cast.
    const int nextTax = view.casts * 2;
    printAt(180, 260, "COMMANDER", 1, kGold);
    std::snprintf(number, sizeof(number), "%d", view.casts);
    printAt(155, 280, number, 3, castsInk);
    printAt(112, 292, "CASTS", 1, castsInk);
    std::snprintf(number, sizeof(number), "+%d", nextTax);
    printAt(259, 280, number, 2, castsInk);
    printAt(221, 292, "TAX", 1, castsInk);

    // Keep the on-device UI legible even with long user-defined deck names.
    char deck[31]{};
    std::snprintf(deck, sizeof(deck), "%.28s", view.deckName);
    printCentered(325, deck, 1, kIvory);
    printCentered(339, "TAP A STAT - TURN KNOB", 1, kMuted);
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
    channel.intr_type = LEDC_INTR_DISABLE;
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
        180 * 180 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    gSkin = static_cast<uint8_t *>(heap_caps_malloc(
        120 * 120, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    vTaskDelete(nullptr);
}

} // namespace

bool mtg_display_init() {
    // Never initialize the LCD synchronously from app_main. Full LCD
    // initialization and DMA flushes can block. The USB sync receiver
    // must be serviced independently, even if the screen is broken.
    return xTaskCreate(displayTask, "mtg_lcd", 10240, nullptr,
                       tskIDLE_PRIORITY + 1, nullptr)
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
}


void mtg_display_showGame(const MtgGameView &view) {
    taskENTER_CRITICAL(&gStatusLock);
    gPending.gameMode = true;
    gPending.game = view;
    ++gPending.generation;
    taskEXIT_CRITICAL(&gStatusLock);
}
