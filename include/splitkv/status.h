#pragma once

#include <string>
#include <utility>

namespace splitkv {

// Status encapsulates the result of an operation. It may indicate success,
// or describe an error with a human-readable message. Modeled after
// LevelDB/RocksDB Status for familiar ergonomics.
class Status {
public:
    // Enumeration of all supported error categories.
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kIOError = 3,
        kInvalidArgument = 4,
        kNotSupported = 5,
    };

    // Default-constructed Status is OK.
    Status() : code_(kOk) {}

    // Copy / move semantics.
    Status(const Status&) = default;
    Status& operator=(const Status&) = default;
    Status(Status&&) noexcept = default;
    Status& operator=(Status&&) noexcept = default;
    // Factory helpers — the idiomatic way to create Status values.
    static Status OK() { return Status(); }

    static Status NotFound(const std::string& msg) {
        return Status(kNotFound, msg);
    }

    static Status Corruption(const std::string& msg) {
        return Status(kCorruption, msg);
    }

    static Status IOError(const std::string& msg) {
        return Status(kIOError, msg);
    }

    static Status InvalidArgument(const std::string& msg) {
        return Status(kInvalidArgument, msg);
    }

    static Status NotSupported(const std::string& msg) {
        return Status(kNotSupported, msg);
    }
    // Observers

    // Returns true when the operation succeeded.
    bool ok() const { return code_ == kOk; }

    bool IsNotFound()   const { return code_ == kNotFound; }
    bool IsCorruption() const { return code_ == kCorruption; }
    bool IsIOError()    const { return code_ == kIOError; }

    Code code() const { return code_; }

    // Human-readable representation, e.g. "IOError: disk full".
    std::string ToString() const {
        if (code_ == kOk) {
            return "OK";
        }

        std::string result;
        switch (code_) {
            case kNotFound:         result = "NotFound: ";         break;
            case kCorruption:       result = "Corruption: ";       break;
            case kIOError:          result = "IOError: ";          break;
            case kInvalidArgument:  result = "InvalidArgument: ";  break;
            case kNotSupported:     result = "NotSupported: ";     break;
            default:                result = "Unknown: ";          break;
        }
        result.append(msg_);
        return result;
    }

private:
    Status(Code code, std::string msg)
        : code_(code), msg_(std::move(msg)) {}

    Code        code_;
    std::string msg_;
};

} // namespace splitkv
