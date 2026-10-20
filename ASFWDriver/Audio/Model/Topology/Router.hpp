// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"

#include <compare>
#include <optional>
#include <vector>

namespace ASFW::AudioModel {

struct Route {
    PortId input;
    PortId output;

    constexpr auto operator<=>(const Route&) const = default;
};

struct RouteBundle {
    RouteBundleId id;
    std::vector<Route> routes;

    auto operator<=>(const RouteBundle&) const = default;
};

struct RouterConstraints {
    std::optional<uint32_t> maxActiveBundles;
    std::optional<uint32_t> maxActiveRoutes;
    std::optional<uint32_t> maxSourcesPerOutput;
    std::optional<uint32_t> maxDestinationsPerInput;

    constexpr auto operator<=>(const RouterConstraints&) const = default;
};

} // namespace ASFW::AudioModel
