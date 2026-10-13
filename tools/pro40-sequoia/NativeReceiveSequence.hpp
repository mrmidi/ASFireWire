// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace NativePackets {
enum class SequenceError { none, geometry, generation, order, exhausted };
// Accounts for callback order only. A logical pass is NOT evidence that DMA
// storage is fresh or stable: that requires a separate hardware reuse protocol.
class ReceiveSequence {
    uint64_t slots_, limit_=0, next_=0;
    uint32_t generation_;
    SequenceError error_=SequenceError::none;
public:
    ReceiveSequence(uint64_t slots,uint64_t passes,uint32_t generation,uint64_t records=0)
        :slots_(slots),generation_(generation) {
        if(!slots || slots%64 || !passes || passes>std::numeric_limits<uint64_t>::max()/slots)
            error_=SequenceError::geometry;
        else {
            limit_=slots*passes;
            if(records) {
                if(records%64 || records>limit_)error_=SequenceError::geometry;
                else limit_=records;
            }
        }
    }
    SequenceError error() const {return error_;}
    uint64_t next() const {return next_;}
    std::optional<uint64_t> accept(uint64_t physicalFirst,uint32_t generation) {
        if(error_!=SequenceError::none)return std::nullopt;
        if(generation!=generation_)error_=SequenceError::generation;
        else if(next_==limit_)error_=SequenceError::exhausted;
        else if(physicalFirst!=next_%slots_)error_=SequenceError::order;
        if(error_!=SequenceError::none)return std::nullopt;
        const auto first=next_;next_+=64;return first;
    }
};
}
