// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketBuffer.hpp"
#include <algorithm>
#include <atomic>
#include <limits>

namespace NativePackets {
enum class JournalError { none, generation, sequence, capacity, buffer, consume, changed };
// Single event-loop owner. Copies complete transport records, never parses CIP
// or audio. This protects consumers from later source reuse; it does NOT make
// copying from memory concurrently overwritten by DMA safe. The producer must
// first establish a stable completed group, retained for the whole copy.
template<size_t Groups=32> class ReceiveJournal {
    static_assert(Groups>0);
    struct Record {std::array<uint8_t,448> bytes{};uint32_t status=0,timestamp=0;};
    std::array<std::array<Record,64>,Groups> groups_{};
    const uint32_t generation_;
    uint64_t first_=0,next_=0;
    size_t head_=0,count_=0,highWater_=0;
    JournalError error_=JournalError::none;
    JournalError fail(JournalError error) {error_=error;return error;}
public:
    explicit ReceiveJournal(uint32_t generation):generation_(generation) {}
    ReceiveJournal(const ReceiveJournal &)=delete;
    ReceiveJournal &operator=(const ReceiveJournal &)=delete;
    JournalError error() const {return error_;}
    uint64_t first() const {return first_;}
    uint64_t next() const {return next_;}
    size_t queuedGroups() const {return count_;}
    size_t highWaterGroups() const {return highWater_;}
    template<class Source> JournalError append(uint64_t first,uint32_t generation,Source source) {
        if(error_!=JournalError::none)return error_;
        if(generation!=generation_)return fail(JournalError::generation);
        if(first!=next_ || next_>std::numeric_limits<uint64_t>::max()-64)
            return fail(JournalError::sequence);
        if(count_==Groups)return fail(JournalError::capacity);
        auto &group=groups_[(head_+count_)%Groups];
        for(unsigned i=0;i<64;++i) {
            const auto sourceRecord=source(first+i);
            if(sourceRecord.bytes.size()!=448)return fail(JournalError::buffer);
            std::copy(sourceRecord.bytes.begin(),sourceRecord.bytes.end(),group[i].bytes.begin());
            group[i].status=sourceRecord.status;group[i].timestamp=sourceRecord.timestamp;
        }
        // Re-read the source after copying the whole group, before publishing.
        // This detects observed changes, not all possible concurrent writes:
        // an ABA replacement or a write after its comparison can escape it.
        // Source must be repeatable and still have an independently safe lease.
        // Compiler barrier only; this does not synchronize or quiesce DMA.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        for(unsigned i=0;i<64;++i) {
            const auto sourceRecord=source(first+i);
            if(sourceRecord.bytes.size()!=448)return fail(JournalError::buffer);
            if(sourceRecord.status!=group[i].status || sourceRecord.timestamp!=group[i].timestamp ||
                !std::equal(sourceRecord.bytes.begin(),sourceRecord.bytes.end(),group[i].bytes.begin()))
                return fail(JournalError::changed);
        }
        // Publish only once every record has been copied and compared. Failure hides all
        // views and requires a new session, rather than exposing a partial group.
        ++count_;next_+=64;highWater_=std::max(highWater_,count_);
        return JournalError::none;
    }
    // Borrowed journal storage: invalid after consume(), fault, or destruction.
    CompletedBuffer view(uint64_t sequence) const {
        if(error_!=JournalError::none || sequence<first_ || sequence>=next_)return {};
        const auto offset=sequence-first_;
        const auto &record=groups_[(head_+size_t(offset/64))%Groups][size_t(offset%64)];
        return {record.bytes,record.status,record.timestamp};
    }
    JournalError consume(uint64_t first) {
        if(error_!=JournalError::none)return error_;
        if(!count_ || first!=first_)return fail(JournalError::consume);
        head_=(head_+1)%Groups;--count_;first_+=64;
        return JournalError::none;
    }
};
}
