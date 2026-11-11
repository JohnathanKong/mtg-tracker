// MTG Tracker USB deck-profile receiver — native ESP-IDF 6.1.
// Boot, sync progress and errors are shown on the round LCD.
// Touch, encoder and audio are later milestones.
// Wire-compatible with mtg-tracker-desktop/src/DeviceSync.cpp (MTG1 protocol).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mtg_display.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr uart_port_t kPort = UART_NUM_0;
constexpr int kBaud = 115200;
constexpr size_t kMaxPayload = 65536;
constexpr size_t kMaxChunk = 256;
constexpr size_t kMaxLine = 95;
constexpr int64_t kIdleTimeoutUs = 15000000;
constexpr char kFolder[] = "/mtg";
constexpr char kLive[] = "/mtg/decks.json";
constexpr char kTemp[] = "/mtg/decks.tmp";
constexpr char kBackup[] = "/mtg/decks.bak";

bool storageReady = false;
bool receiving = false;
FILE *staging = nullptr;
size_t expectedBytes = 0;
size_t receivedBytes = 0;
size_t chunkRemaining = 0;
uint32_t expectedCrc = 0;
uint32_t rollingCrc = 0xFFFFFFFFu;
int64_t lastByteAt = 0;
char line[kMaxLine + 1] = {};
size_t lineLength = 0;
int lastProgressStep = -1;
int shownDeckCount = 0;

void reply(const char *message) {
    uart_write_bytes(kPort, message, std::strlen(message));
    uart_write_bytes(kPort, "\n", 1);
}

void replyError(const char *reason) {
    mtg_display_show(MthStatus::Error, shownDeckCount, 0, reason);
    char output[100];
    std::snprintf(output, sizeof(output), "MTG1 ERROR %s", reason);
    reply(output);
}

uint32_t accumulateCrc(uint32_t crc, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc;
}

bool exists(const char *path) {
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

void abortTransfer() {
    if (staging) {
        std::fclose(staging);
        staging = nullptr;
    }
    if (storageReady) std::remove(kTemp);
    receiving = false;
    expectedBytes = 0;
    receivedBytes = 0;
    chunkRemaining = 0;
    expectedCrc = 0;
    rollingCrc = 0xFFFFFFFFu;
}

bool validText(const cJSON *value, bool allowEmpty = false) {
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    size_t length = std::strlen(value->valuestring);
    return length <= 128 && (allowEmpty || length > 0);
}

bool validateDocument(const char *content, size_t size, int &deckCount) {
    // The payload is not guaranteed to be NUL-terminated.
    cJSON *root = cJSON_ParseWithLength(content, size);
    if (!root) return false;

    bool valid = cJSON_IsObject(root);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *decks = cJSON_GetObjectItemCaseSensitive(root, "decks");
    valid = valid && cJSON_IsNumber(version) && version->valuedouble == 1 &&
            cJSON_IsArray(decks) && cJSON_GetArraySize(decks) <= 100;

    if (valid) {
        const cJSON *deck = nullptr;
        cJSON_ArrayForEach(deck, decks) {
            if (!cJSON_IsObject(deck) ||
                !validText(cJSON_GetObjectItemCaseSensitive(deck, "id")) ||
                !validText(cJSON_GetObjectItemCaseSensitive(deck, "name")) ||
                !validText(cJSON_GetObjectItemCaseSensitive(deck, "commander"), true)) {
                valid = false; break;
            }
            const cJSON *colors = cJSON_GetObjectItemCaseSensitive(deck, "colors");
            const cJSON *trackers = cJSON_GetObjectItemCaseSensitive(deck, "trackers");
            const cJSON *custom = cJSON_GetObjectItemCaseSensitive(deck, "customTrackers");
            if (!cJSON_IsArray(colors) || !cJSON_IsArray(trackers) ||
                !cJSON_IsArray(custom) || cJSON_GetArraySize(colors) > 5 ||
                cJSON_GetArraySize(trackers) > 150 || cJSON_GetArraySize(custom) > 100) {
                valid = false; break;
            }

            const cJSON *entry = nullptr;
            cJSON_ArrayForEach(entry, colors) {
                if (!validText(entry) ||
                    std::strlen(entry->valuestring) != 1 ||
                    !std::strchr("WUBRG", entry->valuestring[0])) {
                    valid = false; break;
                }
            }
            if (!valid) break;

            cJSON_ArrayForEach(entry, trackers) {
                if (!validText(entry)) { valid = false; break; }
            }
            if (!valid) break;

            cJSON_ArrayForEach(entry, custom) {
                if (!cJSON_IsObject(entry) ||
                    !validText(cJSON_GetObjectItemCaseSensitive(entry, "id")) ||
                    !validText(cJSON_GetObjectItemCaseSensitive(entry, "label"))) {
                    valid = false; break;
                }
            }
            if (!valid) break;
        }
    }

    if (valid) deckCount = cJSON_GetArraySize(decks);
    cJSON_Delete(root);
    return valid;
}

bool loadDocument(const char *path, std::string &data) {
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file); return false;
    }
    const long length = std::ftell(file);
    if (length <= 0 || length > long(kMaxPayload) ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file); return false;
    }
    data.resize(size_t(length));
    const size_t n = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    return n == data.size();
}

