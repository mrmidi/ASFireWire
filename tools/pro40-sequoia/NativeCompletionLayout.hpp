// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <optional>
namespace NativePackets {
// Completion notifications only: packet scheduling and DMA bank geometry are
// unchanged. A bank layout splits its first64 slots into1+63, then uses64.
// This retains the bank+1 refill notification instead of delaying it63 cycles.
struct CompletionLayout {
    unsigned group=1,bank=0;
    constexpr bool valid() const {
        return (group==1 && bank==0) || (group==64 && (bank==0 || bank==128 || bank==256));
    }
    constexpr unsigned endingAt(uint64_t slot) const {
        if(!valid())return 0;
        if(group==1)return 1;
        if(bank && slot%bank==0)return 1;
        if(slot%64!=63)return 0;
        return bank && slot%bank==63?63:64;
    }
    // A prefix must end at an actual notification. Partial groups cannot be
    // reported as completed, even when DMA has already processed their slots.
    constexpr std::optional<uint64_t> notifications(uint64_t progress) const {
        if(!valid() || (progress && !endingAt(progress-1)))return {};
        if(!bank)return progress/group;
        return progress/bank*(bank/64+1)+(progress%bank)/64+(progress%bank?1:0);
    }
};
static_assert(CompletionLayout{64,256}.notifications(512)==10);
static_assert(CompletionLayout{64,256}.endingAt(256)==1);
static_assert(CompletionLayout{64,256}.endingAt(319)==63);
}
