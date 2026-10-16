// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
namespace Pro40Audio {
struct HalCallbackTimingRecord {
    uint64_t host=0,anchor=0,seed=0;
    uint32_t client=0,frames=0,numer=0,denom=0;
    AudioServerPlugInIOCycleInfo cycle{};
    uint32_t operation=kAudioServerPlugInIOOperationReadInput;
};
static_assert(std::is_trivially_copyable_v<HalCallbackTimingRecord>);
// Bounded ring. Many writers, exactly one drain caller. Each slot has separate
// free/ready sequence values, so even capacity1 cannot reuse a published slot
// before the consumer finishes reading it. No retries, allocation or waits on
// the callback path: contention/full capacity drops only diagnostic evidence.
// Owner must externally quiesce/join every caller before destroying storage.
template<size_t Capacity> class HalCallbackTiming {
    static_assert(Capacity>0 && Capacity<=UINT32_MAX);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    struct Slot {HalCallbackTimingRecord record{};std::atomic<uint64_t> sequence{0};};
    std::array<Slot,Capacity> slots_{};
    std::atomic<uint64_t> claimed_{0};
    uint64_t consumed_=0; // Owned only by the drain caller.
    std::atomic<uint64_t> dropped_{0};
public:
    HalCallbackTiming() noexcept {
        for(size_t i=0;i<Capacity;++i)slots_[i].sequence.store(2*i,std::memory_order_relaxed);
    }
    bool record(const HalCallbackTimingRecord &record) noexcept {
        auto index=claimed_.load(std::memory_order_relaxed);
        // Stop before sequence arithmetic can wrap; never permit ticket ABA.
        if(index>UINT64_MAX/2-Capacity ||
            slots_[index%Capacity].sequence.load(std::memory_order_acquire)!=2*index ||
            !claimed_.compare_exchange_strong(index,index+1,
            std::memory_order_relaxed,std::memory_order_relaxed)) {
            dropped_.fetch_add(1,std::memory_order_relaxed);return false;
        }
        auto &slot=slots_[index%Capacity];slot.record=record;
        slot.sequence.store(2*index+1,std::memory_order_release);return true;
    }
    // Control/test path only. An in-flight earlier writer holds the FIFO head;
    // later drains revisit it. Never reorder callbacks around an unpublished
    // slot. Bound each drain even while producers continue filling the ring.
    template<class Consumer> size_t drain(Consumer &&consume) {
        size_t count=0;
        while(count<Capacity) {
            auto &slot=slots_[consumed_%Capacity];
            if(slot.sequence.load(std::memory_order_acquire)!=2*consumed_+1)break;
            consume(slot.record);
            slot.sequence.store(2*(consumed_+Capacity),std::memory_order_release);
            ++consumed_;++count;
        }
        return count;
    }
    uint64_t dropped() const noexcept {return dropped_.load(std::memory_order_relaxed);}
};
}
