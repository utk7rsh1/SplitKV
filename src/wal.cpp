#include "wal.h"

#include <iomanip>
#include <sstream>

#include "coding.h"
#include "crc32.h"

namespace splitkv {
// WAL file path helper

std::string WAL::WALFilePath(const std::string& db_path, uint64_t wal_id) {
    std::ostringstream oss;
    oss << db_path << "/wal_" << std::setw(5) << std::setfill('0') << wal_id
        << ".log";
    return oss.str();
}
// Construction / Destruction

WAL::WAL(const std::string& db_path) : db_path_(db_path), current_id_(0) {}

WAL::~WAL() { Close(); }
// Create

Status WAL::Create(uint64_t wal_id) {
    // Close any previously open writer
    if (writer_) {
        Status s = writer_->Close();
        if (!s.ok()) return s;
        writer_.reset();
    }

    current_id_ = wal_id;
    std::string path = WALFilePath(db_path_, wal_id);

    writer_ = std::make_unique<SequentialFileWriter>();
    Status s = writer_->Open(path, /* append = */ false);
    if (!s.ok()) {
        writer_.reset();
        return s;
    }

    return Status::OK();
}
// AddRecord

Status WAL::AddRecord(const WALRecord& record) {
    if (!writer_) {
        return Status::IOError("WAL not open for writing");
    }

    // Build the inner content: Type(1) + SeqNo(8) + Payload
    // Then Len = size of that content.
    //
    // Payload for Put:    KLen(4) + Key + VLogFileId(4) + VLogOffset(8) + VLogSize(4)
    // Payload for Delete: KLen(4) + Key

    std::string inner;

    // Type (1 byte)
    inner.push_back(static_cast<char>(record.type));

    // SeqNo (8 bytes LE)
    PutFixed64(&inner, record.sequence);

    // KLen (4 bytes LE)
    uint32_t key_len = static_cast<uint32_t>(record.key.size());
    PutFixed32(&inner, key_len);

    // Key
    inner.append(record.key);

    // VLog pointer fields (only for Put)
    if (record.type == ValueType::kValue) {
        PutFixed32(&inner, record.vlog_ptr.file_id);
        PutFixed64(&inner, record.vlog_ptr.offset);
        PutFixed32(&inner, record.vlog_ptr.value_size);
    }

    // Len = size of inner content (Type + SeqNo + Payload)
    uint32_t len = static_cast<uint32_t>(inner.size());

    // Build the checksummed portion: Len(4) + inner
    std::string checksummed;
    checksummed.reserve(4 + inner.size());
    PutFixed32(&checksummed, len);
    checksummed.append(inner);

    // Compute CRC32 over checksummed portion (Len + Type + SeqNo + Payload)
    uint32_t crc = CRC32(checksummed.data(), checksummed.size());

    // Build final record: CRC(4) + checksummed
    std::string full_record;
    full_record.reserve(4 + checksummed.size());
    PutFixed32(&full_record, crc);
    full_record.append(checksummed);

    // Write
    Status s = writer_->Append(Slice(full_record));
    if (!s.ok()) return s;

    // Flush to OS buffer
    s = writer_->Flush();
    if (!s.ok()) return s;

    return Status::OK();
}
// Sync / Close

Status WAL::Sync() {
    if (!writer_) return Status::OK();
    return writer_->Sync();
}

Status WAL::Close() {
    Status s = Status::OK();
    if (writer_) {
        s = writer_->Sync();
        Status s2 = writer_->Close();
        if (s.ok()) s = s2;
        writer_.reset();
    }
    return s;
}
// CurrentId

uint64_t WAL::CurrentId() const { return current_id_; }
// Recover (static)

Status WAL::Recover(const std::string& wal_path,
                    std::vector<WALRecord>* records) {
    if (!records) {
        return Status::IOError("records pointer is null");
    }
    records->clear();

    // Check if the file exists
    if (!FileExists(wal_path)) {
        return Status::NotFound("WAL file not found: " + wal_path);
    }

    // Get file size
    uint64_t file_size = 0;
    Status s = FileSize(wal_path, &file_size);
    if (!s.ok()) return s;

    if (file_size == 0) {
        return Status::OK();  // empty WAL
    }

    // Open for sequential reading
    SequentialFileReader reader;
    s = reader.Open(wal_path);
    if (!s.ok()) return s;

    uint64_t offset = 0;

    while (offset < file_size) {
        // Need at least CRC(4) + Len(4) = 8 bytes for any record
        if (offset + 8 > file_size) break;  // truncated header, stop

        // Read CRC (4 bytes)
        std::string crc_buf;
        s = reader.Read(4, &crc_buf);
        if (!s.ok()) break;
        if (crc_buf.size() < 4) break;  // truncated
        uint32_t stored_crc = DecodeFixed32(crc_buf.data());
        offset += 4;

        // Read Len (4 bytes)
        std::string len_buf;
        s = reader.Read(4, &len_buf);
        if (!s.ok()) break;
        if (len_buf.size() < 4) break;  // truncated
        uint32_t len = DecodeFixed32(len_buf.data());
        offset += 4;

        // Sanity check len to avoid huge allocations on corruption
        if (len == 0 || offset + len > file_size) break;  // truncated payload

        // Read the inner content: Type(1) + SeqNo(8) + Payload
        std::string inner_buf;
        s = reader.Read(len, &inner_buf);
        if (!s.ok()) break;
        if (inner_buf.size() < len) break;  // truncated
        offset += len;

        // Verify CRC: covers Len(4) + inner content
        std::string checksummed;
        checksummed.reserve(4 + len);
        PutFixed32(&checksummed, len);
        checksummed.append(inner_buf.data(), inner_buf.size());

        uint32_t computed_crc = CRC32(checksummed.data(), checksummed.size());
        if (computed_crc != stored_crc) {
            // CRC mismatch — likely corruption or torn write. Stop recovery here.
            break;
        }

        // Parse the inner content
        const char* p = inner_buf.data();
        const char* end = p + inner_buf.size();

        // Type (1 byte)
        if (p + 1 > end) break;
        uint8_t type_byte = static_cast<uint8_t>(*p);
        p += 1;

        // SeqNo (8 bytes)
        if (p + 8 > end) break;
        uint64_t seq = DecodeFixed64(p);
        p += 8;

        // KLen (4 bytes)
        if (p + 4 > end) break;
        uint32_t key_len = DecodeFixed32(p);
        p += 4;

        // Key
        if (p + key_len > end) break;
        std::string key(p, key_len);
        p += key_len;

        WALRecord rec;
        rec.type = static_cast<ValueType>(type_byte);
        rec.sequence = seq;
        rec.key = std::move(key);

        if (rec.type == ValueType::kValue) {
            // VLogFileId(4) + VLogOffset(8) + VLogSize(4) = 16 bytes
            if (p + 16 > end) break;
            rec.vlog_ptr.file_id = DecodeFixed32(p);
            p += 4;
            rec.vlog_ptr.offset = DecodeFixed64(p);
            p += 8;
            rec.vlog_ptr.value_size = DecodeFixed32(p);
            p += 4;
        } else {
            rec.vlog_ptr = VLogPointer{0, 0, 0};
        }

        records->push_back(std::move(rec));
    }

    reader.Close();
    return Status::OK();
}

}  // namespace splitkv
