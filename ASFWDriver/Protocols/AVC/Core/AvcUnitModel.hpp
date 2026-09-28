// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcUnitModel.hpp - Plain-data model representing an AV/C unit.
//
// Conforms to Phase 2c specification (docs/avc-rebuild/phase-2.md §2c):
// - Plain data: identity, info, unit/subunit plug counts, subunit list.
// - Directly stores Cmd::UnitPlugCounts, Cmd::UnitAsyncPlugCounts, and
//   Cmd::SubunitPlugCounts without parallel structs or duplicate count fields.
// - Subunit contents (music plugs, clusters, function blocks) are deferred to Phase 4.

#pragma once

#include "AvcTypes.hpp"
#include "IAvcUnit.hpp"
#include "../Commands/GeneralCommands.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::AVC {

/// Subunit identifier consisting of subunit type and numeric ID (0..7).
struct SubunitId {
    SubunitType type{SubunitType::kUnit};
    uint8_t id{0};

    [[nodiscard]] constexpr SubunitAddress ToAddress() const noexcept {
        return SubunitAddress::Of(type, id);
    }

    friend constexpr bool operator==(const SubunitId&, const SubunitId&) noexcept = default;
};

/// High-level model of a discovered subunit and its plug counts.
struct SubunitModel {
    SubunitId id{};
    Cmd::SubunitPlugCounts plugs{};

    friend constexpr bool operator==(const SubunitModel&, const SubunitModel&) noexcept = default;
};

/// Plain-data model of an AV/C Unit on the bus.
struct UnitModel {
    AvcUnitIdentity identity{};
    Cmd::UnitInfo info{};
    Cmd::UnitPlugCounts unitPlugs{};
    Cmd::UnitAsyncPlugCounts unitAsyncPlugs{};
    std::vector<SubunitModel> subunits{};

    [[nodiscard]] bool HasSubunit(SubunitType type) const noexcept {
        return std::any_of(subunits.begin(), subunits.end(),
                           [type](const auto& s) { return s.id.type == type; });
    }

    [[nodiscard]] uint8_t SubunitCount(SubunitType type) const noexcept {
        return static_cast<uint8_t>(std::count_if(
            subunits.begin(), subunits.end(),
            [type](const auto& s) { return s.id.type == type; }));
    }

    [[nodiscard]] std::optional<SubunitModel> FindSubunit(SubunitType type, uint8_t id) const noexcept {
        auto it = std::find_if(subunits.begin(), subunits.end(),
                               [type, id](const auto& s) { return s.id.type == type && s.id.id == id; });
        if (it != subunits.end()) {
            return *it;
        }
        return std::nullopt;
    }
};

} // namespace ASFW::AVC
