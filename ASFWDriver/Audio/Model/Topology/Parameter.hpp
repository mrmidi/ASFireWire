// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include "Id.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ASFW::AudioModel {

enum class ParameterSemantic : uint8_t {
    Unknown,

    Level,
    Mute,
    PhantomPower,
    PhaseInvert,
    Pan,
    Balance,
    Solo,
    NominalLevel,
    ClockSource,
    Dim,
};

using ParameterValue =
    std::variant<
        bool,
        double,
        int64_t
    >;

struct BooleanDomain {
    constexpr auto operator<=>(const BooleanDomain&) const = default;
};

enum class ScalarUnit : uint8_t {
    Generic,
    Decibels,
    Hertz,
    Percent,
    Normalized,
};

struct ScalarDomain {
    double min{};
    double max{};
    std::optional<double> step{};
    ScalarUnit unit{ScalarUnit::Generic};

    constexpr auto operator<=>(const ScalarDomain&) const = default;
};

struct EnumItem {
    int64_t value{};
    std::string name{};

    auto operator<=>(const EnumItem&) const = default;
};

struct EnumDomain {
    std::vector<EnumItem> values{};

    auto operator<=>(const EnumDomain&) const = default;
};

using ParameterDomain =
    std::variant<
        BooleanDomain,
        ScalarDomain,
        EnumDomain
    >;

using ParameterTarget =
    std::variant<
        NodeId,
        PortId,
        CrosspointId
    >;

struct Parameter {
    ParameterId id;
    ParameterTarget target;

    ParameterSemantic semantic;
    ParameterDomain domain;

    std::string name;
};

} // namespace ASFW::AudioModel
