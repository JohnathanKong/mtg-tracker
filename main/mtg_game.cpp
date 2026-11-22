// Off-line Commander game session + deck selector for the Waveshare S3 knob.
// Clockwise GPIO8 and counterclockwise GPIO7 are *direction pulses*, not
// quadrature. This board's measured pinout is in the README references.
// Touch CST816D uses I2C (SDA11/SCL12, 0x15). Coordinates match the LCD.
//
// Session counters are persisted per deck in NVS (not wiped by deck sync).
// Each input callback updates state; display rendering happens on another task.

#include "mtg_game.h"
#include "mtg_scroll.h"
#include "mtg_display.h"
#include "mtg_rotary.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_timer.h"
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

struct CounterDef { std::string id, label; };
struct DeckMeta {
    std::string id, name, commander;
    std::vector<CounterDef> defaults, custom;
};
struct ActiveCounter {
    std::string id, label;
    int value = 0;
    bool temporary = false;
};
constexpr size_t kMaxActiveCounters = 24;
struct StoredCounter {
    uint32_t idHash = 0;
    int32_t value = 0;
    uint8_t temporary = 0;
    uint8_t padding[3]{};
};
struct StoredCounters {
    uint32_t magic = 0x4D543245; // MT2E: exact visible/hidden tracker selections
    uint32_t count = 0;
    StoredCounter items[kMaxActiveCounters]{};
};
const CounterDef kCatalog[] = {
    {"commander_deaths", "Commander Deaths"}, {"commander_damage", "Commander Damage"},
    {"poison", "Poison"}, {"rad", "Radiation"}, {"experience", "Experience"},
    {"energy", "Energy"}, {"treasures", "Treasures"}, {"storm", "Storm"},
    {"tickets", "Tickets"}, {"speed", "Speed"}, {"ring", "Ring Temptation"},
    {"dungeon", "Dungeon"}, {"monarch", "Monarch"}, {"initiative", "Initiative"},
    {"citys_blessing", "City Blessing"}, {"day_night", "Day Night"},
    {"emblems", "Emblems"}, {"mana_pool", "Mana Pool"}, {"turn", "Turn"}
};
bool isPermanent(const std::string &id) {
    return id == "life" || id == "commander_casts" ||
           id == "commander_damage";
}
std::string labelFor(const std::string &id) {
    for (const auto &item : kCatalog)
        if (item.id == id) return item.label;
    return id;
}
struct Saved {
    uint32_t magic = 0x4D544731; // MTG1
    int32_t life = 40, poison = 0, rad = 0, damage = 0, casts = 0, deaths = 0;
};

std::vector<DeckMeta> gDecks;
std::vector<ActiveCounter> gTrackers;
std::vector<ActiveCounter> gHiddenTrackers; // Preserve values while unchecked.
std::vector<CounterDef> gAvailable;
bool gAvailableDirty = true;
int gActiveTracker = -1;
constexpr int kMaxOpponents = 12;
int gOpponentCount = 3;
int gOpponentPage = 0;
int gOpponentSelected = -1;
int gOpponentDamage[kMaxOpponents]{};
struct SavedOpponentDamage {
    uint32_t magic = 0x4D54444D; // MTDM
    uint32_t count = 3;
    int32_t values[kMaxOpponents]{};
};
MtgGameView gView;
portMUX_TYPE gViewLock = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t gRotaryEvents = nullptr;
// Read GPIO7/8 at 2 ms cadence. Dispatch only AFTER a stable pulse release,
// close to the mechanical click, rather than as the contact first closes.
MtgRotaryPulse gPulses[2];
esp_timer_handle_t gEncoderTimer = nullptr;
std::atomic<bool> gReloadPending{false};
uint32_t gArtworkRevision = 0;
bool gDirty = false;
int gActiveDeckIndex = -1; // Separate from picker preview index.
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


int legacyValue(const std::string &id) {
    if (id == "poison") return gView.poison;
    if (id == "rad") return gView.rad;
    if (id == "commander_damage") return gView.damage;
    if (id == "commander_deaths") return gView.deaths;
    return 0;
}
void setLegacyValue(const std::string &id, int value) {
    if (id == "poison") gView.poison = value;
    if (id == "rad") gView.rad = value;
    if (id == "commander_damage") gView.damage = value;
    if (id == "commander_deaths") gView.deaths = value;
}
bool alreadyActive(const std::string &id) {
    for (const auto &counter : gTrackers)
        if (counter.id == id) return true;
    return false;
}
void refreshAvailable() {
    gAvailable.clear();
    // Keep the list in a stable order when checking and unchecking entries.
    // Moving options after each tap would make the touchscreen frustrating.
    auto append = [&](const std::string &id, const std::string &label) {
        if (isPermanent(id)) return;
        for (const CounterDef &existing : gAvailable)
            if (existing.id == id) return;
        gAvailable.push_back({id, label});
    };
    for (const auto &entry : kCatalog) append(entry.id, entry.label);
    if (gActiveDeckIndex >= 0 && gActiveDeckIndex < int(gDecks.size()))
        for (const auto &entry : gDecks[size_t(gActiveDeckIndex)].custom)
            append(entry.id, entry.label);
    // Preserve older synced custom trackers, even after unchecking them.
    for (const auto &active : gTrackers) append(active.id, active.label);
    for (const auto &hidden : gHiddenTrackers) append(hidden.id, hidden.label);
}

