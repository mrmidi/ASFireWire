// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CcmProfileCommand.hpp - CCM PROFILE (0x1D) operand codec.
//
// Source: TA 2002010 (CCM 1.1) §7.4; layouts Figures 7.34 (status command), 7.35 (status
// response), 7.36 (subfunction_dependent of SOURCE); Table 7.26; profiles §8.
//
// CCM PROFILE has STATUS only, and was added in CCM 1.1: a CCM 1.0 device does not
// implement it (§7.4). This codec has no caller; it is a read-only query, but an unproven
// frame still has to be captured before it goes to a device.
//
// Named request:  QueryCcmProfile()   STATUS, subfunction SOURCE   §7.4.1

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC::Cmd {

/// subfunction (Table 7.26).
enum class CcmProfileSubfunction : uint8_t {
    kSource = 0x00,  ///< Which profile the target source device implements.
};

/// A CCM PROFILE status response for the SOURCE subfunction.
struct CcmProfile {
    uint8_t subfunction{0};  ///< operand[0], as sent.
    uint8_t flags{0};        ///< operand[1]: the profile bits (Figure 7.36 offset 0).

    static constexpr uint8_t kCcm10Bit = 0x01;                      ///< "CCM1.0" (§8.1).
    static constexpr uint8_t kDigitalAnalogChangeoverBit = 0x02;    ///< "D/A" (§8.2).

    [[nodiscard]] constexpr bool IsSourceReply() const noexcept {
        return subfunction == static_cast<uint8_t>(CcmProfileSubfunction::kSource);
    }
    [[nodiscard]] constexpr bool ConformsToCcm10() const noexcept { return (flags & kCcm10Bit) != 0; }
    [[nodiscard]] constexpr bool ConformsToDigitalAnalogChangeover() const noexcept {
        return (flags & kDigitalAnalogChangeoverBit) != 0;
    }
};

struct CcmProfileOperands {
    static constexpr Opcode kOpcode = Opcode::kCcmProfile;
    static constexpr bool kRequiresUnitAddress = true;

    CcmProfileSubfunction subfunction{CcmProfileSubfunction::kSource};

    using Reply = CcmProfile;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        // Figure 7.34: operand[1..4] are ones.
        const std::array<uint8_t, kOperandBytes> ops = {static_cast<uint8_t>(subfunction), kFill, kFill, kFill, kFill};
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < kOperandBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return CcmProfile{.subfunction = in[0], .flags = in[1]};
    }

private:
    static constexpr size_t kOperandBytes = 5;
    static constexpr uint8_t kFill = 0xFF;
};

using CcmProfileCommand = Command<CcmProfileOperands>;

/// STATUS: which CCM profiles the target source device implements.
[[nodiscard]] constexpr CcmProfileCommand QueryCcmProfile() noexcept {
    return CcmProfileCommand{};
}

} // namespace ASFW::AVC::Cmd
