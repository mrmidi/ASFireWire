// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ChangeConfigurationCommand.hpp - CHANGE CONFIGURATION (0xC0), Audio subunit.
//
// Source: TA 1999008 §11.1 (Figure 11.1, Table 11.2). Spec-only: no reference stack and no captured device
// uses it, so the layout has not been seen on a wire. The opcode value is the same as the Music subunit's
// MUSIC PLUG INFO; the subunit type tells them apart (Describe(SubunitType, Opcode)).
//
// Named requests:
//   QueryConfiguration(audio)               STATUS or NOTIFY: which configuration is current (ID FFFF in the request)
//   SelectConfiguration(audio, id)          CONTROL: reconfigure the subunit to an existing configuration_ID

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::Cmd {

/// A configuration_ID (Table 11.2): 0001..FFFE are audio subunit dependent, 0000 is reserved and FFFF is
/// "invalid", the placeholder a STATUS request carries.
class ConfigurationId {
public:
    static constexpr uint16_t kReserved = 0x0000;
    static constexpr uint16_t kInvalid = 0xFFFF;

    [[nodiscard]] static constexpr ConfigurationId Of(uint16_t id) noexcept { return ConfigurationId{id}; }
    [[nodiscard]] static constexpr ConfigurationId Invalid() noexcept { return ConfigurationId{kInvalid}; }
    [[nodiscard]] static constexpr ConfigurationId FromRaw(uint16_t raw) noexcept { return ConfigurationId{raw}; }

    [[nodiscard]] constexpr uint16_t Raw() const noexcept { return raw_; }
    /// The ID of a configuration; nullopt for the reserved and invalid values.
    [[nodiscard]] constexpr std::optional<uint16_t> Id() const noexcept {
        if (raw_ == kReserved || raw_ == kInvalid) return std::nullopt;
        return raw_;
    }

    friend constexpr bool operator==(ConfigurationId, ConfigurationId) noexcept = default;

private:
    explicit constexpr ConfigurationId(uint16_t raw) noexcept : raw_(raw) {}
    uint16_t raw_;
};

static_assert(ConfigurationId::Of(1).Id() == 1 && !ConfigurationId::Invalid().Id().has_value());

struct ChangeConfigurationOperands {
    static constexpr Opcode kOpcode = Opcode::kChangeConfiguration;

    /// The configuration to select (CONTROL). STATUS and NOTIFY send FFFF instead.
    ConfigurationId configuration{ConfigurationId::Invalid()};

    using Reply = ConfigurationId;

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        if (address.IsUnit() || address.Type() != SubunitType::kAudio) return Fail(AvcErrorKind::kInvalidArgument);
        return {};
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        switch (t) {
            case CommandType::kControl: {
                // §11.1: only an existing configuration; 0000 is reserved and FFFF is the STATUS placeholder.
                if (!configuration.Id()) return Fail(AvcErrorKind::kInvalidArgument);
                const std::array<uint8_t, 2> bytes = {HighByte(configuration.Raw()), LowByte(configuration.Raw())};
                return w.Append(bytes);
            }
            case CommandType::kStatus:
            case CommandType::kNotify: {  // §11.1: the notify command has the status command's syntax
                const std::array<uint8_t, 2> bytes = {HighByte(ConfigurationId::kInvalid),
                                                      LowByte(ConfigurationId::kInvalid)};
                return w.Append(bytes);
            }
            default:
                return Fail(AvcErrorKind::kInvalidArgument);
        }
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kOperandBytes = 2;
        if (in.size() < kOperandBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        return ConfigurationId::FromRaw(static_cast<uint16_t>((in[0] << 8) | in[1]));
    }
};

using ChangeConfigurationCommand = Command<ChangeConfigurationOperands>;

/// STATUS or NOTIFY: the configuration the Audio subunit is in now (§11.1).
[[nodiscard]] constexpr ChangeConfigurationCommand QueryConfiguration(SubunitAddress audio) noexcept {
    return ChangeConfigurationCommand{.address = audio, .operands = {}};
}

/// CONTROL: reconfigure the Audio subunit to `configuration`. Changes device state. Send only to a device
/// whose frames are proven.
[[nodiscard]] constexpr ChangeConfigurationCommand SelectConfiguration(SubunitAddress audio,
                                                                       ConfigurationId configuration) noexcept {
    return ChangeConfigurationCommand{.address = audio, .operands = {.configuration = configuration}};
}

} // namespace ASFW::AVC::Cmd
