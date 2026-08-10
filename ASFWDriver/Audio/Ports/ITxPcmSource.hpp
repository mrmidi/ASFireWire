#pragma once

#include <cstdint>

namespace ASFW::Audio::Ports {

enum class TxPcmReadResult : uint8_t {
    kReady = 0,
    kNotYetWritten,
    kStaleOverwritten,
    kSnapshotBusy,
    kInvalidRequest,
};

[[nodiscard]] constexpr const char* TxPcmReadResultName(
    TxPcmReadResult result) noexcept {
    switch (result) {
        case TxPcmReadResult::kReady:
            return "ready";
        case TxPcmReadResult::kNotYetWritten:
            return "not-yet-written";
        case TxPcmReadResult::kStaleOverwritten:
            return "stale-overwritten";
        case TxPcmReadResult::kSnapshotBusy:
            return "snapshot-busy";
        case TxPcmReadResult::kInvalidRequest:
            return "invalid-request";
    }
    return "unknown";
}

struct TxPcmReadRequest final {
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

    [[nodiscard]] virtual TxPcmReadResult ReadFloat32Interleaved(
        const TxPcmReadRequest& request,
        float* destination,
        uint32_t destinationSampleCapacity) const noexcept = 0;

    [[nodiscard]] virtual uint64_t OldestValidFrame() const noexcept = 0;
    [[nodiscard]] virtual uint64_t WrittenEndFrame() const noexcept = 0;
};

} // namespace ASFW::Audio::Ports
