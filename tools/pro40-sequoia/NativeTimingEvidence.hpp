// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "InputTimingEvidence.hpp"
namespace Pro40 {
// Caller allocates/touches before native setup and owns through teardown/export.
// Single native control owner; packet content stays in its separate recorder.
struct NativeTimingEvidence {
    NativeTimingEvidence()=default;
    NativeTimingEvidence(const NativeTimingEvidence &)=delete;
    NativeTimingEvidence(NativeTimingEvidence &&)=delete; // Consumer bridge retains this address.
    struct Clock {
        uint64_t before=0,uptime=0,after=0;
        uint32_t progress=0,generation=0,cycle=0,status=0;
        uint32_t packetCount=0;
        bool beforeReceive=false;
    };
    Pro40Packets::InputTimingEvidence<> packets;
    std::array<Clock,4096> clocks{};
    size_t used=0;
    uint32_t numer=0,denom=0;
    bool fault=false;
    // Optional diagnostic sink: caller owns context through synchronous teardown.
    void *observerContext=nullptr;
    void (*clockObserver)(void *,const Clock &,bool) noexcept=nullptr;
    void (*pollObserver)(void *) noexcept=nullptr;
    // Attach before native entry; callbacks/context outlive synchronous teardown.
    // All callbacks must be bounded/nonblocking. They receive borrowed metadata;
    // copy anything needed by another thread. Recorder already contains the PCM.
    struct Consumer {
        void *context=nullptr;
        bool (*record)(void *,size_t,const Pro40Packets::InputTimingEvidence<>::Record &) noexcept=nullptr;
        bool (*clock)(void *,const Clock &) noexcept=nullptr;
        bool (*poll)(void *) noexcept=nullptr;
    };
private:
    Consumer consumer_{};bool consumerAttached_=false,consumerFault_=false;
public:
    bool attachConsumer(Consumer consumer) noexcept {
        if(consumerAttached_||used||!packets.records().empty()||!consumer.record||!consumer.clock||!consumer.poll)return false;
        consumer_=consumer;consumerAttached_=true;
        packets.consumer={this,[](void *context,size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &row) noexcept {
            auto &s=*static_cast<NativeTimingEvidence *>(context);
            return !s.consumerFailed()&&s.consumer_.record(s.consumer_.context,ordinal,row);
        }};return true;
    }
    bool hasConsumer() const noexcept {return consumerAttached_;}
    bool consumerFailed() const noexcept {
        return consumerAttached_&&(consumerFault_||packets.consumerFault()||fault||packets.fault()||!packets.valid());
    }
    bool pollConsumer() noexcept {
        if(!consumerAttached_)return true;
        if(consumerFailed())return false;
        if(!consumer_.poll(consumer_.context))consumerFault_=true;
        return !consumerFailed();
    }
    void observe(Clock row) noexcept {
        if(fault)return;
        if(used==clocks.size()){fault=true;if(clockObserver)clockObserver(observerContext,row,true);return;}
        clocks[used++]=row;
        if(row.status || !numer || !denom || row.before>row.uptime || row.uptime>row.after ||
           row.packetCount!=packets.records().size() ||
           (used==1 ? (!row.beforeReceive || row.packetCount!=0) : row.beforeReceive) ||
           ((row.cycle>>12)&8191)>=8000 || (row.cycle&4095)>=3072 ||
           (used>1 && (row.generation!=clocks[0].generation || row.uptime<=clocks[used-2].uptime)))fault=true;
        if(clockObserver)clockObserver(observerContext,row,fault);
        if(consumerAttached_&&!consumerFailed()&&!consumer_.clock(consumer_.context,row))consumerFault_=true;
    }
    bool valid() const noexcept {
        return packets.valid() && !packets.fault() && !fault && numer && denom && used>=2 && !packets.records().empty()
            && packets.records()[0].generation==clocks[0].generation;
    }
};
}
