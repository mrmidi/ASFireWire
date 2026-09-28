// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IAvcUnit.hpp - The AV/C Unit runtime interface and typed command dispatch seam.
//
// Concept source: Apple IOFireWireAVCUnit (behaviour only; see docs/avc-rebuild/00-overview.md).
//
// Contract:
// - Asynchronous and callback-based; non-blocking.
// - At most one command outstanding per unit (enforced by the transport/engine).
// - Strongly typed commands dispatched via C++26 concept `AvcCommand` and `Send<Cmd>()`.
// - ctype selected at dispatch time via unit.Status(), unit.Control(), unit.Inquiry().
// - Response code policy lives in the engine (IAvcUnit dispatch).

#pragma once

#include "AvcCommand.hpp"
#include "AvcError.hpp"
#include "AvcFrame.hpp"
#include "AvcTypes.hpp"
#include "Common/FWTypes.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>

namespace ASFW::AVC {

/// Identity tuple for an AV/C unit.
struct AvcUnitIdentity {
    uint64_t guid{0};
    FW::NodeId nodeId{0};
    FW::Generation generation{0};
};

template <typename T>
concept AvcCommand = requires(const T& cmd, const Response& response, std::span<const uint8_t> span) {
    typename T::Reply;
    requires (
        requires { { cmd.Encode() }; } ||
        requires { { cmd.Encode(CommandType::kStatus) }; }
    );
    requires (
        requires { { T::Decode(response) } -> std::same_as<Expected<typename T::Reply>>; } ||
        requires { { T::Decode(span) } -> std::same_as<Expected<typename T::Reply>>; } ||
        requires { { cmd.Decode(response) } -> std::same_as<Expected<typename T::Reply>>; } ||
        requires { { cmd.Decode(span) } -> std::same_as<Expected<typename T::Reply>>; }
    );
};

template <typename Cmd>
[[nodiscard]] constexpr bool IsResponseCodeAccepted(const Cmd& cmd, ResponseCode code, CommandType ctype) noexcept {
    (void)cmd;
    if constexpr (requires { { Cmd::OperandsType::AcceptsResponseCode(code, ctype) } -> std::convertible_to<bool>; }) {
        return Cmd::OperandsType::AcceptsResponseCode(code, ctype);
    } else if constexpr (requires { { Cmd::AcceptsResponseCode(code, ctype) } -> std::convertible_to<bool>; }) {
        return Cmd::AcceptsResponseCode(code, ctype);
    } else {
        switch (ctype) {
            case CommandType::kControl:
                return code == ResponseCode::kAccepted;
            case CommandType::kStatus:
                return code == ResponseCode::kImplementedStable || code == ResponseCode::kInTransition;
            case CommandType::kSpecificInquiry:
            case CommandType::kGeneralInquiry:
                return code == ResponseCode::kImplementedStable;
            case CommandType::kNotify:
                return false;
        }
        return false;
    }
}

class IAvcUnit;

template <AvcCommand Cmd, typename Callback>
void SendCommand(IAvcUnit& unit, const Cmd& cmd, CommandType type, FW::Generation generation, Callback&& completion);

template <AvcCommand Cmd, typename Callback>
void SendStatus(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion);

template <AvcCommand Cmd, typename Callback>
void SendControl(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion);

template <AvcCommand Cmd, typename Callback>
void SendInquiry(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion);

/// Pure-virtual interface representing an AV/C Unit on the FireWire bus.
/// Decouples command callers from the concrete transport/simulation engine.
class IAvcUnit {
public:
    virtual ~IAvcUnit() = default;

    using ResponseCallback = std::function<void(Expected<Response>)>;

    /// Submit an AV/C command frame asynchronously.
    /// The transport is responsible for queuing so that at most one command
    /// is outstanding per unit at any time.
    ///
    /// The completion is called with Expected<Response>. Note that Response
    /// operands are valid during the callback invocation only.
    virtual void Submit(const CommandFrame& frame,
                        FW::Generation generation,
                        ResponseCallback completion) = 0;

