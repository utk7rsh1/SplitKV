#pragma once
#include <string>
#include <vector>
#include "splitkv/slice.h"

namespace splitkv {

class WriteBatch {
public:
    WriteBatch() = default;
    
    void Put(const Slice& key, const Slice& value);
    void Delete(const Slice& key);
    void Clear();
    size_t Count() const;
    
    struct Entry {
        enum Type { kPut, kDelete };
        Type type;
        std::string key;
        std::string value;  // empty for delete
    };
    
    const std::vector<Entry>& Entries() const { return entries_; }
    
private:
    std::vector<Entry> entries_;
};

} // namespace splitkv
