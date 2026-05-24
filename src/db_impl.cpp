#include "db_impl.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "splitkv/write_batch.h"
#include "file_util.h"

namespace splitkv {
// DB::Open (Static Factory)
Status DB::Open(const Options& options, const std::string& path, DB** db) {
    auto impl = new DBImpl(options, path);
    Status s = impl->Open();
    if (s.ok()) {
        *db = impl;
    } else {
        delete impl;
        *db = nullptr;
    }
    return s;
}
// Constructor / Destructor
DBImpl::DBImpl(const Options& options, const std::string& db_path)
    : options_(options),
      db_path_(db_path),
      wal_id_(0),
      next_sstable_id_(1),
      sequence_number_(1),
      shutting_down_(false) {
    levels_.resize(options_.max_sstable_levels);
}

DBImpl::~DBImpl() {
    shutting_down_.store(true);
    flush_cv_.notify_all();

    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }

    // Flush remaining memtable if not empty
    if (memtable_ && memtable_->Count() > 0) {
        FlushMemTable(memtable_);
    }

    // Also flush any immutable memtables left
    for (auto& m : immutable_memtables_) {
        FlushMemTable(m);
    }
}
// Open
Status DBImpl::Open() {
    Status s = CreateDir(db_path_);
    if (!s.ok() && !s.IsIOError()) return s; // Ignore IOError on existing dir

    // VLog
    vlog_ = std::make_unique<VLog>(db_path_, options_);
    s = vlog_->Open();
    if (!s.ok()) return s;

    // Manifest and SSTables
    s = LoadManifest();
    if (!s.ok() && !s.IsNotFound()) return s;

    // MemTable
    memtable_ = std::make_shared<MemTable>(options_.memtable_size_limit);

    // WAL
    wal_ = std::make_unique<WAL>(db_path_);
    s = RecoverFromWAL();
    if (!s.ok()) return s;

    // Start background threads
    flush_thread_ = std::thread(&DBImpl::BackgroundFlush, this);

    return Status::OK();
}
// Path Helpers
std::string DBImpl::ManifestPath() const {
    return db_path_ + "/MANIFEST";
}

std::string DBImpl::SSTableFilePath(uint32_t file_id) const {
    std::ostringstream oss;
    oss << db_path_ << "/sst_" << std::setw(5) << std::setfill('0') << file_id << ".sst";
    return oss.str();
}
// WAL Recovery
Status DBImpl::RecoverFromWAL() {
    std::vector<std::string> children;
    Status s = GetChildren(db_path_, &children);
    if (!s.ok()) return s;

    std::vector<uint64_t> wal_ids;
    for (const auto& name : children) {
        if (name.find("wal_") == 0 && name.find(".log") != std::string::npos) {
            uint64_t id = std::stoull(name.substr(4, 5));
            wal_ids.push_back(id);
        }
    }
    std::sort(wal_ids.begin(), wal_ids.end());

    uint64_t max_seq = sequence_number_.load();

    for (uint64_t id : wal_ids) {
        std::string wal_path = WAL::WALFilePath(db_path_, id);
        std::vector<WALRecord> records;
        s = WAL::Recover(wal_path, &records);
        if (!s.ok()) return s;

        for (const auto& record : records) {
            if (record.type == ValueType::kValue) {
                memtable_->Put(record.key, record.vlog_ptr, record.sequence);
            } else if (record.type == ValueType::kDeletion) {
                memtable_->Delete(record.key, record.sequence);
            }
            if (record.sequence > max_seq) {
                max_seq = record.sequence;
            }
        }
        
        // After recovering, flush if it is large enough
        if (memtable_->ShouldFlush()) {
            s = FlushMemTable(memtable_);
            if (!s.ok()) return s;
            memtable_ = std::make_shared<MemTable>(options_.memtable_size_limit);
        }
    }

    sequence_number_.store(max_seq + 1);

    wal_id_ = wal_ids.empty() ? 1 : wal_ids.back() + 1;
    return wal_->Create(wal_id_);
}
// Manifest (Simple implementation)
Status DBImpl::SaveManifest() const {
    std::string tmp_path = ManifestPath() + ".tmp";
    SequentialFileWriter writer;
    Status s = writer.Open(tmp_path);
    if (!s.ok()) return s;

    std::ostringstream oss;
    oss << next_sstable_id_ << "\n";
    
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    for (size_t level = 0; level < levels_.size(); ++level) {
        for (const auto& info : levels_[level]) {
            oss << info.meta.file_id << " "
                << info.meta.level << " "
                << info.meta.num_entries << " "
                << info.meta.smallest_key << " "
                << info.meta.largest_key << "\n";
        }
    }

    s = writer.Append(Slice(oss.str()));
    if (!s.ok()) return s;
    s = writer.Sync();
    if (!s.ok()) return s;
    s = writer.Close();
    if (!s.ok()) return s;

    return RenameFile(tmp_path, ManifestPath());
}

