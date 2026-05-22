#include "splitkv/write_batch.h"

namespace splitkv {

void WriteBatch::Put(const Slice& key, const Slice& value) {
    entries_.push_back({Entry::kPut, key.ToString(), value.ToString()});
}

void WriteBatch::Delete(const Slice& key) {
    entries_.push_back({Entry::kDelete, key.ToString(), ""});
}

void WriteBatch::Clear() {
    entries_.clear();
}

size_t WriteBatch::Count() const {
    return entries_.size();
}

} // namespace splitkv
