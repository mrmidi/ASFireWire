#pragma once

#include "../../../Isoch/Config/AudioConstants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

constexpr uint32_t kDirectTxCipHeaderBytes = 8;
constexpr uint32_t kMaxDirectTxScratchFrames = 8;
constexpr std::size_t kMaxDirectTxScratchBytes =
    kDirectTxCipHeaderBytes +
    (static_cast<std::size_t>(kMaxDirectTxScratchFrames) *
     ASFW::Isoch::Config::kMaxAmdtpDbs *
     sizeof(uint32_t));

struct DirectTxPacketScratch final {
    alignas(uint32_t) std::array<uint8_t, kMaxDirectTxScratchBytes> bytes{};
    uint32_t length{0};
    uint32_t framesEncoded{0};
    bool usedSilence{false};

    void Reset() noexcept {
        bytes.fill(0);
        length = 0;
        framesEncoded = 0;
        usedSilence = false;
    }
};

} // namespace ASFW::AudioEngine::Direct::Tx