Status DBImpl::LoadManifest() {
    if (!FileExists(ManifestPath())) {
        return Status::NotFound("Manifest not found");
    }

    std::string content;
    SequentialFileReader reader;
    Status s = reader.Open(ManifestPath());
    if (!s.ok()) return s;

    // Read full file
    uint64_t size;
    s = FileSize(ManifestPath(), &size);
    if (!s.ok()) return s;
    s = reader.Read(size, &content);
    if (!s.ok()) return s;

    std::istringstream iss(content);
    iss >> next_sstable_id_;

    uint32_t file_id;
    int level;
    uint64_t num_entries;
    std::string smallest, largest;

    while (iss >> file_id >> level >> num_entries >> smallest >> largest) {
        SSTableMeta meta;
        meta.file_id = file_id;
        meta.level = level;
        meta.num_entries = num_entries;
        meta.smallest_key = smallest;
        meta.largest_key = largest;

        std::string path = SSTableFilePath(file_id);
        s = FileSize(path, &meta.file_size);
        if (!s.ok()) continue;

        SSTableReader* r = nullptr;
        s = SSTableReader::Open(path, options_, &r);
        if (s.ok()) {
            levels_[level].push_back({meta, std::shared_ptr<SSTableReader>(r)});
        }
    }

    return Status::OK();
}
// Put / Get / Delete / Write
Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(options, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
    std::lock_guard<std::mutex> lock(memtable_mutex_);

    for (const auto& entry : batch->Entries()) {
        uint64_t seq = sequence_number_.fetch_add(1);

        if (entry.type == WriteBatch::Entry::kPut) {
            VLogPointer vlog_ptr;
            Status s = vlog_->Append(Slice(entry.key), Slice(entry.value), ValueType::kValue, &vlog_ptr);
            if (!s.ok()) return s;

            WALRecord wal_rec{ValueType::kValue, seq, entry.key, vlog_ptr};
            s = wal_->AddRecord(wal_rec);
            if (!s.ok()) return s;

            memtable_->Put(Slice(entry.key), vlog_ptr, seq);
        } else {
            WALRecord wal_rec{ValueType::kDeletion, seq, entry.key, VLogPointer{0, 0, 0}};
            Status s = wal_->AddRecord(wal_rec);
            if (!s.ok()) return s;

            memtable_->Delete(Slice(entry.key), seq);
        }
    }

    if (options.sync) {
        wal_->Sync();
        vlog_->Sync();
    }

    return MaybeScheduleFlush();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    InternalEntry ie;
    bool found = false;

    // 1. Check active MemTable
    {
        std::lock_guard<std::mutex> lock(memtable_mutex_);
        if (memtable_->Get(key, &ie).ok()) {
            found = true;
        } else {
            // 2. Check immutable MemTables (newest first)
            for (auto it = immutable_memtables_.rbegin(); it != immutable_memtables_.rend(); ++it) {
                if ((*it)->Get(key, &ie).ok()) {
                    found = true;
                    break;
                }
            }
        }
    }

    // 3. Check SSTables
    if (!found) {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        
        // Level 0: check all
        for (auto it = levels_[0].rbegin(); it != levels_[0].rend(); ++it) {
            if (it->reader->Get(key, &ie).ok()) {
                found = true;
                break;
            }
        }

        // Level 1+: check one per level
        if (!found) {
            for (size_t lvl = 1; lvl < levels_.size(); ++lvl) {
                for (const auto& info : levels_[lvl]) {
                    if (key.ToString() >= info.meta.smallest_key && key.ToString() <= info.meta.largest_key) {
                        Status get_status = info.reader->Get(key, &ie);
                        if (get_status.ok()) {
                            found = true;
                            break;
                        }
                    }
                }
                if (found) break;
            }
        }
    }

    if (!found) {
        return Status::NotFound("Key not found");
    }
    if (ie.type == ValueType::kDeletion) {
        return Status::NotFound("Key deleted");
    }

    // Read from VLog
    return vlog_->Read(ie.vlog_ptr, nullptr, value);
}
// Background Flush
Status DBImpl::MaybeScheduleFlush() {
    if (memtable_->ShouldFlush()) {
        immutable_memtables_.push_back(memtable_);
        memtable_ = std::make_shared<MemTable>(options_.memtable_size_limit);
        
        // Create new WAL
        wal_id_++;
        Status s = wal_->Create(wal_id_);
        if (!s.ok()) return s;

        flush_cv_.notify_one();
    }
    return Status::OK();
}

