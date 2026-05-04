#include "file_util.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#   include <direct.h>    // _mkdir
#   include <io.h>        // _commit, _access
#   include <windows.h>   // FindFirstFileA, etc.
#   undef DeleteFile
#   include <share.h>     // _SH_DENYNO
#else
#   include <dirent.h>
#   include <unistd.h>    // fsync, access
#endif

namespace splitkv {

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

// Map errno to a suitable Status.
Status IOErrorFromErrno(const std::string& context) {
    return Status::IOError(context + ": " + std::strerror(errno));
}

// Platform-agnostic fdatasync / _commit.
Status SyncFd(FILE* file) {
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        return IOErrorFromErrno("_commit");
    }
#else
    if (fsync(fileno(file)) != 0) {
        return IOErrorFromErrno("fsync");
    }
#endif
    return Status::OK();
}

} // anonymous namespace

// ===========================================================================
// SequentialFileWriter
// ===========================================================================

Status SequentialFileWriter::Open(const std::string& path,
                                  SequentialFileWriter* writer, bool append) {
    FILE* f;
#ifdef _WIN32
    f = _fsopen(path.c_str(), append ? "ab" : "wb", _SH_DENYNO);
    if (f == nullptr) {
        return Status::IOError("Cannot open file for writing: " + path);
    }
#else
    f = std::fopen(path.c_str(), append ? "ab" : "wb");
    if (f == nullptr) {
        return IOErrorFromErrno("Cannot open file for writing: " + path);
    }
#endif
    writer->file_   = f;
    
    if (append) {
        // Find current size
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END);
        writer->offset_ = _ftelli64(f);
#else
        fseeko(f, 0, SEEK_END);
        writer->offset_ = ftello(f);
#endif
    } else {
        writer->offset_ = 0;
    }
    
    return Status::OK();
}

Status SequentialFileWriter::Open(const std::string& path, bool append) {
    return Open(path, this, append);
}

Status SequentialFileWriter::Append(const Slice& data) {
    if (file_ == nullptr) {
        return Status::IOError("File not open");
    }
    size_t written = std::fwrite(data.data(), 1, data.size(), file_);
    if (written != data.size()) {
        return IOErrorFromErrno("fwrite");
    }
    offset_ += written;
    return Status::OK();
}

Status SequentialFileWriter::Flush() {
    if (file_ == nullptr) return Status::OK();
    if (std::fflush(file_) != 0) {
        return IOErrorFromErrno("fflush");
    }
    return Status::OK();
}

Status SequentialFileWriter::Sync() {
    Status s = Flush();
    if (!s.ok()) return s;
    return SyncFd(file_);
}

Status SequentialFileWriter::Close() {
    if (file_ == nullptr) return Status::OK();
    int rc = std::fclose(file_);
    file_ = nullptr;
    if (rc != 0) {
        return IOErrorFromErrno("fclose");
    }
    return Status::OK();
}

SequentialFileWriter::SequentialFileWriter(SequentialFileWriter&& other) noexcept
    : file_(other.file_), offset_(other.offset_) {
    other.file_   = nullptr;
    other.offset_ = 0;
}

SequentialFileWriter& SequentialFileWriter::operator=(
        SequentialFileWriter&& other) noexcept {
    if (this != &other) {
        Close();  // Release any existing handle.
        file_   = other.file_;
        offset_ = other.offset_;
        other.file_   = nullptr;
        other.offset_ = 0;
    }
    return *this;
}

// ===========================================================================
// RandomAccessFileReader
// ===========================================================================

Status RandomAccessFileReader::Open(const std::string& path,
                                    RandomAccessFileReader* reader) {
    FILE* f;
#ifdef _WIN32
    f = _fsopen(path.c_str(), "rb", _SH_DENYNO);
    if (f == nullptr) {
        return Status::IOError("Cannot open file for reading: " + path);
    }
#else
    f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return IOErrorFromErrno("Cannot open file for reading: " + path);
    }
#endif
    reader->file_ = f;
    return Status::OK();
}

Status RandomAccessFileReader::Open(const std::string& path) {
    return Open(path, this);
}

Status RandomAccessFileReader::Read(uint64_t offset, size_t n,
                                    std::string* result) {
    if (file_ == nullptr) {
        return Status::IOError("File not open");
    }
    result->resize(n);

    // Seek to the requested position.
#ifdef _WIN32
    if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
        return IOErrorFromErrno("_fseeki64");
    }
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
        return IOErrorFromErrno("fseeko");
    }
#endif

    size_t nread = std::fread(&(*result)[0], 1, n, file_);
    if (nread < n) {
        if (std::feof(file_)) {
            result->resize(nread);
            // Partial read at EOF — not an error, just fewer bytes.
        } else {
            return IOErrorFromErrno("fread");
        }
    }
    return Status::OK();
}

Status RandomAccessFileReader::Close() {
    if (file_ == nullptr) return Status::OK();
    int rc = std::fclose(file_);
    file_ = nullptr;
    if (rc != 0) {
        return IOErrorFromErrno("fclose");
    }
    return Status::OK();
}

