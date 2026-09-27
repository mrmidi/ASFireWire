// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OperandPack.hpp - Pack and unpack fixed byte layouts with C++26 structured
// binding packs (P1061) and pack indexing (P2662).
//
// Many AV/C operand blocks are a fixed run of bytes: PLUG INFO counts, a plug
// signal format, a plug address. Describe one as an aggregate whose fields are
// uint8_t or std::array<uint8_t, N>, in wire order, and these helpers read and
// write it with no hand-written offsets:
//
//   struct PlugSignalFormatBytes { uint8_t plugId; uint8_t fmt; std::array<uint8_t, 3> fdf; };
//   auto bytes = Pack::ToBytes(PlugSignalFormatBytes{0, 0x90, {0x02, 0xFF, 0xFF}});
//   auto back  = Pack::FromBytes<PlugSignalFormatBytes>(bytes);
//
// Header-only and fully constexpr; the static_asserts at the bottom test it at
// compile time. Needs -std=c++26 (Apple Clang 21, Xcode 27: verified 2026-09-27
// for host and DriverKit arm64e).

#pragma once

#include "AvcError.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ASFW::AVC::Pack {

namespace detail {

template <class F>
struct FieldBytes : std::integral_constant<size_t, 0> {};

template <>
struct FieldBytes<uint8_t> : std::integral_constant<size_t, 1> {};

template <size_t N>
struct FieldBytes<std::array<uint8_t, N>> : std::integral_constant<size_t, N> {};

template <class F>
inline constexpr size_t kFieldBytes = FieldBytes<std::remove_cvref_t<F>>::value;

template <class T>
consteval bool AllFieldsAreBytes() {
    T value{};
    auto& [... fields] = value;
    return ((kFieldBytes<decltype(fields)> > 0) && ...);
}

template <class T>
consteval size_t PackedSize() {
    T value{};
    auto& [... fields] = value;
    return (kFieldBytes<decltype(fields)> + ... + size_t{0});
}

} // namespace detail

/// An aggregate made only of uint8_t and std::array<uint8_t, N> fields, in wire order.
template <class T>
concept ByteLayout = std::is_aggregate_v<T> && std::is_default_constructible_v<T> &&
                     detail::AllFieldsAreBytes<T>();

template <ByteLayout T>
inline constexpr size_t kSize = detail::PackedSize<T>();

template <ByteLayout T>
[[nodiscard]] constexpr std::array<uint8_t, kSize<T>> ToBytes(const T& value) noexcept {
    std::array<uint8_t, kSize<T>> out{};
    size_t pos = 0;
    const auto& [... fields] = value;
    const auto put = [&](const auto& field) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, uint8_t>) {
            out[pos++] = field;
        } else {
            for (const uint8_t byte : field) {
                out[pos++] = byte;
            }
        }
    };
    (put(fields), ...);
    return out;
}

/// Read a T from the front of `bytes`. Extra trailing bytes are ignored (responses
/// may carry padding); too few give kOperandsTooShort with the available count.
template <ByteLayout T>
[[nodiscard]] constexpr Expected<T> FromBytes(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < kSize<T>) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(bytes.size()));
    }
    T value{};
    size_t pos = 0;
    auto& [... fields] = value;
    const auto take = [&](auto& field) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, uint8_t>) {
            field = bytes[pos++];
        } else {
            for (uint8_t& byte : field) {
                byte = bytes[pos++];
            }
        }
    };
    (take(fields), ...);
    return value;
}

namespace selftest {
struct Probe {
    uint8_t a;
    std::array<uint8_t, 3> b;
    uint8_t c;
};
struct NotBytes {
    uint8_t a;
    uint16_t b;
};
static_assert(ByteLayout<Probe>);
static_assert(!ByteLayout<NotBytes>);
static_assert(kSize<Probe> == 5);
static_assert(ToBytes(Probe{1, {2, 3, 4}, 5}) == std::array<uint8_t, 5>{1, 2, 3, 4, 5});
static_assert([] {
    constexpr std::array<uint8_t, 6> raw{9, 8, 7, 6, 5, 0xEE};  // trailing byte ignored
    const auto probe = FromBytes<Probe>(raw);
    return probe && probe->a == 9 && probe->b == std::array<uint8_t, 3>{8, 7, 6} && probe->c == 5;
}());
static_assert([] {
    constexpr std::array<uint8_t, 2> raw{1, 2};
    const auto probe = FromBytes<Probe>(raw);
    return !probe && probe.error().kind == AvcErrorKind::kOperandsTooShort &&
           probe.error().operandOffset == 2;
}());
} // namespace selftest

} // namespace ASFW::AVC::Pack
