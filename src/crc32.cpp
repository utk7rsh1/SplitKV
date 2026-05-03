#include "crc32.h"

#include <cstdint>

namespace splitkv {
// CRC32C lookup table (polynomial 0x82F63B78, a.k.a. Castagnoli).
//
// The table is built once on first use via a static local and is thereafter
// shared read-only across threads.

namespace {

struct CRC32Table {
    uint32_t entries[256];

    CRC32Table() {
        const uint32_t kPolynomial = 0x82F63B78u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ kPolynomial;
                } else {
                    crc >>= 1;
                }
            }
            entries[i] = crc;
        }
    }
};

// Guaranteed thread-safe initialization in C++11 and later.
const CRC32Table& GetTable() {
    static const CRC32Table table;
    return table;
}

} // anonymous namespace

uint32_t CRC32(const char* data, size_t length, uint32_t crc) {
    return CRC32Extend(crc, data, length);
}

uint32_t CRC32Extend(uint32_t crc, const char* data, size_t length) {
    const auto& table = GetTable();
    const auto* p = reinterpret_cast<const unsigned char*>(data);

    crc = ~crc;  // Pre-invert per the standard CRC convention.
    for (size_t i = 0; i < length; ++i) {
        crc = table.entries[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;  // Post-invert.
}

} // namespace splitkv
