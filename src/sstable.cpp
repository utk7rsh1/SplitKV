#include "sstable.h"

#include "coding.h"
#include "crc32.h"

namespace splitkv {

static const uint64_t kMagicNumber = 0x53706C69744B5600;
// SSTableWriter

SSTableWriter::SSTableWriter(const std::string& path, const Options& options)
    : path_(path), options_(options), num_entries_(0), data_offset_(0),
      entries_since_last_index_(0), finished_(false) {}

SSTableWriter::~SSTableWriter() {}

Status SSTableWriter::Open() {
    writer_ = std::make_unique<SequentialFileWriter>();
    return writer_->Open(path_);
}

Status SSTableWriter::Add(const Slice& key, const InternalEntry& entry) {
    if (finished_) {
        return Status::InvalidArgument("SSTableWriter already finished");
    }

    // Keep track of keys for bloom filter
    key_storage_.push_back(key.ToString());

    // Update smallest/largest keys
    if (num_entries_ == 0) {
        smallest_key_ = key.ToString();
    }
    largest_key_ = key.ToString();

    // Add sparse index entry if needed
    if (num_entries_ == 0 || entries_since_last_index_ >= options_.sparse_index_interval) {
        SparseIndexEntry index_entry;
        index_entry.key = key.ToString();
        index_entry.data_offset = data_offset_;
        sparse_index_.push_back(std::move(index_entry));
        entries_since_last_index_ = 0;
    }

    // Encode entry
    // KLen(varint32) + Key(KLen) + ValueType(1B) + Sequence(8B fixed) + VLogPointer(16B)
    std::string buf;
    PutVarint32(&buf, static_cast<uint32_t>(key.size()));
    buf.append(key.data(), key.size());
    buf.push_back(static_cast<char>(entry.type));
    PutFixed64(&buf, entry.sequence);
    
    // Encode VLogPointer
    buf.append(entry.vlog_ptr.Encode());

    Status s = writer_->Append(Slice(buf));
    if (!s.ok()) return s;

    data_offset_ += buf.size();
    num_entries_++;
    entries_since_last_index_++;

    return Status::OK();
}

Status SSTableWriter::Finish() {
    if (finished_) {
        return Status::InvalidArgument("SSTableWriter already finished");
    }

    uint64_t sparse_index_offset = data_offset_;
    
    // Write sparse index
    std::string index_buf;
    for (const auto& entry : sparse_index_) {
        PutVarint32(&index_buf, static_cast<uint32_t>(entry.key.size()));
        index_buf.append(entry.key);
        PutFixed64(&index_buf, entry.data_offset);
    }
    Status s = writer_->Append(Slice(index_buf));
    if (!s.ok()) return s;
    
    uint64_t sparse_index_size = index_buf.size();
    uint64_t bloom_offset = sparse_index_offset + sparse_index_size;

    // Write bloom filter
    std::vector<Slice> keys;
    keys.reserve(key_storage_.size());
    for (const auto& k : key_storage_) {
        keys.push_back(Slice(k));
    }
    
    BloomFilter bloom(options_.bloom_bits_per_key);
    bloom.CreateFilter(keys);
    
    const std::string& bloom_data = bloom.GetRawData();
    std::string bloom_buf;
    // Store num_hashes then the data
    PutFixed32(&bloom_buf, static_cast<uint32_t>(bloom.GetNumHashes()));
    bloom_buf.append(bloom_data);

    s = writer_->Append(Slice(bloom_buf));
    if (!s.ok()) return s;
    
    uint64_t bloom_size = bloom_buf.size();

    // Write footer (48 bytes)
    // sparse_index_offset (8B) + sparse_index_size (8B) + bloom_offset (8B) + bloom_size (8B) + num_entries (8B) + magic_number (8B)
    std::string footer;
    footer.reserve(48);
    PutFixed64(&footer, sparse_index_offset);
    PutFixed64(&footer, sparse_index_size);
    PutFixed64(&footer, bloom_offset);
    PutFixed64(&footer, bloom_size);
    PutFixed64(&footer, num_entries_);
    PutFixed64(&footer, kMagicNumber);

    s = writer_->Append(Slice(footer));
    if (!s.ok()) return s;

    s = writer_->Sync();
    if (!s.ok()) return s;
    s = writer_->Close();
    if (!s.ok()) return s;

    finished_ = true;
    return Status::OK();
}

uint64_t SSTableWriter::NumEntries() const { return num_entries_; }
uint64_t SSTableWriter::FileSize() const { return writer_->Offset(); }
const std::string& SSTableWriter::SmallestKey() const { return smallest_key_; }
const std::string& SSTableWriter::LargestKey() const { return largest_key_; }
// SSTableReader

SSTableReader::SSTableReader() : bloom_(0), num_entries_(0), data_end_offset_(0) {}
SSTableReader::~SSTableReader() {}

Status SSTableReader::Open(const std::string& path, const Options& options, SSTableReader** reader) {
    auto r = std::make_unique<SSTableReader>();
    r->path_ = path;
    r->options_ = options;
    r->file_ = std::make_unique<RandomAccessFileReader>();
    
    Status s = r->file_->Open(path);
    if (!s.ok()) return s;

    s = r->LoadFooter();
    if (!s.ok()) return s;

    *reader = r.release();
    return Status::OK();
}

Status SSTableReader::LoadFooter() {
    uint64_t file_size = 0;
    Status s = FileSize(path_, &file_size);
    if (!s.ok()) return s;

    if (file_size < 48) {
        return Status::Corruption("File too short to be an SSTable");
    }

    std::string footer_buf;
    s = file_->Read(file_size - 48, 48, &footer_buf);
    if (!s.ok()) return s;

    const char* p = footer_buf.data();
    uint64_t sparse_index_offset = DecodeFixed64(p); p += 8;
    uint64_t sparse_index_size = DecodeFixed64(p); p += 8;
    uint64_t bloom_offset = DecodeFixed64(p); p += 8;
    uint64_t bloom_size = DecodeFixed64(p); p += 8;
    num_entries_ = DecodeFixed64(p); p += 8;
    uint64_t magic = DecodeFixed64(p);

    if (magic != kMagicNumber) {
        return Status::Corruption("Not an SSTable (magic number mismatch)");
    }

    data_end_offset_ = sparse_index_offset;

    // Load Sparse Index
    s = LoadSparseIndex(sparse_index_offset, sparse_index_size);
    if (!s.ok()) return s;

    // Load Bloom Filter
    s = LoadBloomFilter(bloom_offset, bloom_size);
    if (!s.ok()) return s;

    return Status::OK();
}

Status SSTableReader::LoadSparseIndex(uint64_t offset, uint64_t size) {
    if (size == 0) return Status::OK();

    std::string buf;
    Status s = file_->Read(offset, size, &buf);
    if (!s.ok()) return s;

    const char* p = buf.data();
    const char* limit = p + size;

    while (p < limit) {
        uint32_t key_len = 0;
        p = DecodeVarint32(p, limit, &key_len);
        if (!p || p + key_len > limit) return Status::Corruption("Truncated sparse index key");

        std::string key(p, key_len);
        p += key_len;

        if (p + 8 > limit) return Status::Corruption("Truncated sparse index offset");
        uint64_t data_offset = DecodeFixed64(p);
        p += 8;

        sparse_index_.push_back({std::move(key), data_offset});
    }

    if (!sparse_index_.empty()) {
        smallest_key_ = sparse_index_.front().key;
        largest_key_ = sparse_index_.back().key; // approximate, accurate largest is in data blocks
    }

    return Status::OK();
}

Status SSTableReader::LoadBloomFilter(uint64_t offset, uint64_t size) {
    if (size < 4) return Status::OK(); // No bloom filter

    std::string buf;
    Status s = file_->Read(offset, size, &buf);
    if (!s.ok()) return s;

    const char* p = buf.data();
    uint32_t num_hashes = DecodeFixed32(p);
    
    std::string raw_data(p + 4, size - 4);
    bloom_.SetRawData(raw_data, num_hashes);

    return Status::OK();
}

Status SSTableReader::ReadEntryAt(uint64_t offset, std::string* key, InternalEntry* entry, uint64_t* next_offset) const {
    // Read an initial chunk to parse the length headers
    std::string buf;
    Status s = file_->Read(offset, 64, &buf);
    if (!s.ok() && !s.IsIOError()) return s; // Could be EOF
    if (buf.empty()) return Status::NotFound("EOF reached");

    const char* p = buf.data();
    const char* limit = p + buf.size();

    uint32_t key_len = 0;
    const char* q = DecodeVarint32(p, limit, &key_len);
    if (!q) {
        // Retry reading more if varint was truncated
        s = file_->Read(offset, 128 + key_len, &buf);
        if (!s.ok()) return s;
        p = buf.data();
        limit = p + buf.size();
        q = DecodeVarint32(p, limit, &key_len);
        if (!q) return Status::Corruption("Truncated key length varint");
    }

    // Now we know exactly how much data to read for the rest of the entry
    size_t varint_len = q - p;
    size_t total_len = varint_len + key_len + 1 + 8 + 16; // KLen + Key + Type + Seq + VLogPtr(16)
    
    if (buf.size() < total_len) {
        s = file_->Read(offset, total_len, &buf);
        if (!s.ok()) return s;
        p = buf.data();
        limit = p + buf.size();
        q = p + varint_len; // Skip varint again
    }

    if (q + key_len > limit) return Status::Corruption("Truncated key data");
    if (key) {
        key->assign(q, key_len);
    }
    q += key_len;

    if (q + 25 > limit) return Status::Corruption("Truncated entry metadata");
    
    entry->type = static_cast<ValueType>(*q);
    q += 1;

    entry->sequence = DecodeFixed64(q);
    q += 8;

    entry->vlog_ptr.file_id = DecodeFixed32(q);
    q += 4;
    entry->vlog_ptr.offset = DecodeFixed64(q);
    q += 8;
    entry->vlog_ptr.value_size = DecodeFixed32(q);
    q += 4;

    if (next_offset) {
        *next_offset = offset + total_len;
    }

    return Status::OK();
}

Status SSTableReader::Get(const Slice& key, InternalEntry* entry) const {
    if (!bloom_.GetRawData().empty() && !bloom_.MayContain(key)) {
        return Status::NotFound("Not in bloom filter");
    }

    if (sparse_index_.empty()) {
        return Status::NotFound("Empty sparse index");
    }

    // Binary search sparse index to find the block
    int left = 0;
    int right = static_cast<int>(sparse_index_.size()) - 1;
    int block_idx = 0;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (sparse_index_[mid].key == key.ToString()) {
            block_idx = mid;
            break;
        } else if (sparse_index_[mid].key < key.ToString()) {
            block_idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    uint64_t offset = sparse_index_[block_idx].data_offset;
    uint64_t end_offset = (block_idx + 1 < sparse_index_.size()) 
                        ? sparse_index_[block_idx + 1].data_offset 
                        : data_end_offset_;

    // Linear scan
    while (offset < end_offset) {
        std::string parsed_key;
        InternalEntry parsed_entry;
        uint64_t next_offset = 0;

        Status s = ReadEntryAt(offset, &parsed_key, &parsed_entry, &next_offset);
        if (!s.ok()) {
            return s;
        }

        if (parsed_key == key.ToString()) {
            if (entry) *entry = parsed_entry;
            return Status::OK();
        } else if (parsed_key > key.ToString()) {
            // Passed it
            break;
        }

        offset = next_offset;
    }

    return Status::NotFound("Key not found in block");
}

std::unique_ptr<SSTableReader::Iterator> SSTableReader::NewIterator() const {
    return std::make_unique<Iterator>(this);
}

uint64_t SSTableReader::NumEntries() const { return num_entries_; }
const std::string& SSTableReader::SmallestKey() const { return smallest_key_; }
const std::string& SSTableReader::LargestKey() const { return largest_key_; }
const std::string& SSTableReader::FilePath() const { return path_; }
// SSTableReader::Iterator

SSTableReader::Iterator::Iterator(const SSTableReader* reader)
    : reader_(reader), current_offset_(0), data_end_offset_(reader->data_end_offset_), valid_(false) {}

SSTableReader::Iterator::~Iterator() {}

Status SSTableReader::Iterator::SeekToFirst() {
    current_offset_ = 0;
    return ReadEntryAt(current_offset_);
}

Status SSTableReader::Iterator::Seek(const Slice& target) {
    if (reader_->sparse_index_.empty()) {
        valid_ = false;
        return Status::NotFound("Empty index");
    }

    int left = 0;
    int right = static_cast<int>(reader_->sparse_index_.size()) - 1;
    int block_idx = 0;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (reader_->sparse_index_[mid].key == target.ToString()) {
            block_idx = mid;
            break;
        } else if (reader_->sparse_index_[mid].key < target.ToString()) {
            block_idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    current_offset_ = reader_->sparse_index_[block_idx].data_offset;

    // Linear scan
    while (current_offset_ < data_end_offset_) {
        Status s = ReadEntryAt(current_offset_);
        if (!s.ok()) return s;

        if (current_key_ >= target.ToString()) {
            return Status::OK();
        }
    }

    valid_ = false;
    return Status::NotFound("Reached EOF");
}

bool SSTableReader::Iterator::Valid() const {
    return valid_;
}

Status SSTableReader::Iterator::Next() {
    if (!valid_) return Status::NotFound("Invalid iterator");
    return ReadEntryAt(current_offset_);
}

Slice SSTableReader::Iterator::Key() const {
    return Slice(current_key_);
}

const InternalEntry& SSTableReader::Iterator::Entry() const {
    return current_entry_;
}

Status SSTableReader::Iterator::ReadEntryAt(uint64_t offset) {
    if (offset >= data_end_offset_) {
        valid_ = false;
        return Status::NotFound("Reached end of data blocks");
    }

    uint64_t next_offset = 0;
    Status s = reader_->ReadEntryAt(offset, &current_key_, &current_entry_, &next_offset);
    if (!s.ok()) {
        valid_ = false;
        return s;
    }

    current_offset_ = next_offset;
    valid_ = true;
    return Status::OK();
}

} // namespace splitkv
