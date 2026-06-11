# SplitKV

A persistent key-value storage engine written in C++17. Built from scratch as a learning project to understand how LSM-tree based storage engines work internally.

The architecture is inspired by the [WiscKey paper](https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf), which proposes separating keys from values to reduce write amplification during compaction.

---

## How it works

**Write path:**

```
Put(key, value)
  -> append value to VLog (returns {file_id, offset} pointer)
  -> write key + pointer to WAL (crash safety)
  -> insert key + pointer into MemTable (in-memory sorted AVL tree)

When MemTable is full:
  -> MemTable becomes immutable
  -> background thread flushes it to a new SSTable file on disk
```

**Read path:**

```
Get(key)
  -> check active MemTable
  -> check immutable MemTables (not yet flushed)
  -> search SSTable levels, newest to oldest
       (Bloom filter skips files that definitely don't have the key)
  -> use VLog pointer to read the actual value
```

**Background:**
- **Compaction** merges SSTables across levels, drops deleted keys and old versions
- **Garbage Collection** scans VLog segments, rewrites live values, deletes the old segment

---

## Architecture

```
                   +------------------+
  Put/Get/Delete   |     DBImpl       |
 ----------------> |                  |
                   | MemTable (AVL)   |
                   | Immutable MTables|
                   | SSTable Levels   |
                   | VLog             |
                   | WAL              |
                   +------------------+
                          |
              +-----------+-----------+
              |                       |
         SSTable files            VLog files
     (keys + pointers only)    (raw values only)
```

---

## Building

Requires CMake 3.16+ and a C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+).

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

---

## Usage

### CLI tool

```bash
./build/tools/splitkv-cli path/to/database
```

Commands:
```
put mykey myvalue    # store a key-value pair
get mykey            # retrieve a value
delete mykey         # mark a key as deleted
stats                # show basic stats
compact              # trigger compaction manually
gc                   # trigger garbage collection manually
exit
```

### TCP server

```bash
./build/tools/splitkv-server path/to/database 7777
```

Connect with telnet or netcat:
```
PING          ->  +PONG
PUT foo bar   ->  +OK
GET foo       ->  $3<CR><LF>bar
DEL foo       ->  +OK
STATS         ->  (stats as text)
```

---

## What I learned

- How AVL trees work. The rotation cases (LL, RR, LR, RL) took me a while to get right.
- Why you need to write to the WAL before updating the MemTable, so you can replay operations after a crash.
- How Bloom filters work and why a 1% false positive rate is fine here.
- What a sparse index is and why it is enough to find keys efficiently in a sorted file.
- How key-value separation reduces write amplification. Compaction only touches keys, not values.
- How `std::shared_mutex` works and when to use shared vs exclusive locks.
- Some Windows/Linux file I/O differences. The `DeleteFile` macro conflict was annoying to debug.

---

## Things I would improve

- The manifest format is very basic. A proper version would use atomic renames.
- Bloom filters are rebuilt from scratch on startup instead of being persisted to disk.
- GC only processes one VLog segment per run.
- Error messages could be more informative in a lot of places.
- No real benchmarks yet comparing write amplification to a naive approach.

---

## References

- WiscKey: https://www.usenix.org/system/files/conference/fast16/fast16-papers-lu.pdf
- LevelDB implementation notes: https://github.com/google/leveldb/blob/main/doc/impl.md
