// Off-line Commander game session + deck selector for the Waveshare S3 knob.
// Clockwise GPIO8 and counterclockwise GPIO7 are *direction pulses*, not
// quadrature. This board's measured pinout is in the README references.
// Touch CST816D uses I2C (SDA11/SCL12, 0x15) and is rotated 180 degrees.
//
// Session counters are persisted per deck in NVS (not wiped by deck sync).
// Each input callback updates state; display rendering happens on another task.

#include "mtg_game.h"
#include "mtg_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "mtg_game";
constexpr gpio_num_t kClockwise = GPIO_NUM_8;
constexpr gpio_num_t kCounterclockwise = GPIO_NUM_7;
constexpr char kDeckJson[] = "/mtg/decks.json";

struct DeckMeta {
    std::string id, name, commander;
};
struct Saved {
    uint32_t magic = 0x4D544731; // MTG1
    int32_t life = 40, poison = 0, rad = 0, damage = 0, casts = 0, deaths = 0;
};

std::vector<DeckMeta> gDecks;
MtgGameView gView;
portMUX_TYPE gViewLock = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t gRotaryEvents = nullptr;
volatile TickType_t gLastEdgeCw = 0, gLastEdgeCcw = 0;
bool gReloadPending = false;
bool gDirty = false;
int64_t gLastChange = 0;
i2c_master_bus_handle_t gI2c = nullptr;
i2c_master_dev_handle_t gTouch = nullptr;

uint32_t crcId(const std::string &id) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : id) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}
void nvsKey(char key[16], const std::string &id) {
    std::snprintf(key, 16, "d%08lx",
                  static_cast<unsigned long>(crcId(id)));
}

void writeState() {
    if (gDecks.empty() || gView.deckIndex < 0 ||
        gView.deckIndex >= int(gDecks.size())) return;
    nvs_handle_t handle;
    if (nvs_open("mtg_game", NVS_READWRITE, &handle) != ESP_OK) return;
    char key[16];
    nvsKey(key, gDecks[size_t(gView.deckIndex)].id);
    const Saved save{0x4D544731, gView.life, gView.poison, gView.rad,
                     gView.damage, gView.casts, gView.deaths};
    esp_err_t err = nvs_set_blob(handle, key, &save, sizeof(save));
    if (err == ESP_OK)
        err = nvs_set_str(handle, "active", gDecks[size_t(gView.deckIndex)].id.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) gDirty = false;
    else ESP_LOGW(kTag, "Save state failed: %s", esp_err_to_name(err));
}

void loadState(const std::string &id) {
    Saved save;
    nvs_handle_t handle;
    if (nvs_open("mtg_game", NVS_READONLY, &handle) == ESP_OK) {
        char key[16]; nvsKey(key, id);
        size_t length = sizeof(save);
        if (nvs_get_blob(handle, key, &save, &length) != ESP_OK ||
            length != sizeof(save) || save.magic != 0x4D544731)
            save = Saved{};
        nvs_close(handle);
    }
    gView.life = save.life;
    gView.poison = save.poison;
    gView.rad = save.rad;
    gView.damage = save.damage;
    gView.casts = save.casts;
    gView.deaths = save.deaths;
}

void publish() {
    MtgGameView snapshot = gView;
    taskENTER_CRITICAL(&gViewLock);
    gView = snapshot;
    taskEXIT_CRITICAL(&gViewLock);
    mtg_display_showGame(snapshot);
}

void selectDeck(int index) {
    if (index < 0 || index >= int(gDecks.size())) return;
    if (gDirty) writeState();
    gView.deckIndex = index;
    gView.deckCount = int(gDecks.size());
    const DeckMeta &deck = gDecks[size_t(index)];
    std::snprintf(gView.deckId, sizeof(gView.deckId), "%s", deck.id.c_str());
    std::snprintf(gView.deckName, sizeof(gView.deckName), "%s", deck.name.c_str());
    std::snprintf(gView.commander, sizeof(gView.commander), "%s", deck.commander.c_str());
    gView.selected = MtgCounter::Life;
    gView.screen = MtgScreen::Dashboard;
    loadState(deck.id);
    gDirty = true; // Save active deck choice even if counters remain unchanged
    gLastChange = esp_timer_get_time();
    publish();
}

