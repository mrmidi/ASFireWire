// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <memory>
#include <new>
#include <cstdlib>
#include <cstdio>
// Full evidence storage or a fixed eight-record tail. Allocate before DMA Start.
// Sequential writes reset reused slots; retrieval never creates an observation.
template<class T,size_t Full> class DiagnosticRecords {
    static_assert(Full>0);
    std::unique_ptr<std::array<T,Full>> full_;
    std::array<T,8> tail_{};
    bool retain_;
    size_t next_=0;
public:
    explicit DiagnosticRecords(bool retain):retain_(retain) {
        if(retain)full_.reset(new(std::nothrow) std::array<T,Full>{});
    }
    bool valid() const {return !retain_ || bool(full_);}
    size_t first() const {return retain_ || next_<=8?0:next_-8;}
    size_t end() const {return !valid()?0:retain_?Full:next_;}
    size_t size() const {return Full;} // The probe's finite observation budget.
    T &operator[](size_t n) {
        if(!valid() || n!=next_ || n>=Full)std::abort(); // Programming error, not a stream fault.
        ++next_;
        if(retain_)return (*full_)[n];
        auto &slot=tail_[n%8];slot=T{};return slot;
    }
    const T &read(size_t n) const {
        if(!valid() || n<first() || n>=end())std::abort();
        return retain_?(*full_)[n]:tail_[n%8];
    }
    void emit(FILE *log,const char *name) const {
        std::fprintf(log,"diagnostic-records kind=%s full=%u observed=%zu retained=%zu capacity=%zu bytes=%zu\n",
            name,unsigned(retain_),next_,retain_?next_:next_<8?next_:8,
            retain_?Full:8,(retain_?Full:8)*sizeof(T));
    }
};
