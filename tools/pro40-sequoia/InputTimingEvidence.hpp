// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Pro40Packets.hpp"
#include <memory>
#include <new>
namespace Pro40Packets {
// Content-side diagnostic only. Allocate/touch before native setup, serialize
// observe calls. Same control owner may snapshot record count during capture;
// export only after the owner stops. No IO or clock fit.
// Linux amdtp-stream.c:515-525 preserves received SYT and data-block counts.
// Keep raw SYT here: its association to an audio frame is not inferred from
// callback dispatch time or from the packet's first-frame ordinal.
template<size_t Capacity=262144> class InputTimingEvidence {
    static_assert(Capacity>0 && Capacity<=262144);
public:
    struct Record {
        uint64_t firstFrame=0;
        uint32_t generation=0,arrivalCycle=0;
        uint16_t syt=0xffff;
        uint8_t blocks=0,dbc=0;
    };
    struct Observer {
        void *context=nullptr;
        void (*record)(void *,size_t,const Record &) noexcept=nullptr;
        void (*failed)(void *) noexcept=nullptr;
    } observer;
    // Optional active content consumer. No waits/IO; same serialized owner.
    struct Consumer {
        void *context=nullptr;
        bool (*record)(void *,size_t,const Record &) noexcept=nullptr;
    } consumer;
private:
    std::unique_ptr<Record[]> records_{new(std::nothrow) Record[Capacity]()};
    size_t used_=0;uint64_t frames_=0;
    Continuity continuity_;
    bool fault_=false;
    bool consumerFault_=false;
    void fail() noexcept {fault_=true;if(observer.failed)observer.failed(observer.context);}
public:
    bool valid() const noexcept {return bool(records_);}
    bool fault() const noexcept {return fault_;}
    bool consumerFault() const noexcept {return consumerFault_;}
    uint64_t frames() const noexcept {return frames_;}
    std::span<const Record> records() const noexcept {return {records_.get(),used_};}
    void observe(std::span<const uint8_t> payload,uint32_t arrival,uint32_t generation,uint8_t sender) noexcept {
        if(fault_)return;
        if(!valid() || used_==Capacity || arrival>=64000) {fail();return;}
        const auto parsed=parseCaptureTiming(payload,0,sender);
        if(!parsed || (parsed.packet.blocks && parsed.packet.syt==0xffff) ||
           (used_ && (generation!=records_[0].generation || arrival!=(records_[used_-1].arrivalCycle+1)%64000))) {
            fail();return;
        }
        if(!continuity_.accept(parsed.packet)) {fail();return;}
        const auto &p=parsed.packet;
        records_[used_++]={frames_,generation,arrival,p.syt,p.blocks,p.dbc};
        frames_+=p.blocks;
        if(observer.record)observer.record(observer.context,used_-1,records_[used_-1]);
        if(consumer.record&&!consumerFault_&&!consumer.record(consumer.context,used_-1,records_[used_-1]))consumerFault_=true;
    }
};
}