void refreshCounterView() {
    gView.pageCount = std::max(1, int((gTrackers.size() + 1) / 2));
    gView.page = std::clamp(gView.page, 0, gView.pageCount - 1);
    gView.tileCount = 0;
    for (auto &tile : gView.tiles) tile = MtgTile{};
    for (int pos = gView.page * 2;
         pos < int(gTrackers.size()) && pos < (gView.page + 1) * 2; ++pos) {
        const ActiveCounter &entry = gTrackers[size_t(pos)];
        MtgTile &tile = gView.tiles[gView.tileCount++];
        std::snprintf(tile.label, sizeof(tile.label), "%s", entry.label.c_str());
        tile.value = entry.value;
        tile.selected = gView.selected == MtgCounter::Dynamic && gActiveTracker == pos;
        if (entry.id == "commander_deaths") tile.accent = 0;
        else if (entry.id == "commander_damage") tile.accent = 1;
        else if (entry.id == "experience" || entry.id == "poison") tile.accent = 2;
        else if (entry.id == "treasures" || entry.id == "rad") tile.accent = 3;
        else tile.accent = 4;
    }
    gView.damagePlayers = gOpponentCount;
    gView.damagePageCount = std::max(1, (gOpponentCount + 2) / 3);
    gOpponentPage = std::clamp(gOpponentPage, 0, gView.damagePageCount - 1);
    gView.damagePage = gOpponentPage;
    gView.selectedDamage = gView.selected == MtgCounter::CommanderDamage ?
                           gOpponentSelected : -1;
    for (int n = 0; n < 3; ++n) {
        const int idx = gOpponentPage * 3 + n;
        gView.damageIndices[n] = idx < gOpponentCount ? idx + 1 : 0;
        gView.damageValues[n] = idx < gOpponentCount ? gOpponentDamage[idx] : 0;
    }
    // Supply the adjacent page's actual live counters during a finger drag.
    // This avoids waiting for a page-change event before rendering both pages.
    gView.dragTileCount = 0;
    gView.dragTargetPage = -1;
    for (auto &tile : gView.dragTiles) tile = MtgTile{};
    if (gView.dragActive && gView.dragOffsetPx != 0) {
        const int neighbor = gView.page + (gView.dragOffsetPx < 0 ? 1 : -1);
        if (neighbor >= 0 && neighbor < gView.pageCount) {
            gView.dragTargetPage = neighbor;
            for (int pos = neighbor * 2;
                 pos < int(gTrackers.size()) && pos < neighbor * 2 + 2; ++pos) {
                const ActiveCounter &entry = gTrackers[size_t(pos)];
                MtgTile &tile = gView.dragTiles[gView.dragTileCount++];
                std::snprintf(tile.label, sizeof(tile.label), "%s", entry.label.c_str());
                tile.value = entry.value;
                tile.selected = false;
                if (entry.id == "commander_deaths") tile.accent = 0;
                else if (entry.id == "commander_damage") tile.accent = 1;
                else if (entry.id == "experience" || entry.id == "poison") tile.accent = 2;
                else if (entry.id == "treasures" || entry.id == "rad") tile.accent = 3;
                else tile.accent = 4;
            }
        }
    }
    gView.selectedTrackerCount = int(gTrackers.size());
    // Avoid building a new full editor catalog for every physical dial tick.
    if (gView.screen != MtgScreen::EditCounters) return;
    if (gAvailableDirty) {
        refreshAvailable();
        gAvailableDirty = false;
    }
    // The list is laid out in physical pixels. Four rows fit; the fifth
    // is partially visible during a drag instead of jumping by whole rows.
    gView.editTotalRows = int(gAvailable.size());
    gView.editMaxScrollPx = mtg_scroll::maxScroll(gView.editTotalRows);
    gView.editScrollPx = mtg_scroll::clampScroll(
        gView.editScrollPx, gView.editTotalRows);
    gView.editFirstRow = mtg_scroll::firstRow(gView.editScrollPx);
    gView.editRowOffsetPx = mtg_scroll::rowOffset(gView.editScrollPx);
    gView.choiceCount = 0;
    for (auto &choice : gView.choices) choice = MtgMenuChoice{};
    for (int pos = gView.editFirstRow;
         pos < int(gAvailable.size()) &&
         pos < gView.editFirstRow + mtg_scroll::kVisibleRows; ++pos) {
        MtgMenuChoice &choice = gView.choices[gView.choiceCount++];
        const CounterDef &definition = gAvailable[size_t(pos)];
        std::snprintf(choice.label,sizeof(choice.label),"%s",
                      definition.label.c_str());
        choice.checked = alreadyActive(definition.id);
    }
}
void rebuildDefaults() {
    gTrackers.clear();
    gHiddenTrackers.clear();
    if (gActiveDeckIndex < 0 || gActiveDeckIndex >= int(gDecks.size())) return;
    for (const auto &definition : gDecks[size_t(gActiveDeckIndex)].defaults) {
        if (gTrackers.size() >= kMaxActiveCounters) break;
        if (!isPermanent(definition.id) && !alreadyActive(definition.id))
            gTrackers.push_back({definition.id, definition.label,
                                 legacyValue(definition.id), false});
    }
}

