// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace ASFW::AudioModel {

template <typename Tag>
struct Id {
    uint32_t value{};

    constexpr auto operator<=>(const Id&) const = default;
};

using NodeId        = Id<struct NodeTag>;
using PortId        = Id<struct PortTag>;
using RouteBundleId = Id<struct RouteBundleTag>;
using CrosspointId  = Id<struct CrosspointTag>;
using ParameterId   = Id<struct ParameterTag>;
using MeterId       = Id<struct MeterTag>;

} // namespace ASFW::AudioModel

namespace std {

template <typename Tag>
struct hash<ASFW::AudioModel::Id<Tag>> {
    std::size_t operator()(const ASFW::AudioModel::Id<Tag>& id) const noexcept {
        return std::hash<uint32_t>{}(id.value);
    }
};

} // namespace std
