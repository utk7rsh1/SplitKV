#pragma once

#include <cstddef>
#include <cstdint>

namespace splitkv {

// Compute the CRC32C (Castagnoli) checksum of |length| bytes starting at
// |data|.  The optional |crc| parameter supplies an initial value so that
// checksums can be computed incrementally.
//
// Uses a software lookup table seeded with the CRC32C polynomial 0x82F63B78.
uint32_t CRC32(const char* data, size_t length, uint32_t crc = 0);

// Extend an existing CRC32C value with additional data.  Equivalent to
// CRC32(data, length, crc) — provided for readability at call sites that
// build a checksum across multiple disjoint buffers.
uint32_t CRC32Extend(uint32_t crc, const char* data, size_t length);

} // namespace splitkv
