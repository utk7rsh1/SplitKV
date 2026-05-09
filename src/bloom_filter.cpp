#include "bloom_filter.h"

#include <algorithm>
#include <cstring>

namespace splitkv {
//  Construction

BloomFilter::BloomFilter(size_t bits_per_key)
    : bits_per_key_(bits_per_key), num_hashes_(0) {
  // k = bits_per_key * ln(2) ≈ bits_per_key * 0.69
  size_t k = static_cast<size_t>(bits_per_key * 0.6931471805599453);
  if (k < 1) k = 1;
  if (k > 30) k = 30;
  num_hashes_ = k;
}
//  Hash — MurmurHash3-inspired 32-bit finalizer mix

uint32_t BloomFilter::Hash(const Slice& key, uint32_t seed) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(key.data());
  const size_t len = key.size();

  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;

  uint32_t h = seed;
  const size_t nblocks = len / 4;

  // Body — process 4-byte blocks.
  for (size_t i = 0; i < nblocks; ++i) {
    uint32_t k;
    std::memcpy(&k, data + i * 4, sizeof(k));

    k *= c1;
    k = (k << 15) | (k >> 17);
    k *= c2;

    h ^= k;
    h = (h << 13) | (h >> 19);
    h = h * 5 + 0xe6546b64;
  }

  // Tail — remaining bytes.
  const uint8_t* tail = data + nblocks * 4;
  uint32_t k1 = 0;
  switch (len & 3) {
    case 3:
      k1 ^= static_cast<uint32_t>(tail[2]) << 16;
      [[fallthrough]];
    case 2:
      k1 ^= static_cast<uint32_t>(tail[1]) << 8;
      [[fallthrough]];
    case 1:
      k1 ^= static_cast<uint32_t>(tail[0]);
      k1 *= c1;
      k1 = (k1 << 15) | (k1 >> 17);
      k1 *= c2;
      h ^= k1;
      break;
    default:
      break;
  }

  // Finalization mix.
  h ^= static_cast<uint32_t>(len);
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;

  return h;
}
//  CreateFilter

void BloomFilter::CreateFilter(const std::vector<Slice>& keys) {
  size_t num_keys = keys.size();
  if (num_keys == 0) {
    data_.clear();
    return;
  }

  // Total bits, with a minimum of 64 to avoid degenerate cases.
  size_t num_bits = num_keys * bits_per_key_;
  if (num_bits < 64) num_bits = 64;

  // Round up to whole bytes.
  size_t num_bytes = (num_bits + 7) / 8;
  num_bits = num_bytes * 8;

  data_.assign(num_bytes, '\0');

  for (const auto& key : keys) {
    // Double-hashing: h(i) = (h1 + i * h2) % num_bits
    uint32_t h1 = Hash(key, 0xbc9f1d34);
    uint32_t h2 = Hash(key, 0xef017832);

    for (size_t i = 0; i < num_hashes_; ++i) {
      uint32_t bit_pos =
          static_cast<uint32_t>((h1 + static_cast<uint64_t>(i) * h2) %
                                num_bits);
      data_[bit_pos / 8] |= static_cast<char>(1 << (bit_pos % 8));
    }
  }
}
//  SetRawData — rebuild from serialized data

void BloomFilter::SetRawData(const std::string& data, size_t num_hashes) {
  data_ = data;
  num_hashes_ = num_hashes;
}
//  MayContain

bool BloomFilter::MayContain(const Slice& key) const {
  if (data_.empty()) return false;

  size_t num_bits = data_.size() * 8;

  uint32_t h1 = Hash(key, 0xbc9f1d34);
  uint32_t h2 = Hash(key, 0xef017832);

  for (size_t i = 0; i < num_hashes_; ++i) {
    uint32_t bit_pos =
        static_cast<uint32_t>((h1 + static_cast<uint64_t>(i) * h2) % num_bits);
    if ((static_cast<uint8_t>(data_[bit_pos / 8]) & (1 << (bit_pos % 8))) ==
        0) {
      return false;
    }
  }
  return true;
}
//  Accessors

const std::string& BloomFilter::GetRawData() const { return data_; }

size_t BloomFilter::GetNumHashes() const { return num_hashes_; }

}  // namespace splitkv
