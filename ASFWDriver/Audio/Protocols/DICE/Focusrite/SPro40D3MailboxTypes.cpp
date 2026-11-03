// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxTypes.cpp - Routing table serializers.
// Byte offsets are behavioral facts from
// references/snd-firewire-ctl-services/protocols/dice/src/focusrite/spro40d3.rs
// (serialize_routing_low_rate / serialize_routing_high_rate). Fresh
// implementation; no LGPL code copied.

#include "SPro40D3MailboxTypes.hpp"

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {
namespace {

constexpr void PutRoute(std::span<uint8_t> frame, size_t offset,
                        uint32_t from, uint32_t to) noexcept {
    ::ASFW::FW::WriteBE32(frame.data() + offset, EncodeRouteEntry(from, to));
}

} // namespace

void SerializeRoutingLowRate(std::span<uint8_t, kRoutingLowRateFrameSize> frame,
                             const RouterOutSrc& outSrc,
                             const RouterMixerSrc& mixerSrc,
                             const RouterMeterSrc& meterSrc) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeSetRouting[i];
    // flags quadlet at payload [0..4]: zero at low rate (reference: high rate
    // sets frame[17]=1, low rate leaves it zero).

    // To stream: fixed analog-in i+1 -> stream channel i (18 channels).
    for (uint32_t i = 0; i < 18; ++i)
        PutRoute(frame, 20 + i * 4, 1 + i, kDstStream + i);

    // To analog outputs: five stereo pairs, inverted destination order.
    for (uint32_t i = 0; i < kOutputPairCount; ++i) {
        PutRoute(frame, 92 + i * 8, outSrc[i * 2],     kDstAnalog + 8 - i * 2);
        PutRoute(frame, 96 + i * 8, outSrc[1 + i * 2], kDstAnalog + 9 - i * 2);
    }
    PutRoute(frame, 132, outSrc[10], kDstSpdif);
    PutRoute(frame, 136, outSrc[11], kDstSpdif + 1);
    for (uint32_t i = 0; i < 8; ++i)
        PutRoute(frame, 140 + i * 4, outSrc[12 + i], kDstAdat + i);
    // Loopback outputs -> stream channels 18/19.
    PutRoute(frame, 172, outSrc[20], kDstStream + 18);
    PutRoute(frame, 176, outSrc[21], kDstStream + 19);

    // Mixer input sources.
    for (uint32_t i = 0; i < kChannelCount; ++i)
        PutRoute(frame, 180 + i * 4, mixerSrc[i], kDstMixer + i);

    // Master meter sources (destination id 0 per reference).
    PutRoute(frame, 252, meterSrc[0], 0x0);
    PutRoute(frame, 256, meterSrc[1], 0x0);
}

void SerializeRoutingHighRate(std::span<uint8_t, kRoutingHighRateFrameSize> frame,
                              const RouterOutSrc& outSrc,
                              const RouterMixerSrc& mixerSrc,
                              const RouterMeterSrc& meterSrc) noexcept {
    std::ranges::fill(frame, uint8_t{0});
    for (size_t i = 0; i < 4; ++i) frame[i] = kOpcodeSetRouting[i];
    frame[17] = 1; // high-rate table marker (reference: frame[17] = 1)

    // To stream: 14 channels at high rate.
    for (uint32_t i = 0; i < 14; ++i)
        PutRoute(frame, 20 + i * 4, 1 + i, kDstStream + i);

    for (uint32_t i = 0; i < kOutputPairCount; ++i) {
        PutRoute(frame, 76 + i * 8, outSrc[i * 2],     kDstAnalog + 8 - i * 2);
        PutRoute(frame, 80 + i * 8, outSrc[1 + i * 2], kDstAnalog + 9 - i * 2);
    }
    PutRoute(frame, 116, outSrc[10], kDstSpdif);
    PutRoute(frame, 120, outSrc[11], kDstSpdif + 1);
    for (uint32_t i = 0; i < 4; ++i)
        PutRoute(frame, 124 + i * 4, outSrc[12 + i], kDstAdat + i);
    PutRoute(frame, 140, outSrc[20], kDstStream + 14);
    PutRoute(frame, 144, outSrc[21], kDstStream + 15);

    for (uint32_t i = 0; i < kChannelCount; ++i)
        PutRoute(frame, 148 + i * 4, mixerSrc[i], kDstMixer + i);

    PutRoute(frame, 220, meterSrc[0], 0x0);
    PutRoute(frame, 224, meterSrc[1], 0x0);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
