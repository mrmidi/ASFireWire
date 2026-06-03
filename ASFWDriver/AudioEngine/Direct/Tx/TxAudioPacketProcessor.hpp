#pragma once

#include "../DirectOutputReader.hpp"
#include "DirectTxPacketScratch.hpp"
#include "DirectTxTypes.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

struct TxAudioPacketRequest final {
    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
    uint32_t am824Slots{0};

    uint8_t sid{0};
    uint8_t dbc{0};
    uint16_t syt{0};
    bool dataPacket{true};
};

struct TxAudioPacketResult final {
    DirectTxReadStatus readStatus{DirectTxReadStatus::kUnavailable};
    uint32_t framesEncoded{0};
    bool usedSilence{false};
};

class TxAudioPacketProcessor final {
public:
    explicit TxAudioPacketProcessor(DirectOutputReader& reader) noexcept
        : reader_(reader) {}

    [[nodiscard]] TxAudioPacketResult BuildScratchPacket(const TxAudioPacketRequest& request,
                                                         DirectTxPacketScratch& scratch) noexcept;

private:
    [[nodiscard]] static uint32_t EffectiveAm824Slots(const TxAudioPacketRequest& request) noexcept;

    DirectOutputReader& reader_;
};

} // namespace ASFW::AudioEngine::Direct::Tx
