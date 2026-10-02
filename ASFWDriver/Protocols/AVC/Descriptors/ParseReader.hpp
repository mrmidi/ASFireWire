// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

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

/// Views are only valid while the source bytes are owned by the caller.
class ParseReader {
public:
    constexpr explicit ParseReader(std::span<const uint8_t> bytes, size_t base = 0)
        : bytes_(bytes), base_(base) {}
    [[nodiscard]] constexpr size_t Offset() const { return base_ + offset_; }
    [[nodiscard]] constexpr size_t Remaining() const { return bytes_.size() - offset_; }
    [[nodiscard]] constexpr Parsed<std::span<const uint8_t>> Take(size_t count) {
        if (count > Remaining()) return std::unexpected(ParseError{Offset(), ParseErrorKind::Truncated});
        auto result = bytes_.subspan(offset_, count); offset_ += count; return result;
    }
    [[nodiscard]] constexpr Parsed<uint8_t> U8() {
        return Take(1).transform([](auto bytes) { return bytes[0]; });
    }
    [[nodiscard]] constexpr Parsed<uint16_t> BE16() {
        return Take(2).transform([](auto bytes) {
            return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
        });
    }
    [[nodiscard]] constexpr Parsed<ParseReader> Section() {
        auto length = BE16(); if (!length) return std::unexpected(length.error());
        const auto base = Offset();
        return Take(*length).transform([base](auto bytes) { return ParseReader(bytes, base); });
    }
    [[nodiscard]] constexpr Parsed<void> End() const {
        if (Remaining()) return std::unexpected(ParseError{Offset(), ParseErrorKind::InvalidLength});
        return {};
    }
private:
    std::span<const uint8_t> bytes_;
    size_t base_, offset_{0};
};
[[nodiscard]] constexpr Parsed<ParseReader> DescriptorBody(std::span<const uint8_t> bytes) {
    if (bytes.size() > kMaxDescriptorBytes)
        return std::unexpected(ParseError{0, ParseErrorKind::BudgetExceeded});
    ParseReader reader(bytes);
    auto body = reader.Section(); if (!body) return std::unexpected(body.error());
    auto end = reader.End(); if (!end) return std::unexpected(end.error());
    return body;
}
constexpr bool ReaderChecksBounds() {
    constexpr uint8_t bytes[]{0x01, 0x02};
    ParseReader reader(bytes);
    auto word = reader.BE16(); auto missing = reader.U8();
    return word && *word == 0x0102 && !missing && missing.error().offset == 2;
}
static_assert(ReaderChecksBounds());
} // namespace ASFW::Protocols::AVC::Descriptors
