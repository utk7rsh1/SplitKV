#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

namespace splitkv {

// Slice is a lightweight, non-owning reference to a contiguous byte range.
// Similar in spirit to std::string_view but tuned for storage-engine use.
class Slice {
public:
    // Constructors

    // Empty slice.
    Slice() : data_(""), size_(0) {}

    // Explicit pointer + length.
    Slice(const char* data, size_t size) : data_(data), size_(size) {}

    // Implicit from std::string (keeps the reference valid only while the
    // string is alive and unmodified).
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

    // Implicit from NUL-terminated C string.
    Slice(const char* s) : data_(s), size_(std::strlen(s)) {}
    // Accessors

    const char* data() const { return data_; }
    size_t      size() const { return size_; }
    bool       empty() const { return size_ == 0; }

    // Index into the slice. No bounds check in release builds.
    char operator[](size_t n) const {
        assert(n < size_);
        return data_[n];
    }
    // Conversions

    // Materialize a std::string copy.
    std::string ToString() const { return std::string(data_, size_); }
    // Comparisons

    // Three-way compare: < 0, 0, or > 0 analogous to memcmp / strcmp.
    int compare(const Slice& other) const {
        const size_t min_len = (size_ < other.size_) ? size_ : other.size_;
        int r = std::memcmp(data_, other.data_, min_len);
        if (r == 0) {
            if (size_ < other.size_)      r = -1;
            else if (size_ > other.size_) r = +1;
        }
        return r;
    }

    // Returns true if the slice begins with the given prefix.
    bool starts_with(const Slice& prefix) const {
        return (size_ >= prefix.size_) &&
               (std::memcmp(data_, prefix.data_, prefix.size_) == 0);
    }
    // Mutation helpers (adjust the window, not the underlying data)

    // Drop the first n bytes from the front of the view.
    void remove_prefix(size_t n) {
        assert(n <= size_);
        data_ += n;
        size_ -= n;
    }
    // Relational operators

    friend bool operator==(const Slice& a, const Slice& b) {
        return a.compare(b) == 0;
    }
    friend bool operator!=(const Slice& a, const Slice& b) {
        return a.compare(b) != 0;
    }
    friend bool operator<(const Slice& a, const Slice& b) {
        return a.compare(b) < 0;
    }
    friend bool operator<=(const Slice& a, const Slice& b) {
        return a.compare(b) <= 0;
    }
    friend bool operator>(const Slice& a, const Slice& b) {
        return a.compare(b) > 0;
    }
    friend bool operator>=(const Slice& a, const Slice& b) {
        return a.compare(b) >= 0;
    }

private:
    const char* data_;
    size_t      size_;
};

} // namespace splitkv
