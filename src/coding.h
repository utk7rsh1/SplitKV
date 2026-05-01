#pragma once

#include <cstdint>
#include <string>

#include "splitkv/slice.h"

namespace splitkv {
// Fixed-width encoding — little-endian, always exactly 4 or 8 bytes.

// Encode a 32-bit integer into |dst| in little-endian byte order.
void EncodeFixed32(char* dst, uint32_t value);

// Encode a 64-bit integer into |dst| in little-endian byte order.
void EncodeFixed64(char* dst, uint64_t value);

// Decode a little-endian 32-bit integer from |ptr|.
uint32_t DecodeFixed32(const char* ptr);

// Decode a little-endian 64-bit integer from |ptr|.
uint64_t DecodeFixed64(const char* ptr);
// Varint encoding — variable-length, 1–5 bytes for 32-bit, 1–10 for 64-bit.
// Uses the standard MSB-continuation format: each byte stores 7 payload bits
// and the high bit indicates whether more bytes follow.

// Encode |v| as a varint starting at |dst|.  Returns a pointer just past the
// last byte written.
char* EncodeVarint32(char* dst, uint32_t v);
char* EncodeVarint64(char* dst, uint64_t v);

// Decode a varint from the range [p, limit).  On success, stores the value in
// |*value| and returns a pointer just past the last consumed byte.  On
// failure (truncated input), returns nullptr.
const char* DecodeVarint32(const char* p, const char* limit, uint32_t* value);
const char* DecodeVarint64(const char* p, const char* limit, uint64_t* value);
// String-appending helpers — append encoded bytes to a std::string.

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t v);
void PutVarint64(std::string* dst, uint64_t v);
// Length-prefixed slices — a varint length followed by the raw bytes.

// Append a varint-length prefix + raw bytes of |value| to |dst|.
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// Consume a length-prefixed slice from the front of |*input|.  On success
// the consumed bytes are removed from |*input|, |*result| points into the
// original buffer, and the function returns true.
bool GetLengthPrefixedSlice(Slice* input, Slice* result);
// Convenience consumers — decode and advance a Slice in one step.

// Consume a varint32 from the front of |*input|.  Returns false on failure.
bool GetVarint32(Slice* input, uint32_t* value);

// Consume a varint64 from the front of |*input|.  Returns false on failure.
bool GetVarint64(Slice* input, uint64_t* value);

} // namespace splitkv
