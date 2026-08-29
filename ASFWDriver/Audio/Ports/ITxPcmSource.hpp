#pragma once

#include <cstdint>

namespace ASFW::Audio::Ports {

enum class PcmCopyResult : uint8_t {
    Ready = 0,
    NotYetPublished,
    Expired,
    WrongEpoch,
    ConcurrentRewrite,
    InvalidRequest,
};

[[nodiscard]] constexpr const char* PcmCopyResultName(
    PcmCopyResult result) noexcept {
    switch (result) {
        case PcmCopyResult::Ready:
            return "ready";
        case PcmCopyResult::NotYetPublished:
            return "not-yet-published";
        case PcmCopyResult::Expired:
            return "expired";
        case PcmCopyResult::WrongEpoch:
            return "wrong-epoch";
        case PcmCopyResult::ConcurrentRewrite:
            return "concurrent-rewrite";
        case PcmCopyResult::InvalidRequest:
            return "invalid-request";
    }
    return "unknown";
}

struct TxPcmReadRequest final {
    uint64_t epoch{0};
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t sourceChannelOffset{0};
    uint32_t channelCount{0};
};

// Audio-owned, read-only source used while constructing a complete AMDTP
// packet. The implementation must return a value-owned snapshot: the caller
// never receives a pointer into the host ring or into mutable staging storage.
class ITxPcmSource {
public:
    virtual ~ITxPcmSource() = default;

    [[nodiscard]] virtual PcmCopyResult CopyExact(
        const TxPcmReadRequest& request,
        float* destination,
        uint32_t destinationSampleCapacity) const noexcept = 0;

    [[nodiscard]] virtual uint64_t OldestValidFrame() const noexcept = 0;
    [[nodiscard]] virtual uint64_t PublishedEndFrame() const noexcept = 0;
    [[nodiscard]] virtual uint64_t Epoch() const noexcept = 0;
};

} // namespace ASFW::Audio::Ports
