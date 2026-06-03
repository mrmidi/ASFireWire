#pragma once

#include "DirectTxPacketScratch.hpp"

#include "../../../AudioWire/AM824/AM824Encoder.hpp"
#include "../../../AudioWire/CIP/CIPHeaderBuilder.hpp"
#include "../../../Isoch/Config/AudioConstants.hpp"
#include "../../../AudioWire/RawPcm24In32/RawPcm24In32Encoder.hpp"
#include "../../../AudioWire/AMDTP/PacketAssembler.hpp"

#include <cstdint>
#include <cstring>

namespace ASFW::AudioEngine::Direct::Tx {

struct DirectTxPacketHeaderRequest final {
    uint8_t sid{0};
    uint32_t am824Slots{0};
    uint8_t dbc{0};
    uint16_t syt{ASFW::Encoding::kSYTNoData};
    bool isNoData{false};
};

[[nodiscard]] constexpr uint32_t DirectTxPacketByteCount(uint32_t frames,
                                                         uint32_t am824Slots) noexcept {
    return kDirectTxCipHeaderBytes + (frames * am824Slots * static_cast<uint32_t>(sizeof(uint32_t)));
}

[[nodiscard]] constexpr bool IsValidDirectTxGeometry(uint32_t frames,
                                                     uint32_t pcmChannels,
                                                     uint32_t am824Slots) noexcept {
    return frames > 0 &&
           frames <= kMaxDirectTxScratchFrames &&
           pcmChannels > 0 &&
           pcmChannels <= ASFW::Isoch::Config::kMaxPcmChannels &&
           am824Slots >= pcmChannels &&
           am824Slots <= ASFW::Isoch::Config::kMaxAmdtpDbs &&
           DirectTxPacketByteCount(frames, am824Slots) <= kMaxDirectTxScratchBytes;
}

[[nodiscard]] inline uint32_t EncodeDirectTxMidiPlaceholder(uint32_t midiSlotIndex) noexcept {
    const uint8_t label = static_cast<uint8_t>(
        ASFW::Encoding::kAM824LabelMIDIConformantBase + (midiSlotIndex & 0x03u));
    return ASFW::Encoding::AM824Encoder::encodeLabelOnly(label);
}

inline void EncodeDirectTxPcmFrame(const int32_t* pcmFrame,
                                   uint32_t pcmChannels,
                                   uint32_t am824Slots,
                                   ASFW::Encoding::AudioWireFormat format,
                                   uint32_t* outWireQuadlets) noexcept {
    const uint32_t midiSlots = (am824Slots > pcmChannels) ? (am824Slots - pcmChannels) : 0;
    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
        if (format == ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
            outWireQuadlets[ch] = ASFW::Encoding::RawPcm24In32::Encode(pcmFrame[ch]);
        } else {
            outWireQuadlets[ch] = ASFW::Encoding::AM824Encoder::encode(pcmFrame[ch]);
        }
    }
    for (uint32_t s = 0; s < midiSlots; ++s) {
        outWireQuadlets[pcmChannels + s] = EncodeDirectTxMidiPlaceholder(s);
    }
}

inline void EncodeDirectTxSilenceFrame(uint32_t pcmChannels,
                                      uint32_t am824Slots,
                                      ASFW::Encoding::AudioWireFormat format,
                                      uint32_t* outWireQuadlets) noexcept {
    const uint32_t midiSlots = (am824Slots > pcmChannels) ? (am824Slots - pcmChannels) : 0;
    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
        if (format == ASFW::Encoding::AudioWireFormat::kRawPcm24In32) {
            outWireQuadlets[ch] = ASFW::Encoding::RawPcm24In32::EncodeSilence();
        } else {
            outWireQuadlets[ch] = ASFW::Encoding::AM824Encoder::encodeSilence();
        }
    }
    for (uint32_t s = 0; s < midiSlots; ++s) {
        outWireQuadlets[pcmChannels + s] = EncodeDirectTxMidiPlaceholder(s);
    }
}

[[nodiscard]] inline bool BeginDirectTxPacket(const DirectTxPacketHeaderRequest& request,
                                              uint8_t* packetBytes,
                                              uint32_t packetCapacityBytes,
                                              uint32_t& bytesWritten) noexcept {
    bytesWritten = 0;
    if (!packetBytes || packetCapacityBytes < kDirectTxCipHeaderBytes) {
        return false;
    }
    if (request.am824Slots == 0 || request.am824Slots > ASFW::Isoch::Config::kMaxAmdtpDbs) {
        return false;
    }

    ASFW::Encoding::CIPHeaderBuilder builder(request.sid, static_cast<uint8_t>(request.am824Slots));
    const ASFW::Encoding::CIPHeader cip = request.isNoData
        ? builder.buildNoData(request.dbc)
        : builder.build(request.dbc, request.syt, false);

    std::memcpy(packetBytes, &cip.q0, sizeof(cip.q0));
    std::memcpy(packetBytes + sizeof(cip.q0), &cip.q1, sizeof(cip.q1));
    bytesWritten = kDirectTxCipHeaderBytes;
    return true;
}

[[nodiscard]] inline uint32_t* DirectTxPacketPayloadQuadlets(uint8_t* packetBytes) noexcept {
    if (!packetBytes) {
        return nullptr;
    }
    return reinterpret_cast<uint32_t*>(packetBytes + kDirectTxCipHeaderBytes);
}

[[nodiscard]] inline const uint32_t* DirectTxPacketPayloadQuadlets(const uint8_t* packetBytes) noexcept {
    if (!packetBytes) {
        return nullptr;
    }
    return reinterpret_cast<const uint32_t*>(packetBytes + kDirectTxCipHeaderBytes);
}

[[nodiscard]] inline bool BeginDirectTxScratchPacket(const DirectTxPacketHeaderRequest& request,
                                                     DirectTxPacketScratch& scratch) noexcept {
    scratch.Reset();
    return BeginDirectTxPacket(request,
                               scratch.bytes.data(),
                               static_cast<uint32_t>(scratch.bytes.size()),
                               scratch.length);
}

[[nodiscard]] inline uint32_t* DirectTxScratchPayloadQuadlets(DirectTxPacketScratch& scratch) noexcept {
    return DirectTxPacketPayloadQuadlets(scratch.bytes.data());
}

[[nodiscard]] inline const uint32_t* DirectTxScratchPayloadQuadlets(const DirectTxPacketScratch& scratch) noexcept {
    return DirectTxPacketPayloadQuadlets(scratch.bytes.data());
}

} // namespace ASFW::AudioEngine::Direct::Tx
