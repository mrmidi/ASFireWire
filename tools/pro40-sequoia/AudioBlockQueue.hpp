// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Pro40Audio {
// Process-local prototype only. One producer and one consumer for the entire
// lifetime. Destroy only after both have quiesced; no concurrent reset/reuse.
// Not an IPC ABI, HAL timeline, or permission to share std::atomic across processes.
struct InputBlock {
    uint64_t epoch=0;
    uint64_t firstFrame=0;
    std::array<int32_t,8> inputOne{}; // Signed PCM24, without gain or resampling.
};
template<size_t Capacity> class AudioBlockQueue {
    static_assert(Capacity>0 && Capacity<std::numeric_limits<uint64_t>::max()/2);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::array<InputBlock,Capacity> blocks_{};
    alignas(64) std::atomic<uint64_t> written_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
public:
    // Each call has fixed work: no spin, wait, allocation, IO or overwrite.
    // False leaves both queue and caller's block unchanged. Caller decides
    // whether full/empty is a fault; never silently skip an audio interval.
    bool tryPush(const InputBlock &block) noexcept {
        const auto written=written_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        if(written==std::numeric_limits<uint64_t>::max() || written-read>=Capacity)return false;
        blocks_[written%Capacity]=block;
        written_.store(written+1,std::memory_order_release);
        return true;
    }
    bool tryPop(InputBlock &block) noexcept {
        const auto read=read_.load(std::memory_order_relaxed);
        if(read==written_.load(std::memory_order_acquire))return false;
        block=blocks_[read%Capacity];
        read_.store(read+1,std::memory_order_release);
        return true;
    }
};
}