    [[nodiscard]] virtual FW::NodeId NodeId() const noexcept = 0;
    [[nodiscard]] virtual FW::Generation CurrentGeneration() const noexcept = 0;
    [[nodiscard]] virtual uint64_t Guid() const noexcept = 0;

    [[nodiscard]] virtual AvcUnitIdentity Identity() const noexcept {
        return AvcUnitIdentity{
            .guid = Guid(),
            .nodeId = NodeId(),
            .generation = CurrentGeneration(),
        };
    }

    /// Dispatch a STATUS command.
    template <AvcCommand Cmd, typename Callback>
    void Status(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendStatus(*this, cmd, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Status(const Cmd& cmd, Callback&& completion) {
        Status(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }

    /// Dispatch a CONTROL command.
    template <AvcCommand Cmd, typename Callback>
    void Control(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendControl(*this, cmd, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Control(const Cmd& cmd, Callback&& completion) {
        Control(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }

    /// Dispatch a SPECIFIC INQUIRY command.
    template <AvcCommand Cmd, typename Callback>
    void Inquiry(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendInquiry(*this, cmd, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Inquiry(const Cmd& cmd, Callback&& completion) {
        Inquiry(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }
};

/// Helper to asynchronously dispatch a strongly typed AV/C command to an IAvcUnit with a specific CommandType.
template <AvcCommand Cmd, typename Callback>
void SendCommand(IAvcUnit& unit, const Cmd& cmd, CommandType type, FW::Generation generation, Callback&& completion) {
    auto encoded = [&]() -> Expected<CommandFrame> {
        if constexpr (requires { { cmd.Encode(type) }; }) {
            return cmd.Encode(type);
        } else {
            return cmd.Encode();
        }
    }();

    if (!encoded) {
        std::forward<Callback>(completion)(std::unexpected(encoded.error()));
        return;
    }

    unit.Submit(*encoded, generation,
                [cmd, type, cb = std::forward<Callback>(completion)](Expected<Response> response) mutable {
                    if (!response) {
                        cb(std::unexpected(response.error()));
                        return;
                    }
                    if (!IsResponseCodeAccepted(cmd, response->code, type)) {
                        cb(std::unexpected(AvcError::Unexpected(response->code)));
                        return;
                    }
                    if constexpr (requires { { Cmd::Decode(*response) } -> std::same_as<Expected<typename Cmd::Reply>>; }) {
                        cb(Cmd::Decode(*response));
                    } else if constexpr (requires { { Cmd::Decode(response->operands) } -> std::same_as<Expected<typename Cmd::Reply>>; }) {
                        cb(Cmd::Decode(response->operands));
                    } else if constexpr (requires { { cmd.Decode(*response) } -> std::same_as<Expected<typename Cmd::Reply>>; }) {
                        cb(cmd.Decode(*response));
                    } else {
                        cb(cmd.Decode(response->operands));
                    }
                });
}

template <AvcCommand Cmd, typename Callback>
void SendStatus(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion) {
    SendCommand(unit, cmd, CommandType::kStatus, generation, std::forward<Callback>(completion));
}

template <AvcCommand Cmd, typename Callback>
void SendControl(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion) {
    SendCommand(unit, cmd, CommandType::kControl, generation, std::forward<Callback>(completion));
}

template <AvcCommand Cmd, typename Callback>
void SendInquiry(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion) {
    SendCommand(unit, cmd, CommandType::kSpecificInquiry, generation, std::forward<Callback>(completion));
}

/// Generic Send() defaulting to STATUS for commands supporting ctype, or cmd.Encode().
template <AvcCommand Cmd, typename Callback>
void Send(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion) {
    SendCommand(unit, cmd, CommandType::kStatus, generation, std::forward<Callback>(completion));
}

template <AvcCommand Cmd, typename Callback>
void Send(IAvcUnit& unit, const Cmd& cmd, Callback&& completion) {
    Send(unit, cmd, unit.CurrentGeneration(), std::forward<Callback>(completion));
}

} // namespace ASFW::AVC
