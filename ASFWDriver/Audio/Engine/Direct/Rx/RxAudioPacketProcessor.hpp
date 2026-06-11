#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "../../../Wire/AMDTP/PacketAssembler.hpp"

#include <cstdint>
#include <cstddef>

namespace ASFW::AudioEngine::Direct::Rx {

struct RxAudioPacketProcessorResult final {
    DirectRxWriteStatus status{DirectRxWriteStatus::kUnavailable};
    uint32_t framesDecoded{0};
    bool hasValidCip{false};
    uint16_t syt{0xFFFF};
    uint8_t fdf{0};
    uint8_t dbs{0};
    uint8_t dbc{0};
};

class RxAudioPacketProcessor final {
public:
    explicit RxAudioPacketProcessor(DirectInputWriter& writer) noexcept
        : writer_(writer) {}

    [[nodiscard]] RxAudioPacketProcessorResult ProcessPacket(const uint8_t* payload,
                                                             size_t length,
                                                             uint64_t absoluteFrame,
                                                             uint32_t channels,
                                                             uint32_t am824Slots,
                                                             ASFW::Encoding::AudioWireFormat format) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
