#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "splitkv/slice.h"
#include "splitkv/status.h"

namespace splitkv {

// ===========================================================================
// SequentialFileWriter — buffered, append-only file writer.
// ===========================================================================

class SequentialFileWriter {
public:
    SequentialFileWriter() : file_(nullptr), offset_(0) {}

    // Open |path| for writing (creates or truncates, unless append is true).
    static Status Open(const std::string& path, SequentialFileWriter* writer, bool append = false);

    // Member Open method
    Status Open(const std::string& path, bool append = false);

    // Append raw bytes to the file.
    Status Append(const Slice& data);

    // Flush user-space buffers to the OS.
    Status Flush();

    // Flush + durable sync to storage media.
    Status Sync();

    // Close the underlying file.  Safe to call more than once.
    Status Close();

    // Current byte offset within the file.
    size_t Offset() const { return offset_; }

    ~SequentialFileWriter() { Close(); }

    // Non-copyable.
    SequentialFileWriter(const SequentialFileWriter&) = delete;
    SequentialFileWriter& operator=(const SequentialFileWriter&) = delete;

    // Movable.
    SequentialFileWriter(SequentialFileWriter&& other) noexcept;
    SequentialFileWriter& operator=(SequentialFileWriter&& other) noexcept;

private:
    FILE*  file_;
    size_t offset_;
};

// ===========================================================================
// RandomAccessFileReader — positioned reads into an open file.
// ===========================================================================

class RandomAccessFileReader {
public:
    RandomAccessFileReader() : file_(nullptr) {}

    // Open |path| for random reads.
    static Status Open(const std::string& path, RandomAccessFileReader* reader);

    // Member Open method
    Status Open(const std::string& path);

    // Read |n| bytes starting at |offset| into |*result|.
    Status Read(uint64_t offset, size_t n, std::string* result);

    Status Close();

    ~RandomAccessFileReader() { Close(); }

    RandomAccessFileReader(const RandomAccessFileReader&) = delete;
    RandomAccessFileReader& operator=(const RandomAccessFileReader&) = delete;

    RandomAccessFileReader(RandomAccessFileReader&& other) noexcept;
    RandomAccessFileReader& operator=(RandomAccessFileReader&& other) noexcept;

private:
    FILE* file_;
};

// ===========================================================================
// SequentialFileReader — forward-only reads.
// ===========================================================================

class SequentialFileReader {
public:
    SequentialFileReader() : file_(nullptr) {}

    // Open |path| for sequential reading.
    static Status Open(const std::string& path, SequentialFileReader* reader);

    // Member Open method
    Status Open(const std::string& path);

    // Read up to |n| bytes into |*result|.  May return fewer bytes at EOF.
    Status Read(size_t n, std::string* result);

    // Skip forward |n| bytes.
    Status Skip(size_t n);

    Status Close();

    ~SequentialFileReader() { Close(); }

    SequentialFileReader(const SequentialFileReader&) = delete;
    SequentialFileReader& operator=(const SequentialFileReader&) = delete;

    SequentialFileReader(SequentialFileReader&& other) noexcept;
    SequentialFileReader& operator=(SequentialFileReader&& other) noexcept;

private:
    FILE* file_;
};

// ===========================================================================
// Free-standing filesystem utilities.
// ===========================================================================

// Returns true if a file or directory exists at |path|.
bool FileExists(const std::string& path);

// Store the size of the file at |path| in |*size|.
Status FileSize(const std::string& path, uint64_t* size);

// Delete the file at |path|.
Status DeleteFile(const std::string& path);

// Create a directory at |path|.  It is not an error if the directory
// already exists.
Status CreateDir(const std::string& path);

// Atomically rename |src| to |dst| (platform-level rename).
Status RenameFile(const std::string& src, const std::string& dst);

// Populate |*result| with the names of the children of |dir| (not full
// paths — just the basenames).
Status GetChildren(const std::string& dir, std::vector<std::string>* result);

} // namespace splitkv