bool validSavedDocument(const char *path, int &count) {
    std::string data;
    return loadDocument(path, data) &&
           validateDocument(data.data(), data.size(), count);
}

int storedDeckCount() {
    if (!storageReady) return 0;
    int count = 0;
    return validSavedDocument(kLive, count) ? count : 0;
}

void commitTransfer() {
    if (!receiving || chunkRemaining != 0 || receivedBytes != expectedBytes) {
        replyError("INCOMPLETE");
        return;
    }
    if (std::fflush(staging) != 0) {
        replyError("WRITE");
        abortTransfer(); return;
    }
    std::fclose(staging);
    staging = nullptr;

    const uint32_t checksum = rollingCrc ^ 0xFFFFFFFFu;
    if (checksum != expectedCrc) {
        replyError("CRC");
        abortTransfer(); return;
    }

    int count = 0;
    if (!validSavedDocument(kTemp, count)) {
        replyError("SCHEMA");
        abortTransfer(); return;
    }

    // Only a complete, checksum-verified document can replace the old one.
    // A backup is retained to allow recovery if power fails during promotion.
    const bool previous = exists(kLive);
    std::remove(kBackup);
    if (previous && std::rename(kLive, kBackup) != 0) {
        replyError("BACKUP");
        abortTransfer(); return;
    }
    if (std::rename(kTemp, kLive) != 0) {
        if (previous) std::rename(kBackup, kLive);
        replyError("SAVE");
        abortTransfer(); return;
    }

    receiving = false;
    expectedBytes = receivedBytes = chunkRemaining = 0;
    shownDeckCount = count;
    lastProgressStep = -1;
    char output[100];
    std::snprintf(output, sizeof(output), "MTG1 STORED %d %08lX",
                  count, static_cast<unsigned long>(checksum));
    reply(output);
    mtg_display_show(MthStatus::Complete, shownDeckCount);
}

void processLine(const char *command) {
    if (std::strcmp(command, "MTG1 HELLO") == 0) {
        if (receiving) abortTransfer();
        reply("MTG1 READY 1");
        mtg_display_show(MthStatus::Ready, shownDeckCount);
        return;
    }
    if (std::strcmp(command, "MTG1 STATUS") == 0) {
        if (receiving) { replyError("BUSY"); return; }
        char output[100];
        std::snprintf(output, sizeof(output), "MTG1 STATUS %d", storedDeckCount());
        reply(output);
        return;
    }

    if (std::strncmp(command, "MTG1 BEGIN ", 11) == 0) {
        if (!storageReady) { replyError("STORAGE"); return; }
        if (receiving) { replyError("BUSY"); return; }
        unsigned long total = 0;
        char crcHex[9] = {};
        char extra = '\0';
        if (std::sscanf(command, "MTG1 BEGIN %lu %8[0-9A-F] %c",
                        &total, crcHex, &extra) != 2 ||
            std::strlen(crcHex) != 8 || total == 0 || total > kMaxPayload) {
            replyError("BEGIN"); return;
        }
        expectedBytes = size_t(total);
        receivedBytes = chunkRemaining = 0;
        expectedCrc = static_cast<uint32_t>(std::strtoul(crcHex, nullptr, 16));
        rollingCrc = 0xFFFFFFFFu;

        std::remove(kTemp);
        staging = std::fopen(kTemp, "wb");
        if (!staging) {
            replyError("STORAGE");
            abortTransfer();
            return;
        }
        receiving = true;
        lastByteAt = esp_timer_get_time();
        lastProgressStep = 0;
        reply("MTG1 OK");
        mtg_display_show(MthStatus::Receiving, shownDeckCount, 0);
        return;
    }

    if (std::strncmp(command, "MTG1 CHUNK ", 11) == 0) {
        unsigned long requested = 0;
        char extra = '\0';
        if (!receiving || chunkRemaining != 0 ||
            std::sscanf(command, "MTG1 CHUNK %lu %c", &requested, &extra) != 1 ||
            requested == 0 || requested > kMaxChunk ||
            receivedBytes + requested > expectedBytes) {
            replyError("CHUNK");
            abortTransfer(); return;
        }
        chunkRemaining = size_t(requested);
        lastByteAt = esp_timer_get_time();
        return;
    }

    if (std::strcmp(command, "MTG1 COMMIT") == 0) {
        commitTransfer();
        return;
    }
    replyError("COMMAND");
}

