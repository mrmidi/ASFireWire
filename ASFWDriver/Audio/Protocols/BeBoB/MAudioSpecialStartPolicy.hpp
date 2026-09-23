// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::BeBoB {

/// Linux calls the host-to-device playback stream `rx_stream` and starts it
/// before the device-to-host capture stream `tx_stream` (bebob_stream.c:411-438,
/// 623-644). In ASFW's host perspective that is IT before IR.
enum class MAudioHostStartDirection : uint8_t {
    Transmit,
    Receive,
};
inline constexpr std::array<MAudioHostStartDirection, 2> kMAudioHostStartOrder{{
    MAudioHostStartDirection::Transmit,
    MAudioHostStartDirection::Receive,
}};

/// Linux's post-domain-start rate reassertion for the 1814/ProjectMix special
/// firmware. The two directions are deliberately ordered: setting the input
/// plug immediately after the output plug can fail. This data is pure policy;
/// a caller must run it only after both streams are started.
enum class MAudioPostStartAction : uint8_t {
    SetOutputSignalFormat,
    SetInputSignalFormat,
};

struct MAudioPostStartStep final {
    MAudioPostStartAction action{};
    uint32_t delayBeforeMs{0};
};

// bebob_maudio.c:314-338, special_set_rate(): output first, 100 ms pause,
// then input. Stream readiness is checked after this by Linux at :663-664.
inline constexpr uint32_t kMAudioPostStartInputDelayMs = 100;
inline constexpr uint32_t kMAudioStreamReadyTimeoutMs = 4000;
inline constexpr std::array<MAudioPostStartStep, 2> kMAudioPostStartPlan{{
    {MAudioPostStartAction::SetOutputSignalFormat, 0},
    {MAudioPostStartAction::SetInputSignalFormat, kMAudioPostStartInputDelayMs},
}};

} // namespace ASFW::Audio::BeBoB
