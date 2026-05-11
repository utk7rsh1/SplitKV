#include "memtable.h"

#include <cstring>

#include <mutex>
#include <shared_mutex>
#include "coding.h"

namespace splitkv {

// ===========================================================================
//  VLogPointer
// ===========================================================================

std::string VLogPointer::Encode() const {
  std::string dst;
  dst.reserve(16);  // 4 + 8 + 4
  PutFixed32(&dst, file_id);
  PutFixed64(&dst, offset);
  PutFixed32(&dst, value_size);
  return dst;
}

VLogPointer VLogPointer::Decode(const Slice& data) {
  VLogPointer ptr{};
  const char* p = data.data();
  ptr.file_id = DecodeFixed32(p);
  ptr.offset = DecodeFixed64(p + 4);
  ptr.value_size = DecodeFixed32(p + 12);
  return ptr;
}

// ===========================================================================
//  MemTable
// ===========================================================================

MemTable::MemTable(size_t size_limit)
    : size_limit_(size_limit), memory_usage_(0) {}
//  Put

Status MemTable::Put(const Slice& key, const VLogPointer& ptr, uint64_t seq) {
  InternalEntry entry;
  entry.vlog_ptr = ptr;
  entry.sequence = seq;
  entry.type = ValueType::kValue;

  std::string key_str = key.ToString();
  size_t entry_mem = key_str.size() + sizeof(InternalEntry) + kNodeOverhead;

  {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // If the key already exists the AVL tree will update in-place;
    // we don't add extra memory in that case.  For simplicity we
    // always account for it — the "approximate" contract allows this.
    InternalEntry existing;
    bool already_exists = tree_.Find(key_str, &existing);

    tree_.Insert(key_str, entry);

    if (!already_exists) {
      memory_usage_.fetch_add(entry_mem, std::memory_order_relaxed);
    }
  }

  return Status::OK();
}
//  Delete (tombstone)

Status MemTable::Delete(const Slice& key, uint64_t seq) {
  InternalEntry entry;
  entry.vlog_ptr = VLogPointer{0, 0, 0};
  entry.sequence = seq;
  entry.type = ValueType::kDeletion;

  std::string key_str = key.ToString();
  size_t entry_mem = key_str.size() + sizeof(InternalEntry) + kNodeOverhead;

  {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    InternalEntry existing;
    bool already_exists = tree_.Find(key_str, &existing);

    tree_.Insert(key_str, entry);

    if (!already_exists) {
      memory_usage_.fetch_add(entry_mem, std::memory_order_relaxed);
    }
  }

  return Status::OK();
}
//  Get

Status MemTable::Get(const Slice& key, InternalEntry* entry) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  std::string key_str = key.ToString();
  InternalEntry found;
  if (tree_.Find(key_str, &found)) {
    if (entry) *entry = found;
    return Status::OK();
  }
  return Status::NotFound("key not found in memtable");
}
//  Capacity helpers

bool MemTable::ShouldFlush() const {
  return memory_usage_.load(std::memory_order_relaxed) >= size_limit_;
}

size_t MemTable::ApproximateMemoryUsage() const {
  return memory_usage_.load(std::memory_order_relaxed);
}

size_t MemTable::Count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return tree_.Size();
}
//  GetSortedEntries  (for flush)

std::vector<std::pair<std::string, InternalEntry>> MemTable::GetSortedEntries()
    const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  std::vector<std::pair<std::string, InternalEntry>> result;
  result.reserve(tree_.Size());

  for (auto it = tree_.begin(); it != tree_.end(); ++it) {
    auto [k, v] = *it;
    result.emplace_back(std::string(k), v);
  }

  return result;
}

}  // namespace splitkv