void receiveByte(uint8_t byte) {
    if (chunkRemaining > 0) {
        if (!staging || std::fwrite(&byte, 1, 1, staging) != 1) {
            replyError("WRITE");
            abortTransfer();
            return;
        }
        rollingCrc = accumulateCrc(rollingCrc, &byte, 1);
        ++receivedBytes;
        --chunkRemaining;
        lastByteAt = esp_timer_get_time();
        if (chunkRemaining == 0) {
            char output[100];
            std::snprintf(output, sizeof(output), "MTG1 NEXT %u",
                          static_cast<unsigned>(receivedBytes));
            reply(output);
            const int step = expectedBytes ? int((receivedBytes * 10) / expectedBytes) : 0;
            if (step > lastProgressStep) {
                lastProgressStep = step;
                mtg_display_show(MthStatus::Receiving, shownDeckCount, step * 10);
            }
        }
        return;
    }

    if (byte == '\r') return;
    if (byte == '\n') {
        line[lineLength] = '\0';
        if (lineLength) processLine(line);
        lineLength = 0;
        return;
    }
    if (byte < 32 || byte > 126 || lineLength >= kMaxLine) {
        lineLength = 0;
        if (receiving) abortTransfer();
        replyError("LINE");
        return;
    }
    line[lineLength++] = char(byte);
}

void setupStorage() {
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kFolder;
    conf.partition_label = "storage";
    conf.max_files = 4;
    // New units need an initial SPIFFS format. A previous Arduino/LittleFS
    // image, if ever flashed, is NOT storage-compatible with this firmware.
    conf.format_if_mount_failed = true;

    const esp_err_t result = esp_vfs_spiffs_register(&conf);
    storageReady = result == ESP_OK;
    if (!storageReady) {
        ESP_LOGE("mtg", "SPIFFS mount failed: %s", esp_err_to_name(result));
        return;
    }

    int count = 0;
    if (!validSavedDocument(kLive, count) && validSavedDocument(kBackup, count)) {
        // Restore the last verified profile if the newest file was lost.
        std::remove(kLive);
        std::rename(kBackup, kLive);
    }
    std::remove(kTemp); // An incomplete transfer from a previous boot.
}

void setupSerial() {
    uart_config_t config = {};
    config.baud_rate = kBaud;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(kPort, &config));
    ESP_ERROR_CHECK(uart_set_pin(kPort, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(kPort, 2048, 2048, 0, nullptr, 0));
}
} // namespace

extern "C" void app_main(void) {
    const bool screenOk = mtg_display_init();
    setupStorage();
    setupSerial();
    shownDeckCount = storedDeckCount();
    mtg_display_show(storageReady ? MthStatus::Ready : MthStatus::StorageError,
                     shownDeckCount);
    ESP_LOGI("mtg", "MTG1 sync receiver ready (ESP-IDF), storage=%d, lcd=%d, decks=%d",
             int(storageReady), int(screenOk), shownDeckCount);

    while (true) {
        uint8_t byte = 0;
        const int count = uart_read_bytes(kPort, &byte, 1, pdMS_TO_TICKS(20));
        if (count == 1) receiveByte(byte);
        if (receiving &&
            esp_timer_get_time() - lastByteAt > kIdleTimeoutUs) {
            abortTransfer();
            replyError("TIMEOUT");
        }
    }
}
