#pragma once

#include "../DirectOutputReader.hpp"
#include "DirectTxTypes.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

struct TxAudioPacketWriteRequest final {
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
    uint32_t am824Slots{0};

    uint8_t sid{0};
    uint8_t dbc{0};
    uint16_t syt{0};
    bool dataPacket{true};
};

struct TxAudioPacketWriteResult final {
    DirectTxReadStatus readStatus{DirectTxReadStatus::kUnavailable};
    uint32_t bytesWritten{0};
    uint32_t framesEncoded{0};
    bool usedSilence{false};
};

class TxAudioPacketWriter final {
public:
    explicit TxAudioPacketWriter(DirectOutputReader& reader) noexcept
        : reader_(reader) {}

    [[nodiscard]] TxAudioPacketWriteResult WritePacket(const TxAudioPacketWriteRequest& request,
                                                       uint8_t* packetBytes,
                                                       uint32_t packetCapacityBytes) noexcept;

private:
    [[nodiscard]] static uint32_t EffectiveAm824Slots(const TxAudioPacketWriteRequest& request) noexcept;

    DirectOutputReader& reader_;
};

} // namespace ASFW::AudioEngine::Direct::Tx
