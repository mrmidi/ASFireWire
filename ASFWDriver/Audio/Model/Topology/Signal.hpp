// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <compare>
#include <cstdint>

namespace ASFW::AudioModel {

/// What a connector or wire slot physically is.
///
/// This is a *connector* taxonomy, not a behaviour one. Behaviour is already
/// carried by the parameters attached to a port: on the Duet the XLR ports own
/// PhantomPower and NominalLevel while the instrument ports own nothing, so a
/// SignalKind that also encoded "has phantom" would be a second, drifting copy
/// of that truth.
///
/// A member is added when a device exposes a physically distinct connector or
/// transport a person must be able to tell apart -- never because a vendor
/// spells an existing one differently.
enum class SignalKind : uint8_t {
    Unknown,

    AnalogLine,
    AnalogMicXlr,
    AnalogInstrument,
    Headphone,

    SpdifCoaxial,
    SpdifOptical,
    Adat,

    HostStream,
};

/// Canonical identity of an endpoint port.
///
/// `index` is the 1-based number of the *first channel* this port carries within
/// its (kind, direction) group, so a port with `channels == 2` and `index == 3`
/// renders as ".. 3/4".
struct SignalIdentity {
    SignalKind kind{SignalKind::Unknown};
    uint32_t index{1};

    constexpr auto operator<=>(const SignalIdentity&) const = default;
};

} // namespace ASFW::AudioModel
