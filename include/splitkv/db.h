#pragma once
#include <string>
#include "splitkv/status.h"
#include "splitkv/slice.h"
#include "splitkv/options.h"

namespace splitkv {

class WriteBatch;

class DB {
public:
    static Status Open(const Options& options, const std::string& path, DB** db);
    
    virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;
    virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;
    virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
    virtual Status Write(const WriteOptions& options, WriteBatch* batch) = 0;
    
    // Convenience methods without options
    Status Put(const Slice& key, const Slice& value) {
        return Put(WriteOptions(), key, value);
    }
    Status Get(const Slice& key, std::string* value) {
        return Get(ReadOptions(), key, value);
    }
    Status Delete(const Slice& key) {
        return Delete(WriteOptions(), key);
    }
    
    // Trigger manual compaction
    virtual Status CompactRange() = 0;
    
    // Trigger manual garbage collection
    virtual Status RunGC() = 0;
    
    // Get database statistics as a string
    virtual std::string GetStats() const = 0;
    
    virtual ~DB() = default;
    
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    
protected:
    DB() = default;
};

} // namespace splitkv
