// Shared, allocation-free parser for the on-wire MTG1 artwork header.
// Kept independent of ESP-IDF so it can be checked by a native host test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace mtg_protocol {

constexpr std::size_t kArtworkBytes = 360u * 360u * 2u;

enum class ArtworkHeaderResult { Ok, Invalid, WrongSize };

struct ArtworkHeader {
    char id[9] = {};
    std::size_t bytes = 0;
    std::uint32_t crc = 0;
};

inline int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

inline bool readHex8(const char *&cursor, std::uint32_t &value) {
    std::uint32_t parsed = 0;
    for (int i = 0; i < 8; ++i) {
        if (*cursor == '\0') return false;
        const int digit = hexDigit(*cursor++);
        if (digit < 0) return false;
        parsed = (parsed << 4) | std::uint32_t(digit);
    }
    value = parsed;
    return true;
}

// Header: MTG1 ART <8-hex deck ID> <decimal byte size> <8-hex CRC32>
// Consume every byte, with no sscanf scanset or platform-specific conversions.
inline ArtworkHeaderResult parseArtworkHeader(const char *command, ArtworkHeader &out) {
    if (!command || std::strncmp(command, "MTG1 ART ", 9) != 0)
        return ArtworkHeaderResult::Invalid;

    const char *cursor = command + 9;
    std::uint32_t id = 0;
    if (!readHex8(cursor, id) || *cursor++ != ' ')
        return ArtworkHeaderResult::Invalid;

    const char *sizeStart = cursor;
    std::size_t bytes = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        const unsigned digit = unsigned(*cursor++ - '0');
        if (bytes > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            return ArtworkHeaderResult::Invalid;
        bytes = bytes * 10 + digit;
    }
    if (cursor == sizeStart || *cursor++ != ' ')
        return ArtworkHeaderResult::Invalid;

    std::uint32_t crc = 0;
    if (!readHex8(cursor, crc) || *cursor != '\0')
        return ArtworkHeaderResult::Invalid;

    std::snprintf(out.id, sizeof(out.id), "%08lX", static_cast<unsigned long>(id));
    out.bytes = bytes;
    out.crc = crc;
    return bytes == kArtworkBytes ? ArtworkHeaderResult::Ok
                                  : ArtworkHeaderResult::WrongSize;
}

} // namespace mtg_protocol
