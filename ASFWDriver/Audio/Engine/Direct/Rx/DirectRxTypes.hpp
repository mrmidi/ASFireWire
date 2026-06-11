#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

enum class DirectRxWriteStatus : uint32_t {
    kUnavailable = 0,
    kAvailable = 1,
    kInvalidBinding = 2,
    kInvalidRange = 3,
    kOverflow = 4,
};

struct DirectRxWriteRequest final {
    uint64_t absoluteFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
};

struct DirectRxWriteResult final {
    DirectRxWriteStatus status{DirectRxWriteStatus::kUnavailable};
    int32_t* firstFramePtr{nullptr};
    uint64_t producedEndFrame{0};
};

} // namespace ASFW::AudioEngine::Direct::Rx