void writeState() {
    if (gDecks.empty() || gActiveDeckIndex < 0 ||
        gActiveDeckIndex >= int(gDecks.size())) return;
    nvs_handle_t handle;
    if (nvs_open("mtg_game", NVS_READWRITE, &handle) != ESP_OK) return;
    char key[16];
    nvsKey(key, gDecks[size_t(gActiveDeckIndex)].id);
    const Saved save{0x4D544731, gView.life, gView.poison, gView.rad,
                     gView.damage, gView.casts, gView.deaths};
    esp_err_t err = nvs_set_blob(handle, key, &save, sizeof(save));
    StoredCounters extras;
    // Write the exact dashboard selection, including hidden trackers' values.
    // temporary bit 0 = game-added; bit 1 = currently hidden.
    for (const auto &counter : gTrackers) {
        if (extras.count >= kMaxActiveCounters) break;
        StoredCounter &entry = extras.items[extras.count++];
        entry.idHash = crcId(counter.id);
        entry.value = counter.value;
        entry.temporary = counter.temporary ? 1 : 0;
    }
    for (const auto &counter : gHiddenTrackers) {
        if (extras.count >= kMaxActiveCounters) break;
        StoredCounter &entry = extras.items[extras.count++];
        entry.idHash = crcId(counter.id);
        entry.value = counter.value;
        entry.temporary = uint8_t((counter.temporary ? 1 : 0) | 2);
    }
    key[0] = 't';
    if (err == ESP_OK)
        err = nvs_set_blob(handle, key, &extras, sizeof(extras));
    SavedOpponentDamage opponents;
    opponents.count = uint32_t(gOpponentCount);
    for (int i = 0; i < kMaxOpponents; ++i)
        opponents.values[i] = gOpponentDamage[i];
    key[0] = 'p';
    if (err == ESP_OK)
        err = nvs_set_blob(handle, key, &opponents, sizeof(opponents));
    if (err == ESP_OK)
        err = nvs_set_str(handle, "active", gDecks[size_t(gActiveDeckIndex)].id.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) gDirty = false;
    else ESP_LOGW(kTag, "Save state failed: %s", esp_err_to_name(err));
}

