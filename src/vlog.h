#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "splitkv/options.h"
#include "splitkv/slice.h"
#include "splitkv/status.h"

// Forward declarations for src-level types
#include "file_util.h"
#include "memtable.h"

namespace splitkv {

// Append-only value log storing raw key-value data.
//
// On-disk record format:
// +----------+----------+----------+-----------+----------+----------+
// | CRC32(4B)| Type(1B) | KLen(4B) | Key(KLen) | VLen(4B) | Val(VLen)|
// +----------+----------+----------+-----------+----------+----------+
//
// CRC32 covers Type + KLen + Key + VLen + Value bytes.
// Type: 0x01 = Value, 0x02 = Deletion.
// KLen and VLen are fixed 4-byte little-endian.
class VLog {
public:
    VLog(const std::string& db_path, const Options& options);
    ~VLog();

    // Open/recover the VLog. Scans directory for existing vlog files,
    // finds the highest ID, opens it for appending at end-of-file.
    Status Open();

    // Append a key-value pair, returns the VLogPointer.
    Status Append(const Slice& key, const Slice& value, ValueType type,
                  VLogPointer* ptr);

    // Read a value given a VLogPointer.
    Status Read(const VLogPointer& ptr, std::string* key,
                std::string* value) const;

    // Get current write offset info.
    uint32_t CurrentFileId() const;
    uint64_t CurrentOffset() const;

    // Sync current writer to disk.
    Status Sync();

    // Close the VLog (writer and all cached readers).
    Status Close();

    // For GC: scan records starting from start_offset in the given file,
    // invoking callback for each valid record.
    Status NewReader(
        uint32_t file_id, uint64_t start_offset,
        std::function<Status(const Slice& key, const Slice& value,
                             ValueType type, uint64_t offset)>
            callback) const;

    // For GC: close and erase reader for a specific file id.
    Status RemoveReader(uint32_t file_id);

private:
    std::string db_path_;
    Options options_;

    // Current writer
    std::unique_ptr<SequentialFileWriter> writer_;
    uint32_t current_file_id_;
    uint64_t current_offset_;

    // Readers cache (file_id -> reader), lazily populated
    mutable std::unordered_map<uint32_t, std::unique_ptr<RandomAccessFileReader>>
        readers_;
    mutable std::mutex reader_mutex_;

    // Returns "db_path_/vlog_XXXXX.dat"
    std::string VLogFilePath(uint32_t file_id) const;

    // Create a new VLog file when the current one exceeds the size limit.
    Status RotateFile();

    // Lazily open a RandomAccessFileReader for the given file_id and cache it.
    Status OpenReader(uint32_t file_id) const;

    // Scan the database directory for existing vlog_*.dat files and set
    // current_file_id_ to the highest found (or 1 if none).
    Status RecoverFileIds();

    // Size of the fixed header portion of a record (CRC + Type + KLen).
    static constexpr uint32_t kRecordHeaderSize = 4 + 1 + 4;  // 9 bytes
    // CRC(4) + Type(1) + KLen(4) + Key + VLen(4) + Value
    static uint32_t RecordSize(uint32_t key_len, uint32_t value_len);
};

}  // namespace splitkv
