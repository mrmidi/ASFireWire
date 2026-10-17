// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Format.hpp - "{}" substitution for diagnostic strings. The dext's libc++ is
// built without exceptions and no driver code uses <format>; this covers the
// only form the topology messages need: positional "{}" with integers,
// floating-point values and strings.
#pragma once

#include <cstdint>

#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace ASFW::AudioModel::detail {

template <typename T>
void AppendValue(std::string& out, const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        out += value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        out += std::to_string(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::integral<T>) {
        out += std::to_string(value);
    } else if constexpr (std::floating_point<T>) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
        out += buffer;
    } else {
        out += std::string_view(value);
    }
}

/// Replaces each "{}" in `pattern`, left to right, with the next argument.
/// Surplus placeholders stay literal; surplus arguments are ignored.
template <typename... Args>
[[nodiscard]] std::string Format(std::string_view pattern, const Args&... args) {
    std::string out;
    out.reserve(pattern.size() + 16 * sizeof...(Args));
    size_t cursor = 0;
    const auto next = [&](const auto& value) {
        const size_t hole = pattern.find("{}", cursor);
        if (hole == std::string_view::npos) return;
        out.append(pattern.substr(cursor, hole - cursor));
        AppendValue(out, value);
        cursor = hole + 2;
    };
    (next(args), ...);
    out.append(pattern.substr(cursor));
    return out;
}

} // namespace ASFW::AudioModel::detail
