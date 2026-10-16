// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketBuffer.hpp"
#include <algorithm>

namespace NativePackets {
// Finite experiment: the final Reused slots alias the first Reused payloads.
// The caller must keep aliases unreachable until the first Reused records are
// copied and canExtend() succeeds. Records are indexed by logical receive order;
// physical descriptor visits and their metadata are accounted for separately.
constexpr unsigned reusedSlot(unsigned logical,unsigned slots=512,unsigned reused=64) {
    return logical>=slots-reused?logical-(slots-reused):logical;
}
template<unsigned Slots=512,unsigned Reused=64>
class ReceiveArchive {
    static_assert(Reused>0 && Reused<Slots && Slots%64==0 && Reused%64==0);
    struct Record {std::array<uint8_t,448> bytes{};uint32_t status=0,timestamp=0;};
    std::array<Record,Slots> records_{};
    unsigned next_=0;
    bool fault_=false;
public:
    unsigned next() const {return next_;}
    bool fault() const {return fault_;}
    bool canExtend() const {return !fault_ && next_>=Reused;}
    bool append(unsigned sequence,CompletedBuffer source) {
        if(fault_)return false;
        if(sequence!=next_ || next_==records_.size() || source.bytes.size()!=448) {
            fault_=true;return false;
        }
        auto &record=records_[next_];
        std::copy(source.bytes.begin(),source.bytes.end(),record.bytes.begin());
        record.status=source.status;record.timestamp=source.timestamp;
        ++next_;return true;
    }
    CompletedBuffer view(unsigned sequence) const {
        if(fault_ || sequence>=next_)return {};
        const auto &record=records_[sequence];
        return {record.bytes,record.status,record.timestamp};
    }
};
}
