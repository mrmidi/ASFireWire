// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// VendorFrame.hpp - Generic VENDOR-DEPENDENT payload handler for vendor extensions.
//
// Conforms to Phase 2b checklist rule 6:
// "Vendor codecs are payloads on the one generic VENDOR-DEPENDENT command".
// Handles company ID validation and payload extraction while preserving quirks
// (e.g. TASCAM echoing FF FF FF and returning IMPLEMENTED/STABLE).

#pragma once

#include "../Commands/GeneralCommands.hpp"
#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <span>

namespace ASFW::AVC::Extensions {

template <typename Spec>
concept VendorSpec = requires {
    { Spec::kCompanyId } -> std::convertible_to<CompanyId>;
};

template <VendorSpec Spec>
struct GenericVendorPayload {
    static constexpr CompanyId kCompanyId = Spec::kCompanyId;

    static constexpr bool kEchoesCompanyId = [] {
        if constexpr (requires { { Spec::kEchoesCompanyId } -> std::convertible_to<bool>; }) {
            return Spec::kEchoesCompanyId;
        } else {
            return true;
        }
    }();

    std::array<uint8_t, 256> bytes{};
    uint16_t length{0};

    using Reply = std::span<const uint8_t>;

    GenericVendorPayload() = default;
    GenericVendorPayload(std::span<const uint8_t> data) noexcept {
        length = static_cast<uint16_t>(std::min(data.size(), bytes.size()));
        if (length > 0) {
            std::copy_n(data.begin(), length, bytes.begin());
        }
    }

    [[nodiscard]] static constexpr bool AcceptsResponseCode(ResponseCode code, AVC::CommandType ctype) noexcept {
        if constexpr (requires { { Spec::AcceptsResponseCode(code, ctype) } -> std::convertible_to<bool>; }) {
            return Spec::AcceptsResponseCode(code, ctype);
        } else {
            return (ctype == AVC::CommandType::kControl)
                ? (code == ResponseCode::kAccepted)
                : (code == ResponseCode::kImplementedStable);
        }
    }

    [[nodiscard]] Expected<void> WritePayload(Cmd::OperandWriter& w, AVC::CommandType /*t*/) const noexcept {
        return w.Append(std::span<const uint8_t>{bytes.data(), length});
    }

    [[nodiscard]] static Expected<Reply> ReadPayload(std::span<const uint8_t> in) noexcept {
        return in;
    }
};

template <VendorSpec Spec>
class VendorFrame {
public:
    static constexpr CompanyId kCompanyId = Spec::kCompanyId;

    using OperandsType = Cmd::VendorDependentOperands<GenericVendorPayload<Spec>>;
    using CommandType = Cmd::Command<OperandsType>;

    [[nodiscard]] static Expected<CommandFrame> Build(
        AVC::CommandType type,
        SubunitAddress address,
        std::span<const uint8_t> payload) noexcept {
        CommandType cmd{
            .address = address,
            .operands = OperandsType{
                .payload = GenericVendorPayload<Spec>(payload)
            }
        };
        return cmd.Encode(type);
    }

    [[nodiscard]] static Expected<std::span<const uint8_t>> Parse(
        std::span<const uint8_t> operands) noexcept {
        return OperandsType::Read(operands);
    }

    [[nodiscard]] static Expected<std::span<const uint8_t>> Parse(
        const Response& response) noexcept {
        return Parse(response.operands);
    }
};

} // namespace ASFW::AVC::Extensions
