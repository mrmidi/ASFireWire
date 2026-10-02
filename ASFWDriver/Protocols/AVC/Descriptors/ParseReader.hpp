// SPDX-License-Identifier: Apache-2.0
//
// ParseReader.hpp - Bounded, constexpr reader for AV/C descriptor bytes.
//
// Every read is checked against the bytes actually present and reports the
// absolute offset of a failure. Device bytes never reach an assertion or an
// unreachable branch. Descriptor fields are big-endian (TA 2002013 §5).

#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numeric>
#include <span>
#include <type_traits>

namespace ASFW::Protocols::AVC::Descriptors {
enum class ParseErrorKind : uint8_t { Truncated, InvalidLength, InvalidValue, BudgetExceeded };
struct ParseError {
    size_t offset;
    ParseErrorKind kind;
    friend constexpr bool operator==(const ParseError&, const ParseError&) = default;
};
template<class T> using Parsed = std::expected<T, ParseError>;
inline constexpr size_t kMaxDescriptorBytes = 4096;
inline constexpr size_t kMaxInfoBlockDepth = 32;
inline constexpr size_t kMaxTextListNodes = 32;
inline constexpr size_t kMaxTextListDepth = 32;

/// A big-endian field of the descriptor's wire layout, as a host value.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T FromBigEndian(T wire) noexcept {
    if constexpr (std::endian::native == std::endian::little && sizeof(T) > 1) return std::byteswap(wire);
    else return wire;
}

/// Views are only valid while the source bytes are owned by the caller.
class ParseReader {
public:
    constexpr explicit ParseReader(std::span<const uint8_t> bytes, size_t base = 0)
        : bytes_(bytes), base_(base) {}
    /// Absolute offset. Saturating, so no length can wrap it into a small value.
    [[nodiscard]] constexpr size_t Offset() const { return std::add_sat(base_, offset_); }
    [[nodiscard]] constexpr size_t Remaining() const { return bytes_.size() - offset_; }
    [[nodiscard]] constexpr Parsed<std::span<const uint8_t>> Take(size_t count) {
        if (count > Remaining()) return std::unexpected(ParseError{Offset(), ParseErrorKind::Truncated});
        auto result = bytes_.subspan(offset_, count); offset_ += count; return result;
    }
    /// One unsigned big-endian field.
    template <std::unsigned_integral T>
    [[nodiscard]] constexpr Parsed<T> Field() {
        return Take(sizeof(T)).transform([](std::span<const uint8_t> bytes) {
            std::array<uint8_t, sizeof(T)> wire{};
            for (size_t i = 0; i < sizeof(T); ++i) wire[i] = bytes[i];
            return FromBigEndian(std::bit_cast<T>(wire)); // Wire bytes in memory order.
        });
    }
    [[nodiscard]] constexpr Parsed<uint8_t> U8() { return Field<uint8_t>(); }
    [[nodiscard]] constexpr Parsed<uint16_t> BE16() { return Field<uint16_t>(); }
    [[nodiscard]] constexpr Parsed<uint32_t> BE32() { return Field<uint32_t>(); }

    /// Read consecutive fields into `out...` in order, each by its own width.
    /// Stops at the first failure: and_then never runs past an error.
    template <std::unsigned_integral... T>
    [[nodiscard]] constexpr Parsed<void> Fields(T&... out) {
        Parsed<void> result{};
        ((result = result.and_then([this, &out]() -> Parsed<void> {
             return Field<T>().transform([&out](T value) { out = value; });
         })), ...);
        return result;
    }

    /// A 16-bit length followed by that many bytes, as its own reader.
    [[nodiscard]] constexpr Parsed<ParseReader> Section() {
        return BE16().and_then([this](uint16_t length) {
            const auto base = Offset();
            return Take(length).transform([base](auto bytes) { return ParseReader(bytes, base); });
        });
    }
    [[nodiscard]] constexpr Parsed<void> End() const {
        if (Remaining()) return std::unexpected(ParseError{Offset(), ParseErrorKind::InvalidLength});
        return {};
    }
private:
    std::span<const uint8_t> bytes_;
    size_t base_, offset_{0};
};

/// The body of a whole descriptor: its 16-bit length must cover exactly the
/// remaining bytes (TA 2002013 §5: descriptor_length excludes itself).
[[nodiscard]] constexpr Parsed<ParseReader> DescriptorBody(std::span<const uint8_t> bytes) {
    if (bytes.size() > kMaxDescriptorBytes)
        return std::unexpected(ParseError{0, ParseErrorKind::BudgetExceeded});
    ParseReader reader(bytes);
    return reader.Section().and_then([&reader](ParseReader body) {
        return reader.End().transform([&body] { return body; });
    });
}

namespace ParseReaderChecks {
constexpr bool BoundsAreChecked() {
    constexpr uint8_t bytes[]{0x01, 0x02};
    ParseReader reader(bytes);
    auto word = reader.BE16(); auto missing = reader.U8();
    return word && *word == 0x0102 && !missing && missing.error().offset == 2;
}
constexpr bool FieldsStopAtTheFirstFailure() {
    constexpr uint8_t bytes[]{0xAB, 0x12, 0x34, 0x56};
    ParseReader reader(bytes);
    uint8_t a{}; uint16_t b{}; uint16_t c{0x7777};
    const auto result = reader.Fields(a, b, c);
    return !result && result.error() == ParseError{3, ParseErrorKind::Truncated} &&
           a == 0xAB && b == 0x1234 && c == 0x7777;
}
constexpr bool LengthMustCoverTheBody() {
    constexpr uint8_t exact[]{0x00, 0x01, 0xFF};
    constexpr uint8_t trailing[]{0x00, 0x01, 0xFF, 0x00};
    return DescriptorBody(exact).has_value() && !DescriptorBody(trailing).has_value();
}
static_assert(BoundsAreChecked());
static_assert(FieldsStopAtTheFirstFailure());
static_assert(LengthMustCoverTheBody());
static_assert(FromBigEndian<uint16_t>(std::bit_cast<uint16_t>(std::array<uint8_t, 2>{0x12, 0x34})) == 0x1234);
} // namespace ParseReaderChecks
} // namespace ASFW::Protocols::AVC::Descriptors
