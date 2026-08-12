// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Discovery/DiscoveryTypes.hpp"

#include <DriverKit/IOLib.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ASFW::DeviceProfiles {

struct MaskedValue32 final {
    uint32_t value{0};
    uint32_t mask{0xFFFF'FFFFU};

    [[nodiscard]] constexpr bool Matches(uint32_t candidate) const noexcept {
        return (candidate & mask) == (value & mask);
    }
};

struct MaskedValue64 final {
    uint64_t value{0};
    uint64_t mask{0xFFFF'FFFF'FFFF'FFFFULL};

    [[nodiscard]] constexpr bool Matches(uint64_t candidate) const noexcept {
        return (candidate & mask) == (value & mask);
    }
};

struct BusInfoWordMatch final {
    size_t index{0};
    MaskedValue32 value{};
};

// One declarative AND clause. Definitions and safety rules OR their clauses.
// Missing evidence never satisfies a constrained field. Unit specifier/version
// are first-class because RME Fireface variants share root model 0x101800:
// references/linux-sound-firewire-stack/firewire/fireface/ff.c:178-239.
// Descriptor comparisons stay exact and case-sensitive; ALSA uses a model name
// to split a shared Saffire model ID:
// references/alsa-userspace-control-protocols-impl/runtime/bebob/src/model.rs:69-84.
struct IdentityMatchClause final {
    std::optional<MaskedValue64> observedGuid;
    std::optional<BusInfoWordMatch> busInfoWord;
    std::optional<MaskedValue32> nodeVendorOui;
    std::optional<MaskedValue32> rootVendorId;
    std::optional<MaskedValue32> rootModelId;
    std::optional<MaskedValue32> unitVendorId;
    std::optional<MaskedValue32> unitModelId;
    std::optional<MaskedValue32> unitSpecifierId;
    std::optional<MaskedValue32> unitVersion;
    std::optional<std::string_view> rootVendorName;
    std::optional<std::string_view> rootModelName;
    std::optional<std::string_view> unitVendorName;
    std::optional<std::string_view> unitModelName;

    [[nodiscard]] bool Matches(const Discovery::DeviceIdentityEvidence& device,
                               const Discovery::UnitIdentityEvidence& unit) const noexcept {
        if (observedGuid && !observedGuid->Matches(device.observedGuid)) return false;
        if (nodeVendorOui && !nodeVendorOui->Matches(device.nodeVendorOui)) return false;
        if (rootVendorId &&
            (!device.rootVendorId || !rootVendorId->Matches(*device.rootVendorId))) return false;
        if (rootModelId &&
            (!device.rootModelId || !rootModelId->Matches(*device.rootModelId))) return false;
        if (unitVendorId && (!unit.vendorId || !unitVendorId->Matches(*unit.vendorId))) return false;
        if (unitModelId && (!unit.modelId || !unitModelId->Matches(*unit.modelId))) return false;
        if (unitSpecifierId &&
            (!unit.specifierId || !unitSpecifierId->Matches(*unit.specifierId))) return false;
        if (unitVersion && (!unit.version || !unitVersion->Matches(*unit.version))) return false;

        if (busInfoWord) {
            if (busInfoWord->index >= device.rawBusInfoQuadlets.size()) return false;
            const uint32_t hostWord =
                OSSwapBigToHostInt32(device.rawBusInfoQuadlets[busInfoWord->index]);
            if (!busInfoWord->value.Matches(hostWord)) return false;
        }

        if (rootVendorName && device.rootVendorName != *rootVendorName) return false;
        if (rootModelName && device.rootModelName != *rootModelName) return false;
        if (unitVendorName && (!unit.vendorName || *unit.vendorName != *unitVendorName)) return false;
        if (unitModelName && (!unit.modelName || *unit.modelName != *unitModelName)) return false;
        return true;
    }
};

} // namespace ASFW::DeviceProfiles