RandomAccessFileReader::RandomAccessFileReader(
        RandomAccessFileReader&& other) noexcept
    : file_(other.file_) {
    other.file_ = nullptr;
}

RandomAccessFileReader& RandomAccessFileReader::operator=(
        RandomAccessFileReader&& other) noexcept {
    if (this != &other) {
        Close();
        file_ = other.file_;
        other.file_ = nullptr;
    }
    return *this;
}

// ===========================================================================
// SequentialFileReader
// ===========================================================================

Status SequentialFileReader::Open(const std::string& path,
                                  SequentialFileReader* reader) {
    FILE* f;
#ifdef _WIN32
    f = _fsopen(path.c_str(), "rb", _SH_DENYNO);
    if (f == nullptr) {
        return Status::IOError("Cannot open file for reading: " + path);
    }
#else
    f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return IOErrorFromErrno("Cannot open file for reading: " + path);
    }
#endif
    reader->file_ = f;
    return Status::OK();
}

Status SequentialFileReader::Open(const std::string& path) {
    return Open(path, this);
}

Status SequentialFileReader::Read(size_t n, std::string* result) {
    if (file_ == nullptr) {
        return Status::IOError("File not open");
    }
    result->resize(n);
    size_t nread = std::fread(&(*result)[0], 1, n, file_);
    result->resize(nread);
    if (nread < n && std::ferror(file_)) {
        return IOErrorFromErrno("fread");
    }
    return Status::OK();
}

Status SequentialFileReader::Skip(size_t n) {
    if (file_ == nullptr) {
        return Status::IOError("File not open");
    }
#ifdef _WIN32
    if (_fseeki64(file_, static_cast<__int64>(n), SEEK_CUR) != 0) {
        return IOErrorFromErrno("_fseeki64");
    }
#else
    if (fseeko(file_, static_cast<off_t>(n), SEEK_CUR) != 0) {
        return IOErrorFromErrno("fseeko");
    }
#endif
    return Status::OK();
}

Status SequentialFileReader::Close() {
    if (file_ == nullptr) return Status::OK();
    int rc = std::fclose(file_);
    file_ = nullptr;
    if (rc != 0) {
        return IOErrorFromErrno("fclose");
    }
    return Status::OK();
}

SequentialFileReader::SequentialFileReader(
        SequentialFileReader&& other) noexcept
    : file_(other.file_) {
    other.file_ = nullptr;
}

SequentialFileReader& SequentialFileReader::operator=(
        SequentialFileReader&& other) noexcept {
    if (this != &other) {
        Close();
        file_ = other.file_;
        other.file_ = nullptr;
    }
    return *this;
}

// ===========================================================================
// Free-standing filesystem utilities
// ===========================================================================

bool FileExists(const std::string& path) {
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

Status FileSize(const std::string& path, uint64_t* size) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) {
        return IOErrorFromErrno("stat: " + path);
    }
    *size = static_cast<uint64_t>(st.st_size);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return IOErrorFromErrno("stat: " + path);
    }
    *size = static_cast<uint64_t>(st.st_size);
#endif
    return Status::OK();
}

Status DeleteFile(const std::string& path) {
    if (std::remove(path.c_str()) != 0) {
        return IOErrorFromErrno("remove: " + path);
    }
    return Status::OK();
}

Status CreateDir(const std::string& path) {
#ifdef _WIN32
    int rc = _mkdir(path.c_str());
#else
    int rc = mkdir(path.c_str(), 0755);
#endif
    if (rc != 0 && errno != EEXIST) {
        return IOErrorFromErrno("mkdir: " + path);
    }
    return Status::OK();
}

Status RenameFile(const std::string& src, const std::string& dst) {
#ifdef _WIN32
    // On Windows, std::rename fails if the destination exists.  Use
    // MoveFileExA with MOVEFILE_REPLACE_EXISTING for atomic semantics.
    if (!MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return Status::IOError("MoveFileExA: " + src + " -> " + dst +
                               " (error " + std::to_string(GetLastError()) + ")");
    }
#else
    if (std::rename(src.c_str(), dst.c_str()) != 0) {
        return IOErrorFromErrno("rename: " + src + " -> " + dst);
    }
#endif
    return Status::OK();
}

Status GetChildren(const std::string& dir,
                   std::vector<std::string>* result) {
    result->clear();

#ifdef _WIN32
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA find_data;
    HANDLE handle = FindFirstFileA(pattern.c_str(), &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            return Status::OK();  // Empty directory.
        }
        return Status::IOError("FindFirstFileA: " + dir +
                               " (error " + std::to_string(err) + ")");
    }
    do {
        const char* name = find_data.cFileName;
        // Skip the "." and ".." pseudo-entries.
        if (std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0) {
            result->emplace_back(name);
        }
    } while (FindNextFileA(handle, &find_data));
    FindClose(handle);
#else
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        return IOErrorFromErrno("opendir: " + dir);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0) {
            result->emplace_back(name);
        }
    }
    closedir(d);
#endif

    return Status::OK();
}

} // namespace splitkv
