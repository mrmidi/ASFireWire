#pragma once

#include "../DirectOutputReader.hpp"
#include "DirectTxTypes.hpp"
#include "../../../Audio/Runtime/TxPacketState.hpp"
#include "../../../AudioWire/AMDTP/PacketAssembler.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

using ASFW::Audio::Runtime::TxPacketState;
using ASFW::Audio::Runtime::TxBlockingResult;
using ASFW::Audio::Runtime::TxPacketProductionResult;

struct TxAudioPacketWriteRequest final {
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
    uint32_t am824Slots{0};

    uint8_t sid{0};
    uint8_t dbc{0};
    uint16_t syt{0};
    bool dataPacket{true};
    ASFW::Encoding::AudioWireFormat wireFormat = ASFW::Encoding::AudioWireFormat::kAM824;
};

class TxAudioPacketWriter final {
public:
    explicit TxAudioPacketWriter(DirectOutputReader& reader) noexcept
        : reader_(reader) {}

    [[nodiscard]] TxPacketProductionResult WritePacket(const TxAudioPacketWriteRequest& request,
                                                       uint8_t* packetBytes,
                                                       uint32_t packetCapacityBytes) noexcept;

private:
    [[nodiscard]] static uint32_t EffectiveAm824Slots(const TxAudioPacketWriteRequest& request) noexcept;

    DirectOutputReader& reader_;
};

} // namespace ASFW::AudioEngine::Direct::Tx