void DBImpl::BackgroundFlush() {
    while (!shutting_down_) {
        std::shared_ptr<MemTable> to_flush;

        {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            flush_cv_.wait(lock, [this]() {
                return shutting_down_ || !immutable_memtables_.empty();
            });

            if (shutting_down_ && immutable_memtables_.empty()) break;

            {
                std::lock_guard<std::mutex> mlock(memtable_mutex_);
                if (!immutable_memtables_.empty()) {
                    to_flush = immutable_memtables_.front();
                }
            }
        }

        if (to_flush) {
            FlushMemTable(to_flush);
            
            {
                std::lock_guard<std::mutex> mlock(memtable_mutex_);
                immutable_memtables_.pop_front();
            }

            // Automatically check and trigger compaction for all levels
            for (int lvl = 0; lvl < static_cast<int>(options_.max_sstable_levels) - 1; ++lvl) {
                bool needs_compaction = false;
                {
                    std::lock_guard<std::mutex> lock(sstable_mutex_);
                    if (lvl == 0) {
                        needs_compaction = (levels_[0].size() >= 4);
                    } else {
                        uint64_t total_size = 0;
                        for (const auto& info : levels_[lvl]) {
                            total_size += info.meta.file_size;
                        }
                        uint64_t limit = static_cast<uint64_t>(10) * 1024 * 1024; // 10MB base for Level 1
                        for (int i = 1; i < lvl; ++i) {
                            limit *= 10;
                        }
                        needs_compaction = (total_size > limit);
                    }
                }
                if (needs_compaction) {
                    CompactLevel(lvl);
                }
            }
        }
    }
}

Status DBImpl::FlushMemTable(std::shared_ptr<MemTable> memtable) {
    if (memtable->Count() == 0) return Status::OK();

    uint32_t file_id;
    {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        file_id = next_sstable_id_++;
    }

    std::string path = SSTableFilePath(file_id);
    SSTableWriter writer(path, options_);
    Status s = writer.Open();
    if (!s.ok()) return s;

    auto entries = memtable->GetSortedEntries();
    for (const auto& pair : entries) {
        s = writer.Add(Slice(pair.first), pair.second);
        if (!s.ok()) return s;
    }

    s = writer.Finish();
    if (!s.ok()) return s;

    SSTableMeta meta;
    meta.file_id = file_id;
    meta.level = 0;
    meta.num_entries = writer.NumEntries();
    meta.file_size = writer.FileSize();
    meta.smallest_key = writer.SmallestKey();
    meta.largest_key = writer.LargestKey();

    SSTableReader* reader = nullptr;
    s = SSTableReader::Open(path, options_, &reader);
    if (!s.ok()) return s;

    {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        levels_[0].push_back({meta, std::shared_ptr<SSTableReader>(reader)});
    }

    return SaveManifest();
}

