#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "splitkv/db.h"
#include "splitkv/options.h"
#include "memtable.h"
#include "vlog.h"
#include "wal.h"
#include "sstable.h"

namespace splitkv {

class DBImpl : public DB {
public:
    DBImpl(const Options& options, const std::string& db_path);
    ~DBImpl() override;

    Status Open();

    Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
    Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;
    Status Delete(const WriteOptions& options, const Slice& key) override;
    Status Write(const WriteOptions& options, WriteBatch* batch) override;
    
    Status CompactRange() override;
    Status RunGC() override;
    std::string GetStats() const override;

private:
    Options options_;
    std::string db_path_;
    
    // VLog
    std::unique_ptr<VLog> vlog_;
    
    // WAL
    std::unique_ptr<WAL> wal_;
    uint64_t wal_id_;
    
    // MemTables
    std::shared_ptr<MemTable> memtable_;           // active, mutable
    std::deque<std::shared_ptr<MemTable>> immutable_memtables_;  // waiting to flush
    mutable std::mutex memtable_mutex_;
    
    // SSTable tracking per level
    struct SSTableInfo {
        SSTableMeta meta;
        std::shared_ptr<SSTableReader> reader;
    };
    std::vector<std::vector<SSTableInfo>> levels_;  // levels_[level] = list of SSTables
    mutable std::mutex sstable_mutex_;
    uint32_t next_sstable_id_;
    
    // Sequence number
    std::atomic<uint64_t> sequence_number_;
    
    // Background threads
    std::thread flush_thread_;
    std::atomic<bool> shutting_down_;
    std::condition_variable flush_cv_;
    std::mutex flush_mutex_;
    
    // Internal methods
    Status RecoverFromWAL();
    Status MaybeScheduleFlush();
    void BackgroundFlush();
    Status FlushMemTable(std::shared_ptr<MemTable> memtable);
    Status CompactLevel(int level);
    std::string SSTableFilePath(uint32_t file_id) const;
    Status LoadSSTables();  // Load existing SSTables on startup
    Status SaveManifest() const;  // Save SSTable metadata
    Status LoadManifest();  // Load SSTable metadata
    
    // Manifest file path
    std::string ManifestPath() const;
};

} // namespace splitkv
