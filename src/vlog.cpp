#include "vlog.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <regex>

#include "coding.h"
#include "crc32.h"

namespace splitkv {
// Helpers

uint32_t VLog::RecordSize(uint32_t key_len, uint32_t value_len) {
    // CRC(4) + Type(1) + KLen(4) + Key(key_len) + VLen(4) + Value(value_len)
    return 4 + 1 + 4 + key_len + 4 + value_len;
}

std::string VLog::VLogFilePath(uint32_t file_id) const {
    std::ostringstream oss;
    oss << db_path_ << "/vlog_" << std::setw(5) << std::setfill('0') << file_id
        << ".dat";
    return oss.str();
}
// Construction / Destruction

VLog::VLog(const std::string& db_path, const Options& options)
    : db_path_(db_path),
      options_(options),
      current_file_id_(0),
      current_offset_(0) {}

VLog::~VLog() { Close(); }
// Open / Recovery

Status VLog::RecoverFileIds() {
    std::vector<std::string> children;
    Status s = GetChildren(db_path_, &children);
    if (!s.ok()) return s;

    uint32_t max_id = 0;
    // Match files like vlog_00001.dat
    std::regex vlog_re("vlog_(\\d{5})\\.dat");
    for (const auto& name : children) {
        std::smatch m;
        if (std::regex_match(name, m, vlog_re)) {
            uint32_t id = static_cast<uint32_t>(std::stoul(m[1].str()));
            if (id > max_id) max_id = id;
        }
    }

    current_file_id_ = (max_id == 0) ? 1 : max_id;
    return Status::OK();
}

Status VLog::Open() {
    // Ensure directory exists
    Status s = CreateDir(db_path_);
    // CreateDir may return error if it already exists — that's fine.
    // We only care if the directory really doesn't exist afterwards.

    s = RecoverFileIds();
    if (!s.ok()) return s;

    std::string path = VLogFilePath(current_file_id_);

    if (FileExists(path)) {
        // Open existing file: determine its size so we can append
        uint64_t size = 0;
        s = FileSize(path, &size);
        if (!s.ok()) return s;
        current_offset_ = size;
    } else {
        current_offset_ = 0;
    }

    // Open file for appending
    writer_ = std::make_unique<SequentialFileWriter>();
    s = writer_->Open(path, /* append = */ true);
    if (!s.ok()) {
        writer_.reset();
        return s;
    }

    return Status::OK();
}
// RotateFile

Status VLog::RotateFile() {
    // Sync and close current writer
    if (writer_) {
        Status s = writer_->Sync();
        if (!s.ok()) return s;
        s = writer_->Close();
        if (!s.ok()) return s;
        writer_.reset();
    }

    current_file_id_++;
    current_offset_ = 0;

    std::string path = VLogFilePath(current_file_id_);
    writer_ = std::make_unique<SequentialFileWriter>();
    Status s = writer_->Open(path, /* append = */ false);
    if (!s.ok()) {
        writer_.reset();
        return s;
    }
    return Status::OK();
}
// Append

Status VLog::Append(const Slice& key, const Slice& value, ValueType type,
                    VLogPointer* ptr) {
    if (!writer_) {
        return Status::IOError("VLog not open for writing");
    }

    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t val_len = static_cast<uint32_t>(value.size());
    const uint32_t record_size = RecordSize(key_len, val_len);

    // Check if we need to rotate to a new file
    if (current_offset_ + record_size > options_.vlog_file_size_limit &&
        current_offset_ > 0) {
        Status s = RotateFile();
        if (!s.ok()) return s;
    }

    // Build the payload (everything after CRC): Type(1) + KLen(4) + Key + VLen(4) + Value
    const uint32_t payload_size = 1 + 4 + key_len + 4 + val_len;
    std::string payload;
    payload.reserve(payload_size);

    payload.push_back(static_cast<char>(type));            // Type (1 byte)
    PutFixed32(&payload, key_len);                         // KLen (4 bytes LE)
    payload.append(key.data(), key.size());                // Key
    PutFixed32(&payload, val_len);                         // VLen (4 bytes LE)
    payload.append(value.data(), value.size());            // Value

    // Compute CRC32 over the payload
    uint32_t crc = CRC32(payload.data(), payload.size());

    // Build the full record: CRC(4) + payload
    std::string record;
    record.reserve(4 + payload.size());
    PutFixed32(&record, crc);
    record.append(payload);

    // Fill in the VLogPointer BEFORE writing (we know the offset)
    if (ptr) {
        ptr->file_id = current_file_id_;
        ptr->offset = current_offset_;
        ptr->value_size = val_len;
    }

    // Write the record
    Status s = writer_->Append(Slice(record));
    if (!s.ok()) return s;

    current_offset_ += record.size();

    // Flush the libc buffer to the OS so that concurrent readers can see it
    s = writer_->Flush();
    if (!s.ok()) return s;

    // Sync if the user requested synchronous writes
    if (options_.sync_writes) {
        s = writer_->Sync();
        if (!s.ok()) return s;
    }

    return Status::OK();
}
// OpenReader (lazy, cached)

Status VLog::OpenReader(uint32_t file_id) const {
    // Caller must hold reader_mutex_ or guarantee exclusive access.
    auto it = readers_.find(file_id);
    if (it != readers_.end()) return Status::OK();

    std::string path = VLogFilePath(file_id);
    auto reader = std::make_unique<RandomAccessFileReader>();
    Status s = reader->Open(path);
    if (!s.ok()) return s;

    readers_[file_id] = std::move(reader);
    return Status::OK();
}
// Read

Status VLog::Read(const VLogPointer& ptr, std::string* key,
                  std::string* value) const {
    std::lock_guard<std::mutex> lock(reader_mutex_);

    Status s = OpenReader(ptr.file_id);
    if (!s.ok()) return s;

    auto it = readers_.find(ptr.file_id);
    if (it == readers_.end()) {
        return Status::IOError("Reader not found after opening");
    }
    RandomAccessFileReader* reader = it->second.get();

    // We need to read the full record starting at ptr.offset.
    // First read the fixed header: CRC(4) + Type(1) + KLen(4) = 9 bytes
    std::string header_buf;
    s = reader->Read(ptr.offset, kRecordHeaderSize, &header_buf);
    if (!s.ok()) return s;
    if (header_buf.size() < kRecordHeaderSize) {
        return Status::Corruption("VLog record header truncated");
    }

    const char* p = header_buf.data();
    uint32_t stored_crc = DecodeFixed32(p);
    p += 4;
    // uint8_t type_byte = static_cast<uint8_t>(*p);  // we don't need the type for Read
    p += 1;
    uint32_t key_len = DecodeFixed32(p);
    p += 4;

    // Now read key + VLen(4) + Value
    const uint32_t remaining = key_len + 4 + ptr.value_size;
    std::string data_buf;
    s = reader->Read(ptr.offset + kRecordHeaderSize, remaining, &data_buf);
    if (!s.ok()) return s;
    if (data_buf.size() < remaining) {
        return Status::Corruption("VLog record data truncated");
    }

    const char* dp = data_buf.data();
    if (key) {
        key->assign(dp, key_len);
    }
    dp += key_len;

    uint32_t val_len = DecodeFixed32(dp);
    dp += 4;
    if (val_len != ptr.value_size) {
        return Status::Corruption("VLog value size mismatch");
    }

    if (value) {
        value->assign(dp, val_len);
    }

    // Verify CRC: covers Type(1) + KLen(4) + Key + VLen(4) + Value
    // Reconstruct the payload that was checksummed
    const uint32_t payload_size = 1 + 4 + key_len + 4 + val_len;
    // The payload starts at offset+4 in the file (right after CRC).
    // We already have the pieces: type byte from header, and data_result.
    // Reconstruct for CRC check:
    std::string payload;
    payload.reserve(payload_size);
    // Type byte is at header_buf.data() + 4
    payload.push_back(header_buf.data()[4]);
    // KLen(4) is at header_buf.data() + 5
    payload.append(header_buf.data() + 5, 4);
    // Key + VLen + Value from data_result
    payload.append(data_buf.data(), data_buf.size());

    uint32_t computed_crc = CRC32(payload.data(), payload.size());
    if (computed_crc != stored_crc) {
        return Status::Corruption("VLog CRC mismatch");
    }

    return Status::OK();
}
// NewReader (GC scanning)

Status VLog::NewReader(
    uint32_t file_id, uint64_t start_offset,
    std::function<Status(const Slice& key, const Slice& value, ValueType type,
                         uint64_t offset)>
        callback) const {
    // Open the file with a SequentialFileReader for sequential scanning
    std::string path = VLogFilePath(file_id);
    SequentialFileReader reader;
    Status s = reader.Open(path);
    if (!s.ok()) return s;

    // Skip to start_offset
    if (start_offset > 0) {
        s = reader.Skip(start_offset);
        if (!s.ok()) return s;
    }

    uint64_t offset = start_offset;

    // Get file size to know when to stop
    uint64_t file_size = 0;
    s = FileSize(path, &file_size);
    if (!s.ok()) return s;

    while (offset < file_size) {
        // Read header: CRC(4) + Type(1) + KLen(4) = 9 bytes
        if (offset + kRecordHeaderSize > file_size) break;  // truncated

        std::string header_buf;
        s = reader.Read(kRecordHeaderSize, &header_buf);
        if (!s.ok()) return s;
        if (header_buf.size() < kRecordHeaderSize) break;  // truncated

        const char* hp = header_buf.data();
        uint32_t stored_crc = DecodeFixed32(hp);
        hp += 4;
        uint8_t type_byte = static_cast<uint8_t>(*hp);
        hp += 1;
        uint32_t key_len = DecodeFixed32(hp);

        // Read key
        std::string key_buf;
        s = reader.Read(key_len, &key_buf);
        if (!s.ok()) return s;
        if (key_buf.size() < key_len) break;  // truncated

        // Read VLen(4)
        std::string vlen_buf;
        s = reader.Read(4, &vlen_buf);
        if (!s.ok()) return s;
        if (vlen_buf.size() < 4) break;  // truncated

        uint32_t val_len = DecodeFixed32(vlen_buf.data());

        // Read value
        std::string val_buf;
        s = reader.Read(val_len, &val_buf);
        if (!s.ok()) return s;
        if (val_buf.size() < val_len) break;  // truncated

        // Verify CRC: covers Type(1) + KLen(4) + Key + VLen(4) + Value
        std::string payload;
        uint32_t payload_size = 1 + 4 + key_len + 4 + val_len;
        payload.reserve(payload_size);
        payload.push_back(static_cast<char>(type_byte));
        payload.append(header_buf.data() + 5, 4);  // KLen bytes
        payload.append(key_buf.data(), key_buf.size());
        payload.append(vlen_buf.data(), 4);
        payload.append(val_buf.data(), val_buf.size());

        uint32_t computed_crc = CRC32(payload.data(), payload.size());
        if (computed_crc != stored_crc) {
            // Corrupted record — stop scanning (could be partial write)
            break;
        }

        uint64_t record_offset = offset;
        uint32_t record_size = RecordSize(key_len, val_len);
        offset += record_size;

        // Invoke callback
        ValueType vtype = static_cast<ValueType>(type_byte);
        s = callback(Slice(key_buf.data(), key_buf.size()),
                     Slice(val_buf.data(), val_buf.size()),
                     vtype, record_offset);
        if (!s.ok()) return s;
    }

    reader.Close();
    return Status::OK();
}
// Accessors

uint32_t VLog::CurrentFileId() const { return current_file_id_; }

uint64_t VLog::CurrentOffset() const { return current_offset_; }
// Sync / Close

Status VLog::Sync() {
    if (!writer_) return Status::OK();
    return writer_->Sync();
}

Status VLog::RemoveReader(uint32_t file_id) {
    std::lock_guard<std::mutex> lock(reader_mutex_);
    auto it = readers_.find(file_id);
    if (it != readers_.end()) {
        if (it->second) {
            it->second->Close();
        }
        readers_.erase(it);
    }
    return Status::OK();
}

Status VLog::Close() {
    Status s = Status::OK();
    if (writer_) {
        s = writer_->Sync();
        Status s2 = writer_->Close();
        if (s.ok()) s = s2;
        writer_.reset();
    }

    std::lock_guard<std::mutex> lock(reader_mutex_);
    for (auto& pair : readers_) {
        if (pair.second) {
            pair.second->Close();
        }
    }
    readers_.clear();

    return s;
}

}  // namespace splitkv