void loadState(const std::string &id) {
    Saved save;
    StoredCounters extras;
    bool hasExtras = false;
    SavedOpponentDamage opponents;
    bool hasOpponents = false;
    nvs_handle_t handle;
    if (nvs_open("mtg_game", NVS_READONLY, &handle) == ESP_OK) {
        char key[16]; nvsKey(key, id);
        size_t length = sizeof(save);
        if (nvs_get_blob(handle, key, &save, &length) != ESP_OK ||
            length != sizeof(save) || save.magic != 0x4D544731)
            save = Saved{};
        key[0] = 't';
        length = sizeof(extras);
        hasExtras = nvs_get_blob(handle, key, &extras, &length) == ESP_OK &&
                    length == sizeof(extras) && (extras.magic == 0x4D543245 || extras.magic == 0x4D543244) &&
                    extras.count <= kMaxActiveCounters;
        key[0] = 'p';
        length = sizeof(opponents);
        hasOpponents = nvs_get_blob(handle, key, &opponents, &length) == ESP_OK &&
                       length == sizeof(opponents) && opponents.magic == 0x4D54444D &&
                       opponents.count >= 1 && opponents.count <= kMaxOpponents;
        nvs_close(handle);
    }
    gView.life = save.life; gView.poison = save.poison; gView.rad = save.rad;
    gView.damage = save.damage; gView.casts = save.casts; gView.deaths = save.deaths;
    gOpponentCount = hasOpponents ? int(opponents.count) : 3;
    for (int i = 0; i < kMaxOpponents; ++i)
        gOpponentDamage[i] = hasOpponents ? opponents.values[i] : 0;
    gOpponentPage = 0;
    gOpponentSelected = -1;
    rebuildDefaults();
    if (hasExtras && extras.magic == 0x4D543245) {
        // New format stores the actual checked/unchecked state. Never
        // resurrect an unchecked Windows default during a power cycle.
        gTrackers.clear();
        gHiddenTrackers.clear();
        for (size_t i = 0; i < extras.count; ++i) {
            const StoredCounter &stored = extras.items[i];
            const CounterDef *known = nullptr;
            for (const auto &definition : kCatalog)
                if (crcId(definition.id) == stored.idHash) { known = &definition; break; }
            if (!known && gActiveDeckIndex >= 0 && gActiveDeckIndex < int(gDecks.size()))
                for (const auto &definition : gDecks[size_t(gActiveDeckIndex)].custom)
                    if (crcId(definition.id) == stored.idHash) {
                        known = &definition;
                        break;
                    }
            if (!known || isPermanent(known->id)) continue;
            ActiveCounter counter{known->id, known->label, stored.value,
                                  (stored.temporary & 1u) != 0};
            setLegacyValue(counter.id, counter.value);
            if (stored.temporary & 2u) gHiddenTrackers.push_back(counter);
            else gTrackers.push_back(counter);
        }
    } else if (hasExtras) {
        // Migrate the existing MT2D save without losing counters/values.
        for (size_t i = 0; i < extras.count; ++i) {
            const StoredCounter &stored = extras.items[i];
            bool found = false;
            for (auto &entry : gTrackers) {
                if (crcId(entry.id) == stored.idHash) {
                    entry.value = stored.value;
                    setLegacyValue(entry.id, entry.value);
                    found = true;
                    break;
                }
            }
            if (found || !stored.temporary || gTrackers.size() >= kMaxActiveCounters) continue;
            for (const auto &definition : kCatalog) {
                if (!isPermanent(definition.id) &&
                    crcId(definition.id) == stored.idHash &&
                    !alreadyActive(definition.id)) {
                    gTrackers.push_back({definition.id, definition.label,
                                         stored.value, true});
                    found = true;
                    break;
                }
            }
            if (found || gActiveDeckIndex < 0 ||
                gActiveDeckIndex >= int(gDecks.size())) continue;
            for (const auto &definition : gDecks[size_t(gActiveDeckIndex)].custom) {
                if (crcId(definition.id) == stored.idHash &&
                    !alreadyActive(definition.id)) {
                    gTrackers.push_back({definition.id, definition.label,
                                         stored.value, true});
                    break;
                }
            }
        }
    }
    gActiveTracker = -1;
    gView.page = 0;
    gView.editScrollPx = 0;
}
void publish() {
    refreshCounterView();
    MtgGameView snapshot = gView;
    taskENTER_CRITICAL(&gViewLock);
    gView = snapshot;
    taskEXIT_CRITICAL(&gViewLock);
    mtg_display_showGame(snapshot);
}

