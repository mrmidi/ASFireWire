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

    // Refined Phase A counters
    std::atomic<uint64_t> txValidPhasePcmPackets{0};
    std::atomic<uint64_t> txValidPhaseSilencePackets{0};
    std::atomic<uint64_t> txNoPhaseSilencePackets{0};
    std::atomic<uint64_t> txUnderrunSilencePackets{0};
    std::atomic<uint64_t> txStaleSyncPackets{0};
    std::atomic<uint64_t> txInvalidGeometryPackets{0};
    std::atomic<uint64_t> txSytFfffPackets{0};
    std::atomic<uint64_t> txValidSytPackets{0};
    std::atomic<uint64_t> txPcmFramesEncoded{0};
    std::atomic<uint64_t> txForwardCursorCorrections{0};
    std::atomic<uint64_t> txPreventedBackwardCorrections{0};
    std::atomic<uint64_t> txStaleOverwrittenReads{0};
    std::atomic<uint64_t> txProducerAheadUnderruns{0};
    std::atomic<uint64_t> txTimelineDiscontinuities{0};
    std::atomic<uint64_t> txPcmNonzeroPackets{0};
    std::atomic<uint64_t> txPcmAllZeroPackets{0};
    std::atomic<uint64_t> txTimelineInvariantFailures{0};

    std::atomic<uint64_t> rxPackets{0};
    std::atomic<uint64_t> rxDecodedFrames{0};
    std::atomic<uint64_t> rxDiscontinuities{0};

    std::atomic<uint64_t> ztsPublished{0};
    std::atomic<uint64_t> ztsRxPublished{0};
    std::atomic<uint64_t> ztsRxAdkPublished{0};

    void Reset() noexcept {
        ioBeginReadCount.store(0, std::memory_order_relaxed);
        ioWriteEndCount.store(0, std::memory_order_relaxed);

        txPackets.store(0, std::memory_order_relaxed);
        txDataPackets.store(0, std::memory_order_relaxed);
        txNoDataPackets.store(0, std::memory_order_relaxed);
        txSilenceSubstitutions.store(0, std::memory_order_relaxed);
        txUnderruns.store(0, std::memory_order_relaxed);

        txValidPhasePcmPackets.store(0, std::memory_order_relaxed);
        txValidPhaseSilencePackets.store(0, std::memory_order_relaxed);
        txNoPhaseSilencePackets.store(0, std::memory_order_relaxed);
        txUnderrunSilencePackets.store(0, std::memory_order_relaxed);
        txStaleSyncPackets.store(0, std::memory_order_relaxed);
        txInvalidGeometryPackets.store(0, std::memory_order_relaxed);
        txSytFfffPackets.store(0, std::memory_order_relaxed);
        txValidSytPackets.store(0, std::memory_order_relaxed);
        txPcmFramesEncoded.store(0, std::memory_order_relaxed);
        txForwardCursorCorrections.store(0, std::memory_order_relaxed);
        txPreventedBackwardCorrections.store(0, std::memory_order_relaxed);
        txStaleOverwrittenReads.store(0, std::memory_order_relaxed);
        txProducerAheadUnderruns.store(0, std::memory_order_relaxed);
        txTimelineDiscontinuities.store(0, std::memory_order_relaxed);
        txPcmNonzeroPackets.store(0, std::memory_order_relaxed);
        txPcmAllZeroPackets.store(0, std::memory_order_relaxed);
        txTimelineInvariantFailures.store(0, std::memory_order_relaxed);

        rxPackets.store(0, std::memory_order_relaxed);
        rxDecodedFrames.store(0, std::memory_order_relaxed);
        rxDiscontinuities.store(0, std::memory_order_relaxed);

        ztsPublished.store(0, std::memory_order_relaxed);
        ztsRxPublished.store(0, std::memory_order_relaxed);
        ztsRxAdkPublished.store(0, std::memory_order_relaxed);
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

    void CountRxZtsPublished() noexcept {
        ztsPublished.fetch_add(1, std::memory_order_relaxed);
        ztsRxPublished.fetch_add(1, std::memory_order_relaxed);
    }

    void CountRxAdkZtsPublished() noexcept {
        ztsRxAdkPublished.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace ASFW::Audio::Runtime
