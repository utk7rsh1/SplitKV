#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "splitkv/slice.h"

namespace splitkv {

class BloomFilter {
 public:
  /// Construct a bloom filter that uses `bits_per_key` bits per key.
  explicit BloomFilter(size_t bits_per_key);

  /// Populate the filter from the given set of keys.
  void CreateFilter(const std::vector<Slice>& keys);

  /// Rebuild the filter from previously-serialized data.
  void SetRawData(const std::string& data, size_t num_hashes);

  /// Returns true if the key *might* be in the set (false positives possible).
  /// Returns false if the key is *definitely not* in the set.
  bool MayContain(const Slice& key) const;

  /// Access the raw bit-array for serialization.
  const std::string& GetRawData() const;

  /// Number of hash functions used.
  size_t GetNumHashes() const;

 private:
  /// MurmurHash3-inspired hash with a seed.
  static uint32_t Hash(const Slice& key, uint32_t seed);

  size_t bits_per_key_;
  size_t num_hashes_;  // k = max(1, bits_per_key * 0.69), capped at 30
  std::string data_;   // bit array stored as bytes
};

}  // namespace splitkv