void selectDeck(int index) {
    if (index < 0 || index >= int(gDecks.size())) return;
    if (gDirty) writeState();
    gActiveDeckIndex = index;
    gView.deckIndex = index;
    gView.deckCount = int(gDecks.size());
    const DeckMeta &deck = gDecks[size_t(index)];
    std::snprintf(gView.deckId, sizeof(gView.deckId), "%s", deck.id.c_str());
    std::snprintf(gView.deckName, sizeof(gView.deckName), "%s", deck.name.c_str());
    std::snprintf(gView.commander, sizeof(gView.commander), "%s", deck.commander.c_str());
    gView.selected = MtgCounter::Life;
    gView.screen = MtgScreen::Dashboard;
    gView.artworkRevision = gArtworkRevision;
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
    ++gArtworkRevision;
    gView.artworkRevision = gArtworkRevision;
    if (gDirty) writeState();
    gDecks.clear();
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *commander = cJSON_GetObjectItemCaseSensitive(item, "commander");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
        DeckMeta d;
        d.id = id->valuestring;
        d.name = name->valuestring;
        d.commander = cJSON_IsString(commander) ? commander->valuestring : "";
        const cJSON *custom = cJSON_GetObjectItemCaseSensitive(item, "customTrackers");
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, custom) {
            const cJSON *cid = cJSON_GetObjectItemCaseSensitive(entry, "id");
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(entry, "label");
            if (cJSON_IsString(cid) && cJSON_IsString(label))
                d.custom.push_back({cid->valuestring, label->valuestring});
        }
        const cJSON *trackers = cJSON_GetObjectItemCaseSensitive(item, "trackers");
        cJSON_ArrayForEach(entry, trackers) {
            if (!cJSON_IsString(entry)) continue;
            const std::string trackerId = entry->valuestring;
            if (isPermanent(trackerId)) continue;
            std::string label = labelFor(trackerId);
            for (const auto &definition : d.custom)
                if (definition.id == trackerId) { label = definition.label; break; }
            bool duplicate = false;
            for (const auto &definition : d.defaults)
                if (definition.id == trackerId) { duplicate = true; break; }
            if (!duplicate) d.defaults.push_back({trackerId, label});
        }
        gDecks.push_back(std::move(d));
    }
    cJSON_Delete(root);

    if (gDecks.empty()) {
        gView = MtgGameView{};
        gView.deckCount = 0;
        gActiveDeckIndex = -1;
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

void pollEncoder(void *) {
    if (!gRotaryEvents) return;
    const gpio_num_t pins[2] = {kClockwise, kCounterclockwise};
    const int directions[2] = {+1, -1};
    for (int i = 0; i < 2; ++i) {
        MtgRotaryPulse &pulse = gPulses[i];
        if (pulse.sample(gpio_get_level(pins[i]) == 0)) {
            const int direction = directions[i];
            xQueueSend(gRotaryEvents, &direction, 0);
        }
    }
}

void initRotary() {
    gRotaryEvents = xQueueCreate(64, sizeof(int));
    if (!gRotaryEvents) { ESP_LOGE(kTag, "Encoder queue unavailable"); return; }
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << kClockwise) | (1ULL << kCounterclockwise);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    for (auto &pulse : gPulses) pulse = MtgRotaryPulse{};
    esp_timer_create_args_t timerConfig = {};
    timerConfig.callback = pollEncoder;
    timerConfig.name = "mtg_encoder";
    const esp_err_t err = esp_timer_create(&timerConfig, &gEncoderTimer);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Encoder poll timer: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_timer_start_periodic(gEncoderTimer, 2000));
    ESP_LOGI(kTag, "Encoder sends one step on debounced release (2 ms sampling)");
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
    // Keep native touch coordinates. The previous 180-degree inversion
    // mapped the bottom-right Add Counter control into the top deck picker.
    x = rawX;
    y = rawY;
    return x >= 0 && x < 360 && y >= 0 && y < 360;
}

void changeCounter(int direction) {
    int *number = nullptr;
    if (gView.selected == MtgCounter::CommanderDamage &&
        gOpponentSelected >= 0 && gOpponentSelected < gOpponentCount)
        number = &gOpponentDamage[gOpponentSelected];
    else if (gView.selected == MtgCounter::Life) number = &gView.life;
    else if (gView.selected == MtgCounter::Casts) number = &gView.casts;
    else if (gView.selected == MtgCounter::Dynamic &&
             gActiveTracker >= 0 && gActiveTracker < int(gTrackers.size()))
        number = &gTrackers[size_t(gActiveTracker)].value;
    if (!number) return;
    const int updated = std::clamp(*number + direction,
                                   gView.selected == MtgCounter::Life ? -999 : 0, 9999);
    if (*number == updated) return;
    *number = updated;
    if (gView.selected == MtgCounter::Dynamic)
        setLegacyValue(gTrackers[size_t(gActiveTracker)].id, updated);
    gDirty = true;
    gLastChange = esp_timer_get_time();
    publish();
}

void knobStep(int direction) {
    // Touch navigates; completed rotary pulses change only selected values.
    if (gDecks.empty() || gView.screen != MtgScreen::Dashboard) return;
    changeCounter(direction);
}

void swipe(int direction, int originY, bool vertical) {
    if (gView.screen == MtgScreen::Dashboard) {
        if (originY >= 195 && originY <= 253) {
            const int maxPage = std::max(0, (gOpponentCount - 1) / 3);
            const int target = std::clamp(gOpponentPage + direction, 0, maxPage);
            if (target != gOpponentPage) {
                gOpponentPage = target;
                publish();
            }
        } else {
            const int target = std::clamp(gView.page + direction, 0, gView.pageCount - 1);
            if (target != gView.page) { gView.page = target; publish(); }
        }
    } else if (gView.screen == MtgScreen::EditCounters) {
        // This screen is not paginated: its scroll position is controlled by
        // the live touch position in the game task.
        (void)vertical;
        return;
    } else if (gView.screen == MtgScreen::ChooseDeck && !gDecks.empty()) {
        int index = (gView.deckIndex + direction) % int(gDecks.size());
        if (index < 0) index += int(gDecks.size());
        gView.deckIndex = index;
        const DeckMeta &d = gDecks[size_t(index)];
        std::snprintf(gView.deckName, sizeof(gView.deckName), "%s", d.name.c_str());
        std::snprintf(gView.commander, sizeof(gView.commander), "%s", d.commander.c_str());
        publish();
    }
}

