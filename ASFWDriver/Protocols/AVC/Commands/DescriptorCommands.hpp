// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DescriptorCommands.hpp - AV/C Descriptor Mechanism commands (OPEN, READ, CLOSE)
// Specification: TA Document 2002013 - AV/C Descriptor Mechanism 1.2
// References: Apple IOFireWireFamily (IOFireWireAVCLib), FFADO libavc/descriptors
//

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcTypes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC::Cmd {

//==============================================================================
// Descriptor Specifier (TA 2002013 §6.1, §6.2)
//==============================================================================

/// Represents a variable-length descriptor specifier in inline storage.
struct DescriptorSpecifier {
    static constexpr size_t kMaxBytes = 8;
    std::array<uint8_t, kMaxBytes> bytes{};
    uint8_t length{0};

    [[nodiscard]] constexpr std::span<const uint8_t> Bytes() const noexcept {
        return {bytes.data(), length};
    }

    /// 6.2.1 (Sub)unit identifier descriptor specifier: [0x00]
    [[nodiscard]] static constexpr DescriptorSpecifier SubunitIdentifier() noexcept {
        return DescriptorSpecifier{.bytes = {0x00}, .length = 1};
    }

    /// Subunit status descriptor specifier: [0x80]
    [[nodiscard]] static constexpr DescriptorSpecifier SubunitStatus() noexcept {
        return DescriptorSpecifier{.bytes = {0x80}, .length = 1};
    }

    /// 6.2.2 List descriptor specified by list_ID: [0x10, id_hi, id_lo]
    [[nodiscard]] static constexpr DescriptorSpecifier ListById(uint16_t listId) noexcept {
        return DescriptorSpecifier{
            .bytes = {0x10, static_cast<uint8_t>(listId >> 8), static_cast<uint8_t>(listId & 0xFF)},
            .length = 3
        };
    }

    /// 6.2.3 List descriptor specified by list_type: [0x11, list_type]
    [[nodiscard]] static constexpr DescriptorSpecifier ListByType(uint8_t listType) noexcept {
        return DescriptorSpecifier{.bytes = {0x11, listType}, .length = 2};
    }

    /// Raw specifier bytes factory
    [[nodiscard]] static constexpr DescriptorSpecifier Raw(std::span<const uint8_t> raw) noexcept {
        DescriptorSpecifier spec{};
        spec.length = static_cast<uint8_t>(std::min(raw.size(), kMaxBytes));
        std::copy_n(raw.begin(), spec.length, spec.bytes.begin());
        return spec;
    }

    [[nodiscard]] constexpr bool operator==(const DescriptorSpecifier& other) const noexcept {
        if (length != other.length) return false;
        return std::equal(bytes.begin(), bytes.begin() + length, other.bytes.begin());
    }
};

//==============================================================================
// OPEN DESCRIPTOR Command (0x08)
// TA 2002013 §7.1 Table 28 & 29
//==============================================================================

enum class OpenDescriptorSubfunction : uint8_t {
    kClose     = 0x00,  ///< Close descriptor session
    kReadOpen  = 0x01,  ///< Open for read-only access
    kWriteOpen = 0x03,  ///< Open for read/write access
};

struct OpenDescriptorReply {
    OpenDescriptorSubfunction subfunction{OpenDescriptorSubfunction::kClose};
    uint8_t status{0};
};

struct OpenDescriptorOperands {
    static constexpr Opcode kOpcode = Opcode::kOpenDescriptor;
    using Reply = OpenDescriptorReply;

    DescriptorSpecifier specifier{DescriptorSpecifier::SubunitStatus()};
    OpenDescriptorSubfunction subfunction{OpenDescriptorSubfunction::kReadOpen};

    [[nodiscard]] constexpr Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kControl) {
            return Fail(AvcErrorKind::kUnsupported);
        }
        auto res = w.Append(specifier.Bytes());
        if (!res) return res;
        res = w.Append(static_cast<uint8_t>(subfunction));
        if (!res) return res;
        // read_write_result = 0xFF for command
        return w.Append(0xFF);
    }

    [[nodiscard]] constexpr Expected<Reply> Read(std::span<const uint8_t> in) const noexcept {
        const size_t specLen = specifier.length;
        if (in.size() < specLen + 2) {
            return Fail(AvcErrorKind::kOperandsTooShort);
        }
        if (!std::equal(specifier.Bytes().begin(), specifier.Bytes().end(), in.begin())) {
            return Fail(AvcErrorKind::kMalformedOperands);
        }
        return Reply{
            .subfunction = static_cast<OpenDescriptorSubfunction>(in[specLen]),
            .status = in[specLen + 1],
        };
    }
};

using OpenDescriptorCommand = Command<OpenDescriptorOperands>;

//==============================================================================
// READ DESCRIPTOR Command (0x09)
// TA 2002013 §7.5 Table 35 & 36
//==============================================================================

enum class ReadResultStatus : uint8_t {
    kComplete           = 0x10,  ///< Complete read: entire requested data returned
    kMoreToRead         = 0x11,  ///< Portion returned; more remaining in descriptor
    kDataLengthTooLarge = 0x12,  ///< Less data exists than requested
};

struct ReadDescriptorReply {
    ReadResultStatus status{ReadResultStatus::kComplete};
    uint16_t reportedLength{0};
    uint16_t reportedOffset{0};
    std::span<const uint8_t> data{};
};

struct ReadDescriptorOperands {
    static constexpr Opcode kOpcode = Opcode::kReadDescriptor;
    using Reply = ReadDescriptorReply;

    DescriptorSpecifier specifier{DescriptorSpecifier::SubunitStatus()};
    uint16_t offset{0};
    uint16_t length{0};

    [[nodiscard]] constexpr Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kControl) {
            return Fail(AvcErrorKind::kUnsupported);
        }
        auto res = w.Append(specifier.Bytes());
        if (!res) return res;
        const uint8_t params[] = {
            0xFF, // read_result_status = 0xFF
            0x00, // reserved = 0x00 per TA 2002013 Table 35
            static_cast<uint8_t>(length >> 8),
            static_cast<uint8_t>(length & 0xFF),
            static_cast<uint8_t>(offset >> 8),
            static_cast<uint8_t>(offset & 0xFF),
        };
        return w.Append(params);
    }

    [[nodiscard]] constexpr Expected<Reply> Read(std::span<const uint8_t> in) const noexcept {
        const size_t specLen = specifier.length;
        const size_t headerLen = specLen + 6;
        if (in.size() < headerLen) {
            return Fail(AvcErrorKind::kOperandsTooShort);
        }
        if (!std::equal(specifier.Bytes().begin(), specifier.Bytes().end(), in.begin())) {
            return Fail(AvcErrorKind::kMalformedOperands);
        }
        const auto status = static_cast<ReadResultStatus>(in[specLen]);
        const uint16_t repLen = (static_cast<uint16_t>(in[specLen + 2]) << 8) | in[specLen + 3];
        const uint16_t repOff = (static_cast<uint16_t>(in[specLen + 4]) << 8) | in[specLen + 5];

        const std::span<const uint8_t> payload = in.subspan(headerLen);
        const size_t actualDataLen = std::min(static_cast<size_t>(repLen), payload.size());

        return Reply{
            .status = status,
            .reportedLength = repLen,
            .reportedOffset = repOff,
            .data = payload.subspan(0, actualDataLen),
        };
    }
};

using ReadDescriptorCommand = Command<ReadDescriptorOperands>;

static_assert(AvcOperands<OpenDescriptorOperands>);
static_assert(AvcOperands<ReadDescriptorOperands>);

} // namespace ASFW::AVC::Cmd