Status DBImpl::CompactLevel(int lvl) {
    if (lvl < 0 || lvl >= static_cast<int>(options_.max_sstable_levels) - 1) {
        return Status::InvalidArgument("Invalid level for compaction");
    }

    std::vector<SSTableInfo> inputs_lvl;
    std::vector<SSTableInfo> inputs_next;

    {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        if (levels_[lvl].empty()) {
            return Status::OK();
        }
        inputs_lvl = levels_[lvl];
    }

    // Determine key range of inputs_lvl
    std::string smallest_key = inputs_lvl[0].meta.smallest_key;
    std::string largest_key = inputs_lvl[0].meta.largest_key;
    for (size_t i = 1; i < inputs_lvl.size(); ++i) {
        if (inputs_lvl[i].meta.smallest_key < smallest_key) {
            smallest_key = inputs_lvl[i].meta.smallest_key;
        }
        if (inputs_lvl[i].meta.largest_key > largest_key) {
            largest_key = inputs_lvl[i].meta.largest_key;
        }
    }

    // Find overlapping files in lvl + 1
    {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        for (const auto& info : levels_[lvl + 1]) {
            if (info.meta.smallest_key <= largest_key && info.meta.largest_key >= smallest_key) {
                inputs_next.push_back(info);
            }
        }
    }

    // Perform multi-way merge
    std::vector<std::unique_ptr<SSTableReader::Iterator>> iters;
    for (auto& info : inputs_lvl) {
        auto iter = info.reader->NewIterator();
        Status s = iter->SeekToFirst();
        if (s.ok()) {
            iters.push_back(std::move(iter));
        } else if (!s.IsNotFound()) {
            return s;
        }
    }
    for (auto& info : inputs_next) {
        auto iter = info.reader->NewIterator();
        Status s = iter->SeekToFirst();
        if (s.ok()) {
            iters.push_back(std::move(iter));
        } else if (!s.IsNotFound()) {
            return s;
        }
    }

    std::vector<SSTableInfo> new_sstables;
    std::unique_ptr<SSTableWriter> current_writer;
    SSTableMeta new_meta;

    while (true) {
        std::string min_key;
        bool found_any = false;

        for (const auto& iter : iters) {
            if (iter->Valid()) {
                std::string k = iter->Key().ToString();
                if (!found_any || k < min_key) {
                    min_key = k;
                    found_any = true;
                }
            }
        }

        if (!found_any) {
            break;
        }

        // Find the latest version of this key among the iterators
        uint64_t max_seq = 0;
        bool has_max_seq = false;
        size_t winner_idx = 0;

        for (size_t i = 0; i < iters.size(); ++i) {
            if (iters[i]->Valid() && iters[i]->Key().ToString() == min_key) {
                uint64_t seq = iters[i]->Entry().sequence;
                if (!has_max_seq || seq > max_seq) {
                    max_seq = seq;
                    winner_idx = i;
                    has_max_seq = true;
                }
            }
        }

        const InternalEntry& winner_entry = iters[winner_idx]->Entry();

        // Check if we should drop this entry
        bool drop = false;
        if (winner_entry.type == ValueType::kDeletion) {
            bool exists_deeper = false;
            // Check levels starting from lvl + 2
            {
                std::lock_guard<std::mutex> lock(sstable_mutex_);
                for (size_t d = lvl + 2; d < options_.max_sstable_levels; ++d) {
                    for (const auto& info : levels_[d]) {
                        if (min_key >= info.meta.smallest_key && min_key <= info.meta.largest_key) {
                            InternalEntry temp;
                            if (info.reader->Get(Slice(min_key), &temp).ok()) {
                                exists_deeper = true;
                                break;
                            }
                        }
                    }
                    if (exists_deeper) break;
                }
            }
            if (!exists_deeper) {
                drop = true;
            }
        }

        if (!drop) {
            if (!current_writer) {
                uint32_t new_file_id;
                {
                    std::lock_guard<std::mutex> lock(sstable_mutex_);
                    new_file_id = next_sstable_id_++;
                }
                std::string path = SSTableFilePath(new_file_id);
                current_writer = std::make_unique<SSTableWriter>(path, options_);
                Status s = current_writer->Open();
                if (!s.ok()) return s;
                new_meta.file_id = new_file_id;
                new_meta.level = lvl + 1;
            }

            Status s = current_writer->Add(Slice(min_key), winner_entry);
            if (!s.ok()) return s;

            if (current_writer->FileSize() >= options_.memtable_size_limit) {
                s = current_writer->Finish();
                if (!s.ok()) return s;

                new_meta.num_entries = current_writer->NumEntries();
                new_meta.file_size = current_writer->FileSize();
                new_meta.smallest_key = current_writer->SmallestKey();
                new_meta.largest_key = current_writer->LargestKey();

                SSTableReader* reader = nullptr;
                s = SSTableReader::Open(SSTableFilePath(new_meta.file_id), options_, &reader);
                if (!s.ok()) return s;

                new_sstables.push_back({new_meta, std::shared_ptr<SSTableReader>(reader)});
                current_writer.reset();
            }
        }

        // Advance all iterators matching min_key
        for (auto& iter : iters) {
            if (iter->Valid() && iter->Key().ToString() == min_key) {
                iter->Next();
            }
        }
    }

    if (current_writer) {
        Status s = current_writer->Finish();
        if (!s.ok()) return s;

        new_meta.num_entries = current_writer->NumEntries();
        new_meta.file_size = current_writer->FileSize();
        new_meta.smallest_key = current_writer->SmallestKey();
        new_meta.largest_key = current_writer->LargestKey();

        SSTableReader* reader = nullptr;
        s = SSTableReader::Open(SSTableFilePath(new_meta.file_id), options_, &reader);
        if (!s.ok()) return s;

        new_sstables.push_back({new_meta, std::shared_ptr<SSTableReader>(reader)});
    }

    // Now update levels_
    {
        std::lock_guard<std::mutex> lock(sstable_mutex_);
        
        // Remove old level files
        for (const auto& input : inputs_lvl) {
            auto it = std::find_if(levels_[lvl].begin(), levels_[lvl].end(),
                [&](const SSTableInfo& info) { return info.meta.file_id == input.meta.file_id; });
            if (it != levels_[lvl].end()) {
                levels_[lvl].erase(it);
            }
        }

        // Remove old next level files
        for (const auto& input : inputs_next) {
            auto it = std::find_if(levels_[lvl + 1].begin(), levels_[lvl + 1].end(),
                [&](const SSTableInfo& info) { return info.meta.file_id == input.meta.file_id; });
            if (it != levels_[lvl + 1].end()) {
                levels_[lvl + 1].erase(it);
            }
        }

        // Add new SSTables
        for (const auto& new_sst : new_sstables) {
            levels_[lvl + 1].push_back(new_sst);
        }

        // Sort next level by smallest_key
        std::sort(levels_[lvl + 1].begin(), levels_[lvl + 1].end(),
            [](const SSTableInfo& a, const SSTableInfo& b) {
                return a.meta.smallest_key < b.meta.smallest_key;
            });
    }

    // Save manifest
    Status s = SaveManifest();
    if (!s.ok()) return s;

    // Delete physical files
    for (const auto& input : inputs_lvl) {
        DeleteFile(SSTableFilePath(input.meta.file_id));
    }
    for (const auto& input : inputs_next) {
        DeleteFile(SSTableFilePath(input.meta.file_id));
    }

    return Status::OK();
}

