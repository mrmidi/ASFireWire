#pragma once

#include <cstdint>

#include "ConfigROMConstants.hpp"

namespace ASFW::ConfigROM {

struct ByteOffset {
    uint32_t value{0};
};

struct QuadletOffset {
    uint32_t value{0};
};

struct QuadletCount {
    uint32_t value{0};
};

[[nodiscard]] constexpr ByteOffset ToBytes(QuadletOffset offset) noexcept {
    return ByteOffset{.value = offset.value * ASFW::ConfigROM::kQuadletBytes};
}

[[nodiscard]] constexpr ByteOffset ToBytes(QuadletCount count) noexcept {
    return ByteOffset{.value = count.value * ASFW::ConfigROM::kQuadletBytes};
}

[[nodiscard]] constexpr QuadletOffset ToQuadlets(ByteOffset offset) noexcept {
#if !defined(NDEBUG)
    if ((offset.value % ASFW::ConfigROM::kQuadletBytes) != 0) {
        __builtin_trap();
    }
#endif
    return QuadletOffset{.value = offset.value / ASFW::ConfigROM::kQuadletBytes};
}

[[nodiscard]] constexpr QuadletOffset operator+(QuadletOffset base, QuadletCount delta) noexcept {
    return QuadletOffset{.value = base.value + delta.value};
}

[[nodiscard]] constexpr QuadletCount operator-(QuadletOffset end, QuadletOffset begin) noexcept {
#if !defined(NDEBUG)
    if (end.value < begin.value) {
        __builtin_trap();
    }
#endif
    return QuadletCount{.value = end.value - begin.value};
}

[[nodiscard]] constexpr bool operator>=(QuadletCount a, QuadletCount b) noexcept {
    return a.value >= b.value;
}

[[nodiscard]] constexpr bool operator>(QuadletCount a, QuadletCount b) noexcept {
    return a.value > b.value;
}

[[nodiscard]] constexpr bool operator<=(QuadletCount a, QuadletCount b) noexcept {
    return a.value <= b.value;
}

[[nodiscard]] constexpr bool operator<(QuadletCount a, QuadletCount b) noexcept {
    return a.value < b.value;
}

} // namespace ASFW::ConfigROM
