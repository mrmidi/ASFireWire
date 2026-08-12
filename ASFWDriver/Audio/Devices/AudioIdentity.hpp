// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Discovery/RuntimeIdentity.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ASFW::Audio::Devices {

// Allocated by AudioDeviceSessionManager. Runtime-only and never used as a
// CoreAudio UID or a FireWire route.
struct AudioEndpointId final {
    uint64_t value{0};

    constexpr auto operator<=>(const AudioEndpointId&) const = default;
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
};

struct AudioEndpointIdHash final {
    [[nodiscard]] constexpr size_t operator()(AudioEndpointId id) const noexcept {
        return std::hash<uint64_t>{}(id.value);
    }
};

enum class PersistentIdentityStatus : uint8_t {
    Persistent,
    Ephemeral,
};

// Trust-qualified input to stable CoreAudio identity. It is deliberately a
// string recipe result, not an alias for observed EUI-64 or a runtime ID.
struct PersistentDeviceKey final {
    std::string value;
    bool trustworthy{false};

    [[nodiscard]] explicit operator bool() const noexcept {
        return trustworthy && !value.empty();
    }
};

struct EndpointAddress final {
    AudioEndpointId endpointId{};
    Discovery::UnitInstanceId unitId{};

    constexpr auto operator<=>(const EndpointAddress&) const = default;
};

} // namespace ASFW::Audio::Devices
