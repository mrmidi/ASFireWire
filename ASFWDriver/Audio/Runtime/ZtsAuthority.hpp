#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class ZtsAuthoritySource : uint8_t {
    None = 0,
    RxClock,
    TxClock,
    DuplexAggregate,
};

inline const char* ToString(ZtsAuthoritySource source) noexcept {
    switch (source) {
        case ZtsAuthoritySource::None:            return "None";
        case ZtsAuthoritySource::RxClock:         return "RxClock";
        case ZtsAuthoritySource::TxClock:         return "TxClock";
        case ZtsAuthoritySource::DuplexAggregate: return "DuplexAggregate";
        default:                                  return "Unknown";
    }
}

enum class ZtsPublicationMode : uint8_t {
    DirectToHAL = 0,
    MirrorPump,
};

inline const char* ToString(ZtsPublicationMode mode) noexcept {
    switch (mode) {
        case ZtsPublicationMode::DirectToHAL: return "DirectToHAL";
        case ZtsPublicationMode::MirrorPump:  return "MirrorPump";
        default:                              return "Unknown";
    }
}

struct ZtsAuthorityState {
    std::atomic<ZtsAuthoritySource> selectedSource{ZtsAuthoritySource::None};
    std::atomic<ZtsPublicationMode> selectedMode{ZtsPublicationMode::MirrorPump};
    std::atomic<uint64_t> sourceGeneration{0};
    std::atomic<uint64_t> authoritativeSampleFrame{0};
    std::atomic<uint64_t> authoritativeHostTicks{0};
    std::atomic<uint32_t> hostNanosPerSampleQ8{0};

    // Counters
    std::atomic<uint64_t> rxSourceUpdates{0};
    std::atomic<uint64_t> txSourceUpdates{0};
    std::atomic<uint64_t> mirrorPublications{0};
    std::atomic<uint64_t> directPublications{0};
    std::atomic<uint64_t> rejectedRxUpdates{0};
    std::atomic<uint64_t> rejectedTxUpdates{0};
    std::atomic<uint64_t> staleSourceUpdates{0};

    void Reset() noexcept {
        selectedSource.store(ZtsAuthoritySource::None, std::memory_order_relaxed);
        selectedMode.store(ZtsPublicationMode::MirrorPump, std::memory_order_relaxed);
        sourceGeneration.store(0, std::memory_order_relaxed);
        authoritativeSampleFrame.store(0, std::memory_order_relaxed);
        authoritativeHostTicks.store(0, std::memory_order_relaxed);
        hostNanosPerSampleQ8.store(0, std::memory_order_relaxed);

        rxSourceUpdates.store(0, std::memory_order_relaxed);
        txSourceUpdates.store(0, std::memory_order_relaxed);
        mirrorPublications.store(0, std::memory_order_relaxed);
        directPublications.store(0, std::memory_order_relaxed);
        rejectedRxUpdates.store(0, std::memory_order_relaxed);
        rejectedTxUpdates.store(0, std::memory_order_relaxed);
        staleSourceUpdates.store(0, std::memory_order_relaxed);
    }
};

} // namespace ASFW::Audio::Runtime
