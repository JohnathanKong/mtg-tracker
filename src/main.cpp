#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>
#include <cstdio>

// Milestone 1 intentionally does not touch the display, encoder, or TF card.
// USB serial receives a complete deck library and stores it in ESP32-S3 flash.
// See docs/sync-protocol-v1.md for the exact desktop/device contract.

namespace {
constexpr size_t kMaxPayload = 65536;
constexpr size_t kMaxChunk = 256;
constexpr size_t kMaxLine = 95;
constexpr unsigned long kIdleTimeoutMs = 15000;
constexpr char kCurrent[] = "/decks.json";
constexpr char kStaging[] = "/decks.tmp";
constexpr char kBackup[] = "/decks.bak";

bool flashReady = false;
bool receiving = false;
File staging;
size_t expected = 0;
size_t received = 0;
size_t chunkRemaining = 0;
uint32_t expectedCrc = 0;
uint32_t runningCrc = 0xFFFFFFFFu;
unsigned long lastReceivedAt = 0;
char line[kMaxLine + 1]{};
size_t lineLength = 0;

uint32_t updateCrc(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return crc;
}

void error(const char *reason) {
  Serial.print("MTG1 ERROR ");
  Serial.println(reason);
}

void abortTransfer() {
  if (staging) staging.close();
  if (flashReady) LittleFS.remove(kStaging);
  receiving = false;
  expected = received = chunkRemaining = 0;
  runningCrc = 0xFFFFFFFFu;
}

bool validText(JsonVariantConst value, bool allowEmpty = false) {
  if (!value.is<const char *>()) return false;
  const char *s = value.as<const char *>();
  if (!s) return false;
  const size_t length = strlen(s);
  return length <= 128 && (allowEmpty || length > 0);
}

bool validatePayload(size_t &count) {
  File file = LittleFS.open(kStaging, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, file);
  file.close();
  if (parseError || !doc["version"].is<int>() || doc["version"].as<int>() != 1 ||
      !doc["decks"].is<JsonArray>()) return false;

  JsonArrayConst decks = doc["decks"].as<JsonArrayConst>();
  if (decks.size() > 100) return false;

  for (JsonObjectConst d : decks) {
    if (!validText(d["id"]) || !validText(d["name"]) ||
        !validText(d["commander"], true) ||
        !d["colors"].is<JsonArray>() || !d["trackers"].is<JsonArray>() ||
        !d["customTrackers"].is<JsonArray>()) return false;

    if (d["colors"].as<JsonArrayConst>().size() > 5 ||
        d["trackers"].as<JsonArrayConst>().size() > 150 ||
        d["customTrackers"].as<JsonArrayConst>().size() > 100) return false;

    for (JsonVariantConst color : d["colors"].as<JsonArrayConst>()) {
      if (!color.is<const char *>()) return false;
      const char *c = color.as<const char *>();
      if (!c || strlen(c) != 1 || !strchr("WUBRG", c[0])) return false;
    }
    for (JsonVariantConst tracker : d["trackers"].as<JsonArrayConst>()) {
      if (!validText(tracker)) return false;
    }
    for (JsonObjectConst custom : d["customTrackers"].as<JsonArrayConst>()) {
      if (!validText(custom["id"]) || !validText(custom["label"])) return false;
    }
  }
  count = decks.size();
  return true;
}

int storedDeckCount() {
  if (!flashReady || !LittleFS.exists(kCurrent)) return 0;
  File file = LittleFS.open(kCurrent, "r");
  if (!file) return 0;
  JsonDocument doc;
  DeserializationError result = deserializeJson(doc, file);
  file.close();
  if (result || !doc["decks"].is<JsonArray>()) return 0;
  return int(doc["decks"].as<JsonArrayConst>().size());
}

void commitTransfer() {
  if (!receiving || received != expected || chunkRemaining != 0) {
    error("INCOMPLETE");
    return;
  }
  staging.close();
  const uint32_t resultCrc = runningCrc ^ 0xFFFFFFFFu;
  if (resultCrc != expectedCrc) {
    error("CRC");
    abortTransfer();
    return;
  }

  size_t count = 0;
  if (!validatePayload(count)) {
    error("SCHEMA");
    abortTransfer();
    return;
  }

  // Stage is already fully received and validated. Keep a backup of the last
  // good profile library in case power is lost during file promotion.
  LittleFS.remove(kBackup);
  const bool previous = LittleFS.exists(kCurrent);
  if (previous && !LittleFS.rename(kCurrent, kBackup)) {
    error("BACKUP");
    abortTransfer();
    return;
  }
  if (!LittleFS.rename(kStaging, kCurrent)) {
    if (previous) LittleFS.rename(kBackup, kCurrent);
    error("SAVE");
    abortTransfer();
    return;
  }

  receiving = false;
  expected = received = chunkRemaining = 0;
  Serial.printf("MTG1 STORED %u %08lX\n",
                unsigned(count), static_cast<unsigned long>(resultCrc));
}

void handleLine(const char *command) {
  if (strcmp(command, "MTG1 HELLO") == 0) {
    if (receiving) abortTransfer();
    Serial.println("MTG1 READY 1");
    return;
  }
  if (strcmp(command, "MTG1 STATUS") == 0) {
    if (receiving) { error("BUSY"); return; }
    Serial.printf("MTG1 STATUS %d\n", storedDeckCount());
    return;
  }
  if (strncmp(command, "MTG1 BEGIN ", 11) == 0) {
    if (!flashReady) { error("STORAGE"); return; }
    if (receiving) { error("BUSY"); return; }

    unsigned long size = 0;
    char hex[9]{};
    char extra = '\0';
    // Exactly two arguments, and exactly eight hex digits for the checksum.
    if (sscanf(command, "MTG1 BEGIN %lu %8[0-9A-F] %c", &size, hex, &extra) != 2 ||
        strlen(hex) != 8 || size == 0 || size > kMaxPayload) {
      error("BEGIN");
      return;
    }
    expected = size;
    received = chunkRemaining = 0;
    expectedCrc = static_cast<uint32_t>(strtoul(hex, nullptr, 16));
    runningCrc = 0xFFFFFFFFu;
    LittleFS.remove(kStaging);
    staging = LittleFS.open(kStaging, "w");
    if (!staging) { error("STORAGE"); abortTransfer(); return; }
    receiving = true;
    lastReceivedAt = millis();
    Serial.println("MTG1 OK");
    return;
  }
  if (strncmp(command, "MTG1 CHUNK ", 11) == 0) {
    unsigned long chunkSize = 0;
    char extra = '\0';
    if (!receiving || chunkRemaining != 0 ||
        sscanf(command, "MTG1 CHUNK %lu %c", &chunkSize, &extra) != 1 ||
        chunkSize == 0 || chunkSize > kMaxChunk ||
        received + chunkSize > expected) {
      error("CHUNK");
      abortTransfer();
      return;
    }
    chunkRemaining = chunkSize;
    lastReceivedAt = millis();
    return;
  }
  if (strcmp(command, "MTG1 COMMIT") == 0) {
    commitTransfer();
    return;
  }
  error("COMMAND");
}

void processSerial() {
  while (Serial.available()) {
    if (chunkRemaining > 0) {
      uint8_t bytes[kMaxChunk];
      const size_t n = min(size_t(Serial.available()),
                           min(chunkRemaining, kMaxChunk));
      if (!n) return;
      const size_t got = Serial.readBytes(bytes, n);
      if (!got) return;
      if (!staging || staging.write(bytes, got) != got) {
        error("WRITE");
        abortTransfer();
        return;
      }
      runningCrc = updateCrc(runningCrc, bytes, got);
      received += got;
      chunkRemaining -= got;
      lastReceivedAt = millis();
      if (chunkRemaining == 0) {
        Serial.printf("MTG1 NEXT %u\n", unsigned(received));
      }
      continue;
    }

    const int b = Serial.read();
    if (b < 0) return;
    if (b == '\r') continue;
    if (b == '\n') {
      line[lineLength] = '\0';
      if (lineLength > 0) handleLine(line);
      lineLength = 0;
      continue;
    }
    if (b < 32 || b > 126 || lineLength >= kMaxLine) {
      lineLength = 0;
      if (receiving) abortTransfer();
      error("LINE");
      continue;
    }
    line[lineLength++] = char(b);
  }
}
} // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(30);
  flashReady = LittleFS.begin(true); // Formats only the dedicated LittleFS
                                      // partition if it is not initialized.
  if (flashReady && !LittleFS.exists(kCurrent) &&
      LittleFS.exists(kBackup)) {
    LittleFS.rename(kBackup, kCurrent);
  }
}

void loop() {
  processSerial();
  if (receiving && millis() - lastReceivedAt > kIdleTimeoutMs) {
    abortTransfer();
    error("TIMEOUT");
  }
  delay(1);
}
