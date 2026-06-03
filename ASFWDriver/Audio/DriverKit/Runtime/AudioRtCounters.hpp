#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct AudioRtCounters final {
    std::atomic<uint64_t> ioBeginReadCount{0};
    std::atomic<uint64_t> ioWriteEndCount{0};

    std::atomic<uint64_t> txPackets{0};
    std::atomic<uint64_t> txDataPackets{0};
    std::atomic<uint64_t> txNoDataPackets{0};
    std::atomic<uint64_t> txSilenceSubstitutions{0};
    std::atomic<uint64_t> txUnderruns{0};

    std::atomic<uint64_t> rxPackets{0};
    std::atomic<uint64_t> rxDecodedFrames{0};
    std::atomic<uint64_t> rxDiscontinuities{0};

    std::atomic<uint64_t> ztsPublished{0};

    void Reset() noexcept {
        ioBeginReadCount.store(0, std::memory_order_relaxed);
        ioWriteEndCount.store(0, std::memory_order_relaxed);

        txPackets.store(0, std::memory_order_relaxed);
        txDataPackets.store(0, std::memory_order_relaxed);
        txNoDataPackets.store(0, std::memory_order_relaxed);
        txSilenceSubstitutions.store(0, std::memory_order_relaxed);
        txUnderruns.store(0, std::memory_order_relaxed);

        rxPackets.store(0, std::memory_order_relaxed);
        rxDecodedFrames.store(0, std::memory_order_relaxed);
        rxDiscontinuities.store(0, std::memory_order_relaxed);

        ztsPublished.store(0, std::memory_order_relaxed);
    }

    void CountBeginRead() noexcept {
        ioBeginReadCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CountWriteEnd() noexcept {
        ioWriteEndCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CountZtsPublished() noexcept {
        ztsPublished.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace ASFW::Audio::Runtime
