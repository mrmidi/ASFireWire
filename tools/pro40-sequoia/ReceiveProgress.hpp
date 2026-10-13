// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <limits>
namespace NativePackets {
// Native status/timestamp format matches NativeReceivePacket.hpp:25-40.
// Fixed transport bookkeeping. Publish only after a stable group has been copied
// into owned storage; this counter/anchor does not itself grant a DMA lease.
class ReceiveProgress {
    uint64_t next_=0;
    uint32_t generation_,anchor_=0;
    bool anchored_=false,fault_=false;
    bool fail() {fault_=true;return false;}
    static unsigned cycle(uint32_t stamp) {return ((stamp>>25)&7)*8000+((stamp>>12)&8191);}
public:
    explicit ReceiveProgress(uint32_t generation):generation_(generation) {}
    bool fault() const {return fault_;}
    uint64_t next() const {return next_;}
    uint32_t firstTimestamp() const {return anchor_;}
    bool ready(unsigned minimum) const {return !fault_ && anchored_ && next_>=minimum;}
    template<class Source> bool publish(uint64_t first,uint32_t generation,Source source) {
        if(fault_)return false;
        if(first!=next_ || generation!=generation_ || next_>std::numeric_limits<uint64_t>::max()-64)return fail();
        const auto anchor=anchored_?anchor_:source(first).timestamp;
        for(unsigned i=0;i<64;++i) {
            const auto record=source(first+i);
            if((record.status&31)!=17 || (record.timestamp&0xf0000fff) ||
               ((record.timestamp>>12)&8191)>=8000 ||
               cycle(record.timestamp)!=(cycle(anchor)+(first+i)%64000)%64000)return fail();
        }
        anchor_=anchor;anchored_=true;next_+=64;return true;
    }
};
static_assert(sizeof(ReceiveProgress)<=32);
}
