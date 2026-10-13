// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketBuffer.hpp"
#include <algorithm>
#include <span>
namespace NativePackets {
// Diagnostic retention only. Failure is inspected after Stop; append cannot
// return a transport decision. Source bytes must be stable for this copy.
template<size_t Capacity> class TransmitRefillArchive {
    std::array<PreparedBuffer,Capacity> records_{};
    size_t next_=0;
    bool fault_=false;
public:
    void append(uint64_t logical,std::span<const uint8_t> mapped) {
        if(fault_)return;
        if(logical!=next_ || next_==Capacity || mapped.size()<8 ||
           mapped.size()>448 || mapped.size()%4) {fault_=true;return;}
        auto &record=records_[next_++];
        record.length=uint32_t(mapped.size());
        std::copy(mapped.begin(),mapped.end(),record.bytes.begin());
    }
    bool fault() const {return fault_;}
    size_t size() const {return next_;}
    std::span<const PreparedBuffer> records() const {return std::span(records_).first(next_);}
};
}