bool loadDecksFromFlash() {
    FILE *file = std::fopen(kDeckJson, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    if (length <= 0 || length > 65536) { std::fclose(file); return false; }
    std::rewind(file);
    std::string bytes(size_t(length), '\0');
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) return false;
    cJSON *root = cJSON_ParseWithLength(bytes.data(), bytes.size());
    if (!root) return false;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "decks");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return false; }
    const std::string previousId = gView.deckId;
    if (gDirty) writeState();
    gDecks.clear();
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *commander = cJSON_GetObjectItemCaseSensitive(item, "commander");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
        DeckMeta d{ id->valuestring, name->valuestring,
                   cJSON_IsString(commander) ? commander->valuestring : "" };
        gDecks.push_back(std::move(d));
    }
    cJSON_Delete(root);

    if (gDecks.empty()) {
        gView = MtgGameView{};
        gView.deckCount = 0;
        gView.screen = MtgScreen::ChooseDeck;
        publish();
        return true;
    }
    // Preserve the current deck by its stable ID when the desktop reorders
    // profiles; otherwise restore last chosen deck from NVS.
    std::string target = previousId;
    if (target.empty()) {
        nvs_handle_t handle;
        if (nvs_open("mtg_game", NVS_READONLY, &handle) == ESP_OK) {
            char buffer[128]{};
            size_t size = sizeof(buffer);
            if (nvs_get_str(handle, "active", buffer, &size) == ESP_OK) target = buffer;
            nvs_close(handle);
        }
    }
    int index = 0;
    for (size_t i = 0; i < gDecks.size(); ++i)
        if (gDecks[i].id == target) { index = int(i); break; }
    gDirty = false;
    selectDeck(index);
    ESP_LOGI(kTag, "Loaded %d decks, active: %s",
             int(gDecks.size()), gView.deckName);
    return true;
}

void IRAM_ATTR rotaryInterrupt(void *context) {
    const int direction = int(reinterpret_cast<intptr_t>(context));
    volatile TickType_t *last = direction > 0 ? &gLastEdgeCw : &gLastEdgeCcw;
    const TickType_t now = xTaskGetTickCountFromISR();
    if (now - *last < pdMS_TO_TICKS(5)) return;
    *last = now;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(gRotaryEvents, &direction, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void initRotary() {
    gRotaryEvents = xQueueCreate(64, sizeof(int));
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << kClockwise) | (1ULL << kCounterclockwise);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK(gpio_config(&config));
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Encoder ISR service: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(kClockwise, rotaryInterrupt,
                      reinterpret_cast<void *>(intptr_t(1))));
    ESP_ERROR_CHECK(gpio_isr_handler_add(kCounterclockwise, rotaryInterrupt,
                      reinterpret_cast<void *>(intptr_t(-1))));
}

void initTouch() {
    i2c_master_bus_config_t config = {};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = GPIO_NUM_11;
    config.scl_io_num = GPIO_NUM_12;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&config, &gI2c);
    if (err != ESP_OK) { ESP_LOGW(kTag, "I2C: %s", esp_err_to_name(err)); return; }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = 0x15;
    dev.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(gI2c, &dev, &gTouch);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Touch controller: %s", esp_err_to_name(err));
        gTouch = nullptr;
    }
}

bool touchPoint(int &x, int &y) {
    if (!gTouch) return false;
    uint8_t startReg = 0x01, result[6]{};
    if (i2c_master_transmit_receive(gTouch, &startReg, 1,
                                    result, sizeof(result), 40) != ESP_OK) return false;
    if ((result[1] & 0x0F) == 0) return false;
    const int rawX = ((result[2] & 0x0F) << 8) | result[3];
    const int rawY = ((result[4] & 0x0F) << 8) | result[5];
    // The LCD panel is physically mounted upside down relative to CST816D.
    x = 359 - rawX;
    y = 359 - rawY;
    return x >= 0 && x < 360 && y >= 0 && y < 360;
}

