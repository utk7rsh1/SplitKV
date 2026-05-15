#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "splitkv/slice.h"
#include "splitkv/status.h"

#include "file_util.h"
#include "memtable.h"

namespace splitkv {

// A single record in the Write-Ahead Log.
struct WALRecord {
    ValueType type;
    uint64_t sequence;
    std::string key;
    VLogPointer vlog_ptr;  // only valid for Put (type == kValue)
};

// Sequential Write-Ahead Log for crash durability.
//
// Record format:
// +----------+----------+----------+----------+-----------+
// | CRC32(4B)| Len(4B)  | Type(1B) | SeqNo(8B)| Payload   |
// +----------+----------+----------+----------+-----------+
//
// CRC32 covers everything after CRC (Len + Type + SeqNo + Payload).
// Len = total bytes after CRC (1 + 8 + payload_len).
// Type: 0x01 = Put, 0x02 = Delete.
// Payload for Put: KLen(4B) + Key + VLogFileId(4B) + VLogOffset(8B) + VLogSize(4B).
// Payload for Delete: KLen(4B) + Key.
class WAL {
public:
    explicit WAL(const std::string& db_path);
    ~WAL();

    // Create new WAL file for writing.
    Status Create(uint64_t wal_id);

    // Append a record.
    Status AddRecord(const WALRecord& record);

    // Sync to disk.
    Status Sync();

    // Close current WAL.
    Status Close();

    // Recovery: read all records from a WAL file.
    // Tolerant of truncated trailing records (incomplete writes from power loss).
    static Status Recover(const std::string& wal_path,
                          std::vector<WALRecord>* records);

    // Get current WAL ID.
    uint64_t CurrentId() const;

    // WAL file path helper.
    static std::string WALFilePath(const std::string& db_path, uint64_t wal_id);

private:
    std::string db_path_;
    std::unique_ptr<SequentialFileWriter> writer_;
    uint64_t current_id_;
};

}  // namespace splitkv
