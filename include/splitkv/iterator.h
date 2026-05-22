#pragma once
#include <string>
#include "splitkv/slice.h"
#include "splitkv/status.h"

namespace splitkv {

class DBIterator {
public:
    virtual ~DBIterator() = default;
    
    virtual bool Valid() const = 0;
    virtual void SeekToFirst() = 0;
    virtual void SeekToLast() = 0;
    virtual void Seek(const Slice& target) = 0;
    virtual void Next() = 0;
    virtual void Prev() = 0;
    virtual Slice Key() const = 0;
    virtual std::string Value() const = 0;  // reads from VLog
    virtual Status status() const = 0;
};

} // namespace splitkv