Status DBImpl::CompactRange() {
    // Synchronously run compaction for all levels
    for (int lvl = 0; lvl < static_cast<int>(options_.max_sstable_levels) - 1; ++lvl) {
        bool has_files = false;
        {
            std::lock_guard<std::mutex> lock(sstable_mutex_);
            has_files = !levels_[lvl].empty();
        }
        if (has_files) {
            Status s = CompactLevel(lvl);
            if (!s.ok()) return s;
        }
    }
    return Status::OK();
}

Status DBImpl::RunGC() {
    std::vector<uint32_t> vlog_ids;
    std::vector<std::string> children;
    Status s = GetChildren(db_path_, &children);
    if (!s.ok()) return s;

    for (const auto& name : children) {
        if (name.find("vlog_") == 0 && name.find(".dat") != std::string::npos && name.size() == 14) {
            uint32_t id = std::stoul(name.substr(5, 5));
            vlog_ids.push_back(id);
        }
    }
    std::sort(vlog_ids.begin(), vlog_ids.end());

    if (vlog_ids.empty()) {
        return Status::OK(); // Nothing to GC
    }

    uint32_t active_file_id = vlog_->CurrentFileId();
    // Only GC completed files (file_id < active_file_id)
    uint32_t gc_file_id = 0;
    bool found_gc_file = false;
    for (uint32_t id : vlog_ids) {
        if (id < active_file_id) {
            gc_file_id = id;
            found_gc_file = true;
            break; // take the oldest one
        }
    }

    if (!found_gc_file) {
        return Status::OK(); // No completed vlog files to GC
    }

    uint64_t total_bytes = 0;
    uint64_t live_bytes = 0;
    struct LiveRecord {
        std::string key;
        uint64_t offset;
        uint32_t val_size;
    };
    std::vector<LiveRecord> live_records;

    s = vlog_->NewReader(gc_file_id, 0, [&](const Slice& key, const Slice& value, ValueType type, uint64_t offset) {
        uint32_t rec_size = 4 + 1 + 4 + key.size() + 4 + value.size(); // CRC(4) + Type(1) + KLen(4) + Key + VLen(4) + Value
        total_bytes += rec_size;

        if (type == ValueType::kValue) {
            bool live = false;
            {
                std::lock_guard<std::mutex> mem_lock(memtable_mutex_);
                InternalEntry ie;
                bool found_latest = false;

                if (memtable_->Get(key, &ie).ok()) {
                    found_latest = true;
                } else {
                    for (auto it = immutable_memtables_.rbegin(); it != immutable_memtables_.rend(); ++it) {
                        if ((*it)->Get(key, &ie).ok()) {
                            found_latest = true;
                            break;
                        }
                    }
                }

                if (!found_latest) {
                    std::lock_guard<std::mutex> sst_lock(sstable_mutex_);
                    // Level 0
                    for (auto it = levels_[0].rbegin(); it != levels_[0].rend(); ++it) {
                        if (it->reader->Get(key, &ie).ok()) {
                            found_latest = true;
                            break;
                        }
                    }
                    // Level 1+
                    if (!found_latest) {
                        for (size_t lvl = 1; lvl < levels_.size(); ++lvl) {
                            for (const auto& info : levels_[lvl]) {
                                if (key.ToString() >= info.meta.smallest_key && key.ToString() <= info.meta.largest_key) {
                                    if (info.reader->Get(key, &ie).ok()) {
                                        found_latest = true;
                                        break;
                                    }
                                }
                            }
                            if (found_latest) break;
                        }
                    }
                }

                if (found_latest && ie.type == ValueType::kValue &&
                    ie.vlog_ptr.file_id == gc_file_id && ie.vlog_ptr.offset == offset) {
                    live = true;
                }
            }

            if (live) {
                live_bytes += rec_size;
                live_records.push_back({key.ToString(), offset, static_cast<uint32_t>(value.size())});
            }
        }
        return Status::OK();
    });

    if (!s.ok()) return s;
    if (total_bytes == 0) return Status::OK();

    double garbage_ratio = 1.0 - (double)live_bytes / total_bytes;
    if (garbage_ratio >= options_.vlog_gc_threshold) {
        // Rewrite live records
        for (const auto& rec : live_records) {
            std::string value;
            VLogPointer old_ptr{gc_file_id, rec.offset, rec.val_size};
            s = vlog_->Read(old_ptr, nullptr, &value);
            if (!s.ok()) continue;

            VLogPointer new_ptr;
            s = vlog_->Append(Slice(rec.key), Slice(value), ValueType::kValue, &new_ptr);
            if (!s.ok()) return s;

            // Perform CAS update
            {
                std::lock_guard<std::mutex> mem_lock(memtable_mutex_);
                bool still_live = false;
                InternalEntry ie;
                bool found_latest = false;

                if (memtable_->Get(Slice(rec.key), &ie).ok()) {
                    found_latest = true;
                } else {
                    for (auto it = immutable_memtables_.rbegin(); it != immutable_memtables_.rend(); ++it) {
                        if ((*it)->Get(Slice(rec.key), &ie).ok()) {
                            found_latest = true;
                            break;
                        }
                    }
                }

                if (!found_latest) {
                    std::lock_guard<std::mutex> sst_lock(sstable_mutex_);
                    // Level 0
                    for (auto it = levels_[0].rbegin(); it != levels_[0].rend(); ++it) {
                        if (it->reader->Get(Slice(rec.key), &ie).ok()) {
                            found_latest = true;
                            break;
                        }
                    }
                    // Level 1+
                    if (!found_latest) {
                        for (size_t lvl = 1; lvl < levels_.size(); ++lvl) {
                            for (const auto& info : levels_[lvl]) {
                                if (rec.key >= info.meta.smallest_key && rec.key <= info.meta.largest_key) {
                                    if (info.reader->Get(Slice(rec.key), &ie).ok()) {
                                        found_latest = true;
                                        break;
                                    }
                                }
                            }
                            if (found_latest) break;
                        }
                    }
                }

                if (found_latest && ie.type == ValueType::kValue &&
                    ie.vlog_ptr.file_id == gc_file_id && ie.vlog_ptr.offset == rec.offset) {
                    still_live = true;
                }

                if (still_live) {
                    uint64_t seq = sequence_number_.fetch_add(1);
                    WALRecord wal_rec{ValueType::kValue, seq, rec.key, new_ptr};
                    s = wal_->AddRecord(wal_rec);
                    if (s.ok()) {
                        memtable_->Put(Slice(rec.key), new_ptr, seq);
                    }
                }
            }
        }

        // Sync vlog and wal
        vlog_->Sync();
        wal_->Sync();

        // Remove reader and delete old vlog file
        vlog_->RemoveReader(gc_file_id);
        
        std::ostringstream oss;
        oss << db_path_ << "/vlog_" << std::setw(5) << std::setfill('0') << gc_file_id << ".dat";
        DeleteFile(oss.str());
    }

    return Status::OK();
}

std::string DBImpl::GetStats() const {
    std::ostringstream oss;
    oss << "Sequence Number: " << sequence_number_.load() << "\n";
    oss << "VLog File ID: " << vlog_->CurrentFileId() << ", Offset: " << vlog_->CurrentOffset() << "\n";
    oss << "Active MemTable Count: " << memtable_->Count() << "\n";
    oss << "Active MemTable Memory: " << memtable_->ApproximateMemoryUsage() << "\n";
    oss << "Active MemTable Limit: " << options_.memtable_size_limit << "\n";
    oss << "Immutable MemTables: " << immutable_memtables_.size() << "\n";
    
    std::lock_guard<std::mutex> lock(sstable_mutex_);
    for (size_t i = 0; i < levels_.size(); ++i) {
        oss << "Level " << i << " SSTables: " << levels_[i].size() << "\n";
    }
    return oss.str();
}

} // namespace splitkv


