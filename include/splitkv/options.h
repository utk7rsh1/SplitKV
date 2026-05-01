#pragma once

#include <cstddef>

namespace splitkv {

// Options controls the behavior of the database engine.  Sensible defaults
// are provided so callers can open a DB with just `Options{}`.
struct Options {
    // Maximum size (in bytes) a single MemTable may grow to before it is
    // flushed to an immutable MemTable and eventually written to an SSTable.
    size_t memtable_size_limit = 4 * 1024 * 1024;  // 4 MB

    // Number of bits per key for the Bloom filter in each SSTable block.
    // Higher values reduce false-positive rates at the cost of memory.
    size_t bloom_bits_per_key = 10;

    // Maximum number of levels in the LSM tree.
    size_t max_sstable_levels = 7;

    // Fraction of dead/garbage bytes in a VLog segment that triggers GC.
    // E.g., 0.5 means GC fires when >= 50 % of the segment is garbage.
    double vlog_gc_threshold = 0.5;

    // Number of background threads available for compaction work.
    size_t compaction_threads = 2;

    // Values whose serialized size is at or below this threshold are inlined
    // directly in the LSM-tree index rather than written to the VLog.
    size_t value_inline_threshold = 256;

    // When true, every write is fsync'd before returning to the caller.
    bool sync_writes = false;

    // Target maximum size (in bytes) of a single VLog file before rotation.
    size_t vlog_file_size_limit = 256 * 1024 * 1024;  // 256 MB

    // Target uncompressed size of a data block inside an SSTable.
    size_t block_size = 4096;

    // Sparse-index sampling interval: one index entry is emitted for every
    // N-th key written into an SSTable data block.
    size_t sparse_index_interval = 16;

    // Maximum number of immutable MemTables allowed before writes are
    // stalled to let background flushes catch up.
    size_t max_memtable_count = 2;
};

// ReadOptions controls the behavior of individual read operations.
struct ReadOptions {
    // When true, checksums stored alongside data are verified on every read.
    bool verify_checksums = false;
};

// WriteOptions controls the behavior of individual write operations.
struct WriteOptions {
    // When true, the write is fsync'd before the call returns.
    bool sync = false;
};

} // namespace splitkv
