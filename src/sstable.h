#pragma once

#include <string>
#include <vector>
#include <memory>

#include "splitkv/status.h"
#include "splitkv/slice.h"
#include "splitkv/options.h"
#include "bloom_filter.h"
#include "memtable.h"
#include "file_util.h"

namespace splitkv {

// Metadata about an SSTable file
struct SSTableMeta {
    uint32_t file_id;          // Unique SSTable file ID
    int level;                 // Compaction level (0 = freshly flushed)
    std::string smallest_key;  // Smallest key in this SSTable
    std::string largest_key;   // Largest key in this SSTable
    uint64_t num_entries;      // Number of entries
    uint64_t file_size;        // File size in bytes
};

// Writes a new SSTable file from sorted entries
class SSTableWriter {
public:
    SSTableWriter(const std::string& path, const Options& options);
    ~SSTableWriter();
    
    Status Open();
    
    // Add entries in sorted key order
    Status Add(const Slice& key, const InternalEntry& entry);
    
    // Finalize: write sparse index, bloom filter, and footer
    Status Finish();
    
    uint64_t NumEntries() const;
    uint64_t FileSize() const;
    const std::string& SmallestKey() const;
    const std::string& LargestKey() const;
    
private:
    std::string path_;
    Options options_;
    std::unique_ptr<SequentialFileWriter> writer_;
    
    // Tracking
    uint64_t num_entries_;
    uint64_t data_offset_;  // current write offset in data block
    std::string smallest_key_;
    std::string largest_key_;
    
    // Sparse index: every N entries, record (key, offset)
    struct SparseIndexEntry {
        std::string key;
        uint64_t data_offset;
    };
    std::vector<SparseIndexEntry> sparse_index_;
    size_t entries_since_last_index_;
    
    // Bloom filter keys collector
    std::vector<std::string> key_storage_;
    
    bool finished_;
};

// Reads an SSTable file
class SSTableReader {
public:
    SSTableReader();
    ~SSTableReader();
    
    // Open and load index + bloom filter into memory
    static Status Open(const std::string& path, const Options& options,
                       SSTableReader** reader);
    
    // Point lookup: check bloom filter, then binary search sparse index,
    // then scan data block
    Status Get(const Slice& key, InternalEntry* entry) const;
    
    // Iteration support for compaction
    class Iterator {
    public:
        Iterator(const SSTableReader* reader);
        ~Iterator();
        
        Status SeekToFirst();
        Status Seek(const Slice& target);
        bool Valid() const;
        Status Next();
        Slice Key() const;
        const InternalEntry& Entry() const;
        
    private:
        const SSTableReader* reader_;
        std::string current_key_;
        InternalEntry current_entry_;
        uint64_t current_offset_;
        uint64_t data_end_offset_;  // where data block ends
        bool valid_;
        
        Status ReadEntryAt(uint64_t offset);
    };
    
    std::unique_ptr<Iterator> NewIterator() const;
    
    uint64_t NumEntries() const;
    const std::string& SmallestKey() const;
    const std::string& LargestKey() const;
    const std::string& FilePath() const;
    
private:
    std::string path_;
    Options options_;
    std::unique_ptr<RandomAccessFileReader> file_;
    
    // Loaded into memory
    BloomFilter bloom_;
    
    struct SparseIndexEntry {
        std::string key;
        uint64_t data_offset;
    };
    std::vector<SparseIndexEntry> sparse_index_;
    
    uint64_t num_entries_;
    uint64_t data_end_offset_;  // = sparse_index_offset (start of index block)
    std::string smallest_key_;
    std::string largest_key_;
    
    // Internal helpers
    Status LoadFooter();
    Status LoadSparseIndex(uint64_t offset, uint64_t size);
    Status LoadBloomFilter(uint64_t offset, uint64_t size);
    Status ReadEntryAt(uint64_t offset, std::string* key, InternalEntry* entry,
                       uint64_t* next_offset) const;
};

} // namespace splitkv