void changeOpponentCount(int delta) {
    if (delta > 0 && gOpponentCount < kMaxOpponents) {
        gOpponentDamage[gOpponentCount] = 0;
        ++gOpponentCount;
        gOpponentPage = (gOpponentCount - 1) / 3;
    } else if (delta < 0 && gOpponentCount > 1) {
        --gOpponentCount;
        gOpponentDamage[gOpponentCount] = 0;
        gOpponentPage = std::min(gOpponentPage, (gOpponentCount - 1) / 3);
        if (gOpponentSelected >= gOpponentCount) {
            gOpponentSelected = -1;
            gView.selected = MtgCounter::Life;
        }
    } else {
        return;
    }
    gDirty = true;
    gLastChange = esp_timer_get_time();
    publish();
}

void tap(int x, int y) {
    if (gDecks.empty()) return;
    if (gView.screen == MtgScreen::ConfirmReset) {
        if (x >= 73 && x <= 175 && y >= 196 && y <= 247) {
            gView.life = 40;
            gView.poison = gView.rad = gView.damage = gView.casts = gView.deaths = 0;
            // New games start with exactly three opponents and no damage dealt.
            gOpponentCount = 3;
            gOpponentPage = 0;
            gOpponentSelected = -1;
            for (int &damage : gOpponentDamage) damage = 0;
            // Keep Windows defaults, discard only game-added counters.
            rebuildDefaults();
            gActiveTracker = -1;
            gView.page = 0;
            gView.selected = MtgCounter::Life;
            gDirty = true;
            gLastChange = esp_timer_get_time();
            ESP_LOGI(kTag, "New game confirmed for %s", gView.deckId);
        }
        gView.screen = MtgScreen::Dashboard;
        publish();
        return;
    }
    if (gView.screen == MtgScreen::ChooseDeck) {
        selectDeck(gView.deckIndex);
        return;
    }
    if (gView.screen == MtgScreen::EditCounters) {
        // Checkboxes change visibility, not the numeric value of a counter.
        // DONE exits; swiping switches between four-option pages.
        if (y < 90 || y >= 317) {
            gView.screen = MtgScreen::Dashboard;
            publish();
            return;
        }
        if (y >= 110 && y < 294 && x >= 55 && x <= 305) {
            const int index = mtg_scroll::tappedIndex(
                y,gView.editScrollPx,int(gAvailable.size()));
            const int row = index - gView.editFirstRow;
            if (index >= 0 && index < int(gAvailable.size())) {
                const CounterDef item = gAvailable[size_t(index)];
                int active = -1;
                for (size_t n = 0; n < gTrackers.size(); ++n)
                    if (gTrackers[n].id == item.id) {
                        active = int(n);
                        break;
                    }
                if (active >= 0) {
                    // Save the value so unchecking never silently zeroes it.
                    gHiddenTrackers.push_back(gTrackers[size_t(active)]);
                    gTrackers.erase(gTrackers.begin() + active);
                    if (gActiveTracker == active) {
                        gActiveTracker = -1;
                        gView.selected = MtgCounter::Life;
                    } else if (gActiveTracker > active) {
                        --gActiveTracker;
                    }
                } else if (gTrackers.size() + gHiddenTrackers.size() < kMaxActiveCounters ||
                           std::any_of(gHiddenTrackers.begin(),gHiddenTrackers.end(),
                               [&](const ActiveCounter &entry){return entry.id == item.id;})) {
                    ActiveCounter entry{item.id, item.label, legacyValue(item.id), true};
                    for (auto it = gHiddenTrackers.begin(); it != gHiddenTrackers.end(); ++it) {
                        if (it->id == item.id) {
                            entry = *it;
                            gHiddenTrackers.erase(it);
                            break;
                        }
                    }
                    gTrackers.push_back(std::move(entry));
                } else {
                    return; // Selection capacity reached.
                }
                gView.page = std::clamp(gView.page, 0,
                                        std::max(0, int((gTrackers.size() + 1) / 2) - 1));
                gDirty = true;
                gLastChange = esp_timer_get_time();
                gAvailableDirty = true;
                publish();
            }
        }
        return;
    }

    if (y < 101) {
        gView.screen = MtgScreen::ChooseDeck;
    } else if (y >= 207 && y <= 249 && x >= 25 && x <= 70) {
        changeOpponentCount(-1);
        return;
    } else if (y >= 207 && y <= 249 && x >= 290 && x <= 335) {
        changeOpponentCount(+1);
        return;
    } else if (y >= 209 && y <= 245 && x >= 73 && x <= 287) {
        int segment = x < 144 ? 0 : x < 217 ? 1 : 2;
        const int opponent = gOpponentPage * 3 + segment;
        if (opponent < gOpponentCount) {
            gOpponentSelected = opponent;
            gActiveTracker = -1;
            gView.selected = MtgCounter::CommanderDamage;
        }
    } else if (y >= 107 && y <= 160) {
        gView.selected = MtgCounter::Life;
        gActiveTracker = -1;
        gOpponentSelected = -1;
    } else if (y >= 160 && y < 191) {
        gView.selected = MtgCounter::Casts;
        gActiveTracker = -1;
        gOpponentSelected = -1;
    } else if (y >= 256 && y <= 289 && x >= 279 && x <= 321) {
        // The only way to edit the set of optional counters.
        gView.screen = MtgScreen::EditCounters;
        gView.editScrollPx = 0;
        gAvailableDirty = true;
    } else if (y >= 259 && y <= 288) {
        int tile = -1;
        if (x >= 48 && x <= 156) tile = 0;
        else if (x >= 165 && x <= 273) tile = 1;
        if (tile >= 0) {
            const int index = gView.page * 2 + tile;
            if (index < int(gTrackers.size())) {
                gActiveTracker = index;
                gOpponentSelected = -1;
                gView.selected = MtgCounter::Dynamic;
            }
        }
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
    bool holdTriggered = false;
    int64_t touchStarted = 0;
    int touchX = 0, touchY = 0;
    int startX = 0, startY = 0;
    bool draggingCounterPages = false;
    int64_t lastDragDrawUs = 0;
    bool draggingEdit = false;
    int editStartScroll = 0;
    int editPrevY = 0;
    int64_t editPrevUs = 0;
    int64_t editLastDrawUs = 0;
    float editFingerVelocity = 0.0f; // Positive is scroll down the list
    float editMomentumVelocity = 0.0f;
    float editMomentumFraction = 0.0f;
    int64_t editMomentumLastUs = 0;
    while (true) {
        if (gReloadPending.exchange(false)) {
            loadDecksFromFlash();
        }
        int direction = 0;
        while (gRotaryEvents && xQueueReceive(gRotaryEvents, &direction, 0) == pdTRUE)
            knobStep(direction);

        int x = 0, y = 0;
        const bool currentlyDown = touchPoint(x, y);
        if (currentlyDown && !down) {
            touchStarted = esp_timer_get_time();
            startX = x; startY = y;
            holdTriggered = false;
            draggingCounterPages = false;
            draggingEdit = false;
            editMomentumVelocity = 0.0f;
            editMomentumFraction = 0.0f;
            editStartScroll = gView.editScrollPx;
            editPrevY = y;
            editPrevUs = touchStarted;
            editFingerVelocity = 0.0f;
        }
        if (currentlyDown) {
            touchX = x; touchY = y;
            // Edit Counters behaves like a web scroll container. The list
            // follows the finger pixel-for-pixel, rather than selecting a
            // different four-row page when the touch ends.
            if (gView.screen == MtgScreen::EditCounters &&
                startY >= mtg_scroll::kViewportTop &&
                startY < mtg_scroll::kViewportBottom &&
                startX >= 55 && startX <= 305) {
                const int dx = x - startX;
                const int dy = y - startY;
                if (!draggingEdit && std::abs(dy) >= 7 &&
                    std::abs(dy) > std::abs(dx)) {
                    draggingEdit = true;
                }
                if (draggingEdit) {
                    const int64_t now = esp_timer_get_time();
                    const int deltaY = y - editPrevY;
                    const int64_t dt = now - editPrevUs;
                    if (dt >= 8000) {
                        // Bound flick speed and smooth touch-sampling jitter.
                        const float speed = std::clamp(
                            -float(deltaY) * 1000000.0f / float(dt),
                            -1400.0f, 1400.0f);
                        editFingerVelocity = editFingerVelocity * 0.55f + speed * 0.45f;
                        editPrevY = y;
                        editPrevUs = now;
                    }
                    const int offset = mtg_scroll::clampScroll(
                        editStartScroll - dy, int(gAvailable.size()));
                    if (offset != gView.editScrollPx &&
                        now - editLastDrawUs >= 24000) {
                        gView.editScrollPx = offset;
                        editLastDrawUs = now;
                        publish();
                    }
                }
            }
            // Horizontal swipes move the bottom counter carousel from most
            // of the dashboard, just like the prior gesture handler. Player
            // damage has its own swipe region; EDIT remains a tap target.
            const bool damageSwipe = startY >= 195 && startY <= 253;
            const bool editTarget = startY >= 256 && startY <= 289 &&
                                    startX >= 279 && startX <= 321;
            if (gView.screen == MtgScreen::Dashboard && !damageSwipe &&
                !editTarget && startY >= 42 && startY <= 335 &&
                startX >= 25 && startX <= 335 && gView.pageCount > 1) {
                const int dx = x - startX, dy = y - startY;
                if (!draggingCounterPages && std::abs(dx) >= 10 &&
                    std::abs(dx) > std::abs(dy)) {
                    draggingCounterPages = true;
                }
                if (draggingCounterPages) {
                    int offset = std::clamp(dx, -226, 226);
                    if ((offset < 0 && gView.page >= gView.pageCount - 1) ||
                        (offset > 0 && gView.page == 0))
                        offset = 0;
                    const int64_t now = esp_timer_get_time();
                    if ((std::abs(offset - gView.dragOffsetPx) >= 2 &&
                         now - lastDragDrawUs >= 24000) ||
                        (!gView.dragActive && offset != 0)) {
                        gView.dragActive = offset != 0;
                        gView.dragOffsetPx = offset;
                        lastDragDrawUs = now;
                        publish();
                    }
                }
            }
            // Long hold over LIFE starts a *confirmation*, never an instant reset.
            if (!holdTriggered && !draggingCounterPages &&
                gView.screen == MtgScreen::Dashboard &&
                startX >= 115 && startX <= 245 &&
                startY >= 117 && startY <= 181 &&
                std::abs(x - startX) < 25 && std::abs(y - startY) < 25 &&
                esp_timer_get_time() - touchStarted >= 1800000) {
                gView.screen = MtgScreen::ConfirmReset;
                holdTriggered = true;
                publish();
            }
        }
        if (!currentlyDown && down && !holdTriggered) {
            const int dx = touchX - startX;
            const int dy = touchY - startY;
            if (draggingCounterPages) {
                // A short drag springs back; a committed drag snaps to the
                // neighbouring page from the finger's actual release offset.
                const int offset = std::clamp(dx, -226, 226);
                const int direction = offset < 0 ? 1 : -1;
                const int candidate = gView.page + direction;
                if (std::abs(offset) >= 68 && candidate >= 0 &&
                    candidate < gView.pageCount)
                    gView.page = candidate;
                gView.dragActive = false;
                gView.dragOffsetPx = 0;
                draggingCounterPages = false;
                publish();
            } else if (gView.screen == MtgScreen::EditCounters) {
                if (draggingEdit) {
                    // Keep the final finger position, then gently decelerate.
                    gView.editScrollPx = mtg_scroll::clampScroll(
                        editStartScroll - dy, int(gAvailable.size()));
                    publish();
                    const int64_t now = esp_timer_get_time();
                    editMomentumVelocity = now - editPrevUs > 95000 ?
                        0.0f : std::clamp(editFingerVelocity, -1000.0f, 1000.0f);
                    if (std::abs(editMomentumVelocity) < 90.0f)
                        editMomentumVelocity = 0.0f;
                    editMomentumLastUs = now;
                    draggingEdit = false;
                } else if (std::abs(dx) < 18 && std::abs(dy) < 18) {
                    tap(touchX,touchY);
                }
            } else if (std::abs(dx) > 42 &&
                       std::abs(dx) > std::abs(dy) * 13 / 10) {
                swipe(dx < 0 ? +1 : -1, startY, false);
            } else if (std::abs(dx) < 28 && std::abs(dy) < 28) {
                tap(touchX, touchY);
            }
        }
        down = currentlyDown;

        // Inertial vertical scrolling. No row-snapping, no pagination, and
        // no blocking animation that would delay the rotary input handler.
        if (!currentlyDown && editMomentumVelocity != 0.0f &&
            gView.screen == MtgScreen::EditCounters) {
            const int64_t now = esp_timer_get_time();
            const float dt = std::clamp(float(now - editMomentumLastUs) *
                                        0.000001f, 0.0f, 0.05f);
            editMomentumLastUs = now;
            editMomentumFraction += editMomentumVelocity * dt;
            const int whole = int(editMomentumFraction);
            editMomentumFraction -= float(whole);
            if (whole != 0) {
                const int next = mtg_scroll::clampScroll(
                    gView.editScrollPx + whole, int(gAvailable.size()));
                if (next != gView.editScrollPx &&
                    now - editLastDrawUs >= 27000) {
                    gView.editScrollPx = next;
                    editLastDrawUs = now;
                    publish();
                }
                if (next == 0 || next == mtg_scroll::maxScroll(int(gAvailable.size())))
                    editMomentumVelocity = 0.0f;
            }
            // Exponential friction, independent of the game task tick rate.
            editMomentumVelocity *= std::exp(-6.0f * dt);
            if (std::abs(editMomentumVelocity) < 30.0f)
                editMomentumVelocity = 0.0f;
        }
        if (gDirty && esp_timer_get_time() - gLastChange >= 1500000)
            writeState();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
} // namespace

void mtg_game_start() {
    if (xTaskCreate(task, "mtg_game", 9216, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS)
        ESP_LOGE(kTag, "Could not start game input task");
}

void mtg_game_reload_decks() { gReloadPending.store(true); }

void mtg_game_view(MtgGameView &output) {
    taskENTER_CRITICAL(&gViewLock);
    output = gView;
    taskEXIT_CRITICAL(&gViewLock);
}
