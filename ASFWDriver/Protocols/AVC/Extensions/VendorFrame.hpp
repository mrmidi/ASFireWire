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
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::AVC::Extensions {

template <typename Spec>
concept VendorSpec = requires {
    { Spec::kCompanyId } -> std::convertible_to<CompanyId>;
};

template <VendorSpec Spec>
class VendorFrame {
public:
    static constexpr CompanyId kCompanyId = Spec::kCompanyId;

    [[nodiscard]] static constexpr bool EchoesCompanyId() noexcept {
        if constexpr (requires { Spec::kEchoesCompanyId; }) {
            return Spec::kEchoesCompanyId;
        } else {
            return true;
        }
    }

    [[nodiscard]] static Expected<CommandFrame> Build(
        CommandType type,
        SubunitAddress address,
        std::span<const uint8_t> payload) noexcept {
        return Cmd::BuildVendorDependent(type, address, kCompanyId, payload);
    }

    [[nodiscard]] static Expected<std::span<const uint8_t>> Parse(
        std::span<const uint8_t> operands) noexcept {
        auto res = Cmd::ParseVendorDependent(operands);
        if (!res) {
            return std::unexpected(res.error());
        }
        if constexpr (EchoesCompanyId()) {
            if (res->companyId != kCompanyId) {
                return Fail(AvcErrorKind::kMalformedOperands);
            }
        }
        return res->payload;
    }

    [[nodiscard]] static Expected<std::span<const uint8_t>> Parse(
        const Response& response) noexcept {
        return Parse(response.operands);
    }
};

} // namespace ASFW::AVC::Extensions
