// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeReceiveJournal.hpp"
namespace NativePackets {
// One event-loop owner. Functional receive storage independent of test duration.
// The producer retains a stable completed DMA group until append returns.
class SourceQueue {
    ReceiveJournal<16> journal_; // 1024 packets, including startup lead/slack.
    uint32_t generation_;
    uint64_t copied_=0;
    uint64_t observed_=0,maximumObserverLead_=0;
    bool observing_=false;
    bool fault_=false;
    bool fail() {fault_=true;return false;}
    bool copyRecord(uint64_t n,PreparedBuffer &out,uint32_t &status,uint32_t &stamp) {
        const auto record=journal_.view(n);
        if(record.bytes.size()!=448)return fail();
        std::copy(record.bytes.begin(),record.bytes.end(),out.bytes.begin());
        out.length=448;status=record.status;stamp=record.timestamp;
        return true;
    }
public:
    explicit SourceQueue(uint32_t generation):journal_(generation),generation_(generation) {}
    bool fault() const {return fault_ || journal_.error()!=JournalError::none;}
    uint64_t first() const {return journal_.first();}
    uint64_t next() const {return journal_.next();}
    uint64_t copied() const {return copied_;}
    uint64_t observed() const {return observed_;}
    uint64_t maximumObserverLead() const {return maximumObserverLead_;}
    bool observing() const {return observing_;}
    bool observerValid() const {
        return !fault() && (!observing_ || (first()<=observed_ && observed_<=next()));
    }
    size_t queued() const {return journal_.queuedGroups();}
    size_t highWater() const {return journal_.highWaterGroups();}
    // Optional second, sequential reader, selected before any native start.
    // Both readers share this event-loop owner; neither lends journal pointers.
    bool enableObserver() {
        if(fault() || observing_ || next() || copied_)return fail();
        observing_=true;return true;
    }
    bool observe(uint64_t n,uint32_t generation,PreparedBuffer &out,uint32_t &status,uint32_t &stamp) {
        if(fault())return false;
        if(!observing_ || generation!=generation_ || n!=observed_)return fail();
        if(!copyRecord(n,out,status,stamp))return false;
        ++observed_;
        if(observed_>copied_)maximumObserverLead_=std::max(maximumObserverLead_,observed_-copied_);
        return true;
    }
    template<class Source> bool append(uint64_t first,uint32_t generation,Source source) {
        if(fault())return false;
        return journal_.append(first,generation,source)==JournalError::none;
    }
    bool copy(uint64_t n,uint32_t generation,PreparedBuffer &out,uint32_t &status,uint32_t &stamp) {
        if(fault())return false;
        if(generation!=generation_ || n!=copied_)return fail();
        if(!copyRecord(n,out,status,stamp))return false;
        ++copied_;
        return true;
    }
    // Commit a complete TX source batch only after both TX publications.
    // Preflight all conditions before releasing any group. Failure is sticky.
    bool acknowledge(uint64_t first,uint32_t generation,unsigned packets=128) {
        if(fault())return false;
        if((packets!=128 && packets!=256) || generation!=generation_ || first!=journal_.first() ||
           first>UINT64_MAX-packets || copied_!=first+packets ||
           (observing_ && observed_<first+packets) || journal_.queuedGroups()<packets/64)return fail();
        for(unsigned offset=0;offset<packets;offset+=64)
            if(journal_.consume(first+offset)!=JournalError::none)return fail();
        return true;
    }
};
}
