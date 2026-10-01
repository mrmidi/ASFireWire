// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceIdentity.hpp - Recognise a TCAT DICE unit from its Config ROM alone.
//
// A DICE unit directory carries the vendor's OUI as specifier_id and interface
// version 0x000001, and DICE firmware builds the GUID from fixed fields:
// GUID[63:40] OUI, [39:32] category, [31:22] product (= the unit's model id),
// [21:0] serial. A unit that only happens to use version 1 does not satisfy all
// of it. The rule and the per-vendor categories follow Linux
// sound/firewire/dice/dice.c:14-24 (OUIs), :26-29 (categories) and :33-71
// (check_dice_category); dice.c:458-461 applies it to every unit with that
// version that no explicit table row claimed.
//
// Known units that fail the rule and so need their own catalog row:
// - Saffire Pro 40 (TCD3070): product field 0x13, model id 0x0000de
//   (dice.c:385-394);
// - SSL Duende: non-standard GUID (dice.c:360-368).

#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::DeviceProfiles::Audio {

inline constexpr uint32_t kDiceInterfaceVersion = 0x000001;

inline constexpr uint32_t kDiceCategory = 0x04;
inline constexpr uint32_t kWeissDiceCategory = 0x00;
inline constexpr uint32_t kLoudDiceCategory = 0x10;
inline constexpr uint32_t kHarmanDiceCategory = 0x20;

inline constexpr uint32_t kWeissDiceOui = 0x001c6a;
inline constexpr uint32_t kLoudDiceOui = 0x000ff2;
inline constexpr uint32_t kHarmanDiceOui = 0x000fd7;

[[nodiscard]] constexpr uint32_t DiceCategoryFor(uint32_t oui) noexcept {
    switch (oui) {
        case kWeissDiceOui:
            return kWeissDiceCategory;
        case kLoudDiceOui:
            return kLoudDiceCategory;
        case kHarmanDiceOui:
            return kHarmanDiceCategory;
        default:
            return kDiceCategory;
    }
}

/// True when the unit's directory and the device GUID follow the DICE layout.
/// `specifierId`, `version` and `modelId` come from the unit directory.
[[nodiscard]] constexpr bool IsDiceIdentity(uint64_t guid,
                                            std::optional<uint32_t> specifierId,
                                            std::optional<uint32_t> version,
                                            std::optional<uint32_t> modelId) noexcept {
    if (!specifierId || !version || !modelId || *version != kDiceInterfaceVersion) {
        return false;
    }
    const auto guidHi = static_cast<uint32_t>(guid >> 32U);
    const auto guidLo = static_cast<uint32_t>(guid);
    return guidHi == ((*specifierId << 8U) | DiceCategoryFor(*specifierId)) &&
           (guidLo >> 22U) == *modelId;
}

} // namespace ASFW::DeviceProfiles::Audio
