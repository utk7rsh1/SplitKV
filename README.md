# SplitKV

A persistent key-value storage engine written in C++17.

I built this to understand how database storage engines work internally. The design is based on the WiscKey paper, which stores keys and values separately to reduce write amplification during compaction.

## How it works

**Write path:**
When you call `Put(key, value)`:
1. The value is appended to a Value Log file (VLog), giving back a `{file_id, offset}` pointer
2. The key and pointer are written to a Write-Ahead Log (WAL) for crash safety
3. The key and pointer go into an in-memory sorted index called the MemTable (AVL tree)
4. When the MemTable hits its size limit, it gets flushed to a sorted file on disk (SSTable)

**Read path:**
When you call `Get(key)`:
1. Check the active MemTable
2. Check older in-memory MemTables not yet flushed
3. Search SSTable files newest to oldest, using a Bloom filter to skip files that definitely do not have the key
4. Once found, use the pointer to read the actual value from VLog

**Background:**
- Compaction merges SSTables, removes deleted keys and old versions
- Garbage collection scans old VLog segments and rewrites only live values

## Components

- `avl_tree.h` - AVL tree backing the MemTable
- `bloom_filter` - per-SSTable filter for fast negative lookups  
- `vlog` - append-only value storage
- `wal` - write-ahead log for crash recovery
- `sstable` - sorted, immutable key index files
- `db_impl` - orchestrates everything

## Building

```bash
mkdir build && cd build
cmake .. && cmake --build .
```

## Tools

```bash
./build/tools/splitkv-cli path/to/db        # interactive CLI
./build/tools/splitkv-server path/to/db 7777  # TCP server
```
