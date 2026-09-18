// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RuntimeIdentity.hpp
//
// Strong, non-persistent identifiers used to address objects inside one driver
// lifetime.  IEEE 1394 EUI-64 values are observations, not runtime keys: some
// shipping devices report zero or duplicate EUI-64 values.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ASFW::Discovery {

struct DeviceInstanceId final {
    uint64_t value{0};

    constexpr auto operator<=>(const DeviceInstanceId&) const = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
};

struct UnitInstanceId final {
    DeviceInstanceId device{};
    uint32_t unitDirectoryOffset{0};

    constexpr auto operator<=>(const UnitInstanceId&) const = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(device);
    }
};

struct DeviceInstanceIdHash final {
    [[nodiscard]] constexpr size_t operator()(DeviceInstanceId id) const noexcept {
        return std::hash<uint64_t>{}(id.value);
    }
};

struct UnitInstanceIdHash final {
    [[nodiscard]] constexpr size_t operator()(const UnitInstanceId& id) const noexcept {
        const size_t deviceHash = DeviceInstanceIdHash{}(id.device);
        const size_t offsetHash = std::hash<uint32_t>{}(id.unitDirectoryOffset);
        return deviceHash ^ (offsetHash + 0x9e3779b9U + (deviceHash << 6U) +
                             (deviceHash >> 2U));
    }
};

} // namespace ASFW::Discovery
