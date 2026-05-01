#include "coding.h"

#include <cassert>
#include <cstring>

namespace splitkv {

// ===========================================================================
// Fixed-width encoding — little-endian
// ===========================================================================

void EncodeFixed32(char* dst, uint32_t value) {
    // Write bytes in little-endian order regardless of host endianness.
    auto* p = reinterpret_cast<unsigned char*>(dst);
    p[0] = static_cast<unsigned char>(value);
    p[1] = static_cast<unsigned char>(value >> 8);
    p[2] = static_cast<unsigned char>(value >> 16);
    p[3] = static_cast<unsigned char>(value >> 24);
}

void EncodeFixed64(char* dst, uint64_t value) {
    auto* p = reinterpret_cast<unsigned char*>(dst);
    p[0] = static_cast<unsigned char>(value);
    p[1] = static_cast<unsigned char>(value >> 8);
    p[2] = static_cast<unsigned char>(value >> 16);
    p[3] = static_cast<unsigned char>(value >> 24);
    p[4] = static_cast<unsigned char>(value >> 32);
    p[5] = static_cast<unsigned char>(value >> 40);
    p[6] = static_cast<unsigned char>(value >> 48);
    p[7] = static_cast<unsigned char>(value >> 56);
}

uint32_t DecodeFixed32(const char* ptr) {
    auto* p = reinterpret_cast<const unsigned char*>(ptr);
    return (static_cast<uint32_t>(p[0]))
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t DecodeFixed64(const char* ptr) {
    auto* p = reinterpret_cast<const unsigned char*>(ptr);
    return (static_cast<uint64_t>(p[0]))
         | (static_cast<uint64_t>(p[1]) << 8)
         | (static_cast<uint64_t>(p[2]) << 16)
         | (static_cast<uint64_t>(p[3]) << 24)
         | (static_cast<uint64_t>(p[4]) << 32)
         | (static_cast<uint64_t>(p[5]) << 40)
         | (static_cast<uint64_t>(p[6]) << 48)
         | (static_cast<uint64_t>(p[7]) << 56);
}

// ===========================================================================
// Varint encoding
// ===========================================================================

char* EncodeVarint32(char* dst, uint32_t v) {
    auto* ptr = reinterpret_cast<unsigned char*>(dst);
    // The constant 0x80 (128) is the continuation bit.
    while (v >= 0x80) {
        *ptr++ = static_cast<unsigned char>(v | 0x80);
        v >>= 7;
    }
    *ptr++ = static_cast<unsigned char>(v);
    return reinterpret_cast<char*>(ptr);
}

char* EncodeVarint64(char* dst, uint64_t v) {
    auto* ptr = reinterpret_cast<unsigned char*>(dst);
    while (v >= 0x80) {
        *ptr++ = static_cast<unsigned char>(v | 0x80);
        v >>= 7;
    }
    *ptr++ = static_cast<unsigned char>(v);
    return reinterpret_cast<char*>(ptr);
}

const char* DecodeVarint32(const char* p, const char* limit, uint32_t* value) {
    if (p == limit) return nullptr;

    // Fast path: single-byte varint (very common for small values).
    uint32_t result = static_cast<unsigned char>(*p);
    if ((result & 0x80) == 0) {
        *value = result;
        return p + 1;
    }

    // General path — up to 5 bytes for a 32-bit varint.
    result = 0;
    uint32_t shift = 0;
    while (p < limit && shift <= 28) {
        uint32_t byte = static_cast<unsigned char>(*p);
        p++;
        result |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return p;
        }
        shift += 7;
    }
    return nullptr;  // Truncated or overlong varint.
}

const char* DecodeVarint64(const char* p, const char* limit, uint64_t* value) {
    if (p == limit) return nullptr;

    // Fast path for single-byte values.
    uint64_t result = static_cast<unsigned char>(*p);
    if ((result & 0x80) == 0) {
        *value = result;
        return p + 1;
    }

    // General path — up to 10 bytes for a 64-bit varint.
    result = 0;
    uint32_t shift = 0;
    while (p < limit && shift <= 63) {
        uint64_t byte = static_cast<unsigned char>(*p);
        p++;
        result |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return p;
        }
        shift += 7;
    }
    return nullptr;  // Truncated or overlong varint.
}

// ===========================================================================
// String-appending helpers
// ===========================================================================

void PutFixed32(std::string* dst, uint32_t value) {
    char buf[4];
    EncodeFixed32(buf, value);
    dst->append(buf, 4);
}

void PutFixed64(std::string* dst, uint64_t value) {
    char buf[8];
    EncodeFixed64(buf, value);
    dst->append(buf, 8);
}

void PutVarint32(std::string* dst, uint32_t v) {
    char buf[5];  // Max 5 bytes for a 32-bit varint.
    char* end = EncodeVarint32(buf, v);
    dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
    char buf[10]; // Max 10 bytes for a 64-bit varint.
    char* end = EncodeVarint64(buf, v);
    dst->append(buf, static_cast<size_t>(end - buf));
}

// ===========================================================================
// Length-prefixed slices
// ===========================================================================

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    if (!GetVarint32(input, &len)) {
        return false;
    }
    if (input->size() < len) {
        return false;
    }
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
}

// ===========================================================================
// Convenience consumers
// ===========================================================================

bool GetVarint32(Slice* input, uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = DecodeVarint32(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = DecodeVarint64(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<size_t>(limit - q));
    return true;
}

} // namespace splitkv