void changeCounter(int direction) {
    int *number = nullptr;
    switch (gView.selected) {
    case MtgCounter::Life:   number = &gView.life; break;
    case MtgCounter::Poison: number = &gView.poison; break;
    case MtgCounter::Rad:    number = &gView.rad; break;
    case MtgCounter::Damage: number = &gView.damage; break;
    case MtgCounter::Casts:  number = &gView.casts; break;
    case MtgCounter::Deaths: number = &gView.deaths; break;
    }
    if (!number) return;
    *number = std::clamp(*number + direction,
                         gView.selected == MtgCounter::Life ? -999 : 0, 9999);
    gDirty = true;
    gLastChange = esp_timer_get_time();
    publish();
}

void knobStep(int direction) {
    if (gDecks.empty()) return;
    if (gView.screen == MtgScreen::ChooseDeck) {
        int index = (gView.deckIndex + direction) % int(gDecks.size());
        if (index < 0) index += int(gDecks.size());
        gView.deckIndex = index;
        const DeckMeta &d = gDecks[size_t(index)];
        std::snprintf(gView.deckName, sizeof(gView.deckName), "%s", d.name.c_str());
        std::snprintf(gView.commander, sizeof(gView.commander), "%s", d.commander.c_str());
        publish();
    } else {
        changeCounter(direction);
    }
}

void tap(int x, int y) {
    if (gDecks.empty()) return;
    if (gView.screen == MtgScreen::ChooseDeck) {
        selectDeck(gView.deckIndex);
        return;
    }
    // Top crown: deck chooser. Tap counter area to select it, rotate
    // to change its value. All counters remain visible on the dashboard.
    if (y < 54) {
        gView.screen = MtgScreen::ChooseDeck;
    } else if (y >= 264) {
        gView.selected = MtgCounter::Casts;
    } else if (y >= 166 && x < 100) {
        gView.selected = MtgCounter::Damage;
    } else if (y >= 166 && x > 260) {
        gView.selected = MtgCounter::Deaths;
    } else if (y < 166 && x < 135) {
        gView.selected = MtgCounter::Poison;
    } else if (y < 166 && x > 225) {
        gView.selected = MtgCounter::Rad;
    } else {
        gView.selected = MtgCounter::Life;
    }
    publish();
}

void task(void *) {
    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGE(kTag, "NVS full; do not erase automatically (saved games)");
    }
    // Avoid modifying the user's stored deck library on boot.
    loadDecksFromFlash();
    initRotary();
    initTouch();
    bool down = false;
    int touchX = 0, touchY = 0;
    while (true) {
        if (gReloadPending) {
            gReloadPending = false;
            loadDecksFromFlash();
        }
        int direction = 0;
        while (gRotaryEvents && xQueueReceive(gRotaryEvents, &direction, 0) == pdTRUE)
            knobStep(direction);

        int x = 0, y = 0;
        const bool currentlyDown = touchPoint(x, y);
        if (currentlyDown) {
            touchX = x; touchY = y;
        }
        if (!currentlyDown && down)
            tap(touchX, touchY);
        down = currentlyDown;

        if (gDirty && esp_timer_get_time() - gLastChange >= 1500000)
            writeState();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
} // namespace

void mtg_game_start() {
    if (xTaskCreate(task, "mtg_game", 9216, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS)
        ESP_LOGE(kTag, "Could not start game input task");
}

void mtg_game_reload_decks() { gReloadPending = true; }

void mtg_game_view(MtgGameView &output) {
    taskENTER_CRITICAL(&gViewLock);
    output = gView;
    taskEXIT_CRITICAL(&gViewLock);
}
