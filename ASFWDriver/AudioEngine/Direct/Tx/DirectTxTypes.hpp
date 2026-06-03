#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

enum class DirectTxReadStatus : uint32_t {
    kUnavailable = 0,
    kAvailable = 1,
    kUnderrun = 2,
    kInvalidBinding = 3,
    kInvalidRange = 4,
};

struct DirectTxReadRequest final {
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
};

struct DirectTxReadResult final {
    DirectTxReadStatus status{DirectTxReadStatus::kUnavailable};
    const int32_t* firstFramePtr{nullptr};
    uint64_t writtenEndFrame{0};
    uint64_t requestedEndFrame{0};
};

struct DirectTxProbeCounters final {
    uint64_t probes{0};
    uint64_t available{0};
    uint64_t underruns{0};
    uint64_t invalidBinding{0};
    uint64_t invalidRange{0};
};

} // namespace ASFW::AudioEngine::Direct::Tx
