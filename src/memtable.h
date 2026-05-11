#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "avl_tree.h"
#include "splitkv/slice.h"
#include "splitkv/status.h"

namespace splitkv {
//  VLogPointer — locates a value inside the Value Log

struct VLogPointer {
  uint32_t file_id;     // VLog file ID
  uint64_t offset;      // Byte offset in VLog file
  uint32_t value_size;  // Size of value in VLog

  /// Encode into a fixed-size binary string (4 + 8 + 4 = 16 bytes).
  std::string Encode() const;

  /// Decode from a binary string produced by Encode().
  static VLogPointer Decode(const Slice& data);
};
//  ValueType / InternalEntry

enum class ValueType : uint8_t {
  kValue = 0x01,
  kDeletion = 0x02,
};

struct InternalEntry {
  VLogPointer vlog_ptr;
  uint64_t sequence;  // Monotonically increasing sequence number
  ValueType type;
};
//  MemTable

class MemTable {
 public:
  explicit MemTable(size_t size_limit);

  // ---- Thread-safe mutations ----

  /// Insert a key → VLogPointer mapping.
  Status Put(const Slice& key, const VLogPointer& ptr, uint64_t seq);

  /// Mark a key as deleted (tombstone).
  Status Delete(const Slice& key, uint64_t seq);

  /// Look up a key.  Returns OK and fills *entry if found,
  /// or Status::NotFound() otherwise.
  Status Get(const Slice& key, InternalEntry* entry) const;

  // ---- Capacity ----

  /// True when approximate memory usage >= size limit.
  bool ShouldFlush() const;

  /// Approximate heap bytes consumed by this memtable.
  size_t ApproximateMemoryUsage() const;

  /// Number of entries (puts + deletes).
  size_t Count() const;

  // ---- Iteration (for flushing to SSTable) ----

  /// Snapshot sorted entries for flush.  Thread-safe (takes shared lock).
  std::vector<std::pair<std::string, InternalEntry>> GetSortedEntries() const;

 private:
  AVLTree<std::string, InternalEntry> tree_;
  size_t size_limit_;
  std::atomic<size_t> memory_usage_;
  mutable std::shared_mutex mutex_;

  // Approximate per-entry overhead: AVL node pointers (left, right),
  // height, key string, InternalEntry struct, allocator overhead.
  static constexpr size_t kNodeOverhead = 48;
};

}  // namespace splitkv
