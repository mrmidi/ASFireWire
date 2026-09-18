// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceControl.hpp - Typed key and value for protocol-backed CoreAudio controls.
//
// Shape follows documentation/AUDIO_BACKENDS_CONTROLS.md §5.1-5.3: one typed accessor keyed
// on (kind, class, scope, element) instead of three virtuals per control type, with the
// scope in the key so an input and an output 'vlme' on element 0 cannot collide.
//
// Only kLevel has a producer today. The boolean path keeps its own virtuals until FW-133
// decides whether it migrates here.

#pragma once

#include <cstdint>

namespace ASFW::Audio {

enum class ControlKind : uint32_t {
    kBoolean = 0,
    kLevel = 1,
    kSelector = 2,
};

/// ADK class and scope FourCCs (AudioDriverKitTypes.h), spelled out so the protocol layer
/// does not include AudioDriverKit.
inline constexpr uint32_t kControlClassVolume = 0x766C6D65;   // 'vlme'
inline constexpr uint32_t kControlScopeOutput = 0x6F757470;   // 'outp'
inline constexpr uint32_t kControlScopeInput = 0x696E7074;    // 'inpt'
inline constexpr uint32_t kControlElementMain = 0;

struct ControlKey final {
    ControlKind kind{ControlKind::kLevel};
    uint32_t classIdFourCC{0};
    uint32_t scopeFourCC{0};
    uint32_t element{0};

    friend constexpr bool operator==(const ControlKey&, const ControlKey&) = default;
};

/// The master output volume: what the volume keys and the Sound settings slider drive.
inline constexpr ControlKey kMasterOutputVolumeKey{ControlKind::kLevel, kControlClassVolume,
                                                   kControlScopeOutput, kControlElementMain};

struct ControlValue final {
    ControlKind kind{ControlKind::kLevel};
    bool boolean{false};
    float decibels{0.0f};
    uint32_t selector{0};
};

/// What a protocol reports about a control it backs. The range is fixed for the control's
/// lifetime: IOUserAudioLevelControl has no SetRange (design note §1).
struct ControlInfo final {
    bool isSettable{false};
    float minDecibels{0.0f};
    float maxDecibels{0.0f};
};

/// The nub/driver seam is IIG dispatch, which carries integers only. Level values cross it
/// as float bit patterns.
[[nodiscard]] constexpr uint32_t DecibelBits(float decibels) noexcept {
    return __builtin_bit_cast(uint32_t, decibels);
}

[[nodiscard]] constexpr float DecibelsFromBits(uint32_t bits) noexcept {
    return __builtin_bit_cast(float, bits);
}

} // namespace ASFW::Audio
