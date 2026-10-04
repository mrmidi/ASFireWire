// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IAvcUnit.hpp - The AV/C Unit runtime interface and typed command dispatch seam.
//
// Concept source: Apple IOFireWireAVCUnit (behaviour only; see documentation/avc-rebuild/00-overview.md).
//
// Contract:
// - Asynchronous and callback-based; non-blocking.
// - At most one command outstanding per unit (enforced by the transport/engine).
// - Strongly typed commands dispatched through Status(), Control(), and Inquiry().
// - ctype selected at dispatch time via unit.Status(), unit.Control(), unit.Inquiry().
// - Response code policy lives in the engine (IAvcUnit dispatch).

#pragma once

#include "AvcCommand.hpp"
#include "AvcError.hpp"
#include "AvcFrame.hpp"
#include "AvcTypes.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"
#include "../../../Common/FWTypes.hpp"
#include "../../../Common/Lifetime.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

namespace ASFW::AVC {

/// Identity tuple for an AV/C unit.
struct AvcUnitIdentity {
    uint64_t guid{0};
    FW::NodeId nodeId{0};
    FW::Generation generation{0};
};

template <typename T>
concept AvcCommand = requires(const T& cmd, std::span<const uint8_t> span) {
    typename T::Reply;
    { cmd.Encode(CommandType::kStatus) } -> std::same_as<Expected<CommandFrame>>;
    { cmd.Decode(span) } -> std::same_as<Expected<typename T::Reply>>;
};

[[nodiscard]] constexpr bool IsResponseCodeAccepted(ResponseCode code, CommandType ctype) noexcept {
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

class IAvcUnit;

template <AvcCommand Cmd, typename Callback>
void SendCommand(IAvcUnit& unit, const Cmd& cmd, CommandType type, FW::Generation generation, Callback&& completion);

/// Pure-virtual interface representing an AV/C Unit on the FireWire bus.
/// Decouples command callers from the concrete transport/simulation engine.
class IAvcUnit {
public:
    virtual ~IAvcUnit() = default;

    /// Expires when this unit is destroyed. A continuation that holds the unit
    /// by raw pointer or reference checks it before using the unit: the
    /// transaction engine can deliver a completion after its unit is gone (its
    /// bus callbacks and timers keep it alive). Everything runs on the driver's
    /// one work queue, so checking and destruction cannot interleave.
    [[nodiscard]] std::weak_ptr<const void> LifetimeToken() const noexcept { return lifetime_.Token(); }

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

    /// Full route identity for generation-bound multi-command operations.
    /// Simulators use a stable synthetic incarnation; concrete units override.
    [[nodiscard]] virtual std::optional<Discovery::DeviceRouteToken> CurrentRoute() const noexcept {
        return Discovery::DeviceRouteToken{Guid(), 1, 1, CurrentGeneration(),
                                           NodeId().value};
    }
    [[nodiscard]] virtual bool IsCurrentRoute(const Discovery::DeviceRouteToken& route) const noexcept {
        return CurrentRoute() == route;
    }

    /// Publish a completed duplex rate change to driver-owned discovery state.
    /// Called only after the family has successfully applied/confirmed both directions.
    /// The route binds the update to the operation's device incarnation/generation.
    /// Transport-only implementations and simulators have no discovery cache.
    virtual void RememberConfirmedDuplexRate(const Discovery::DeviceRouteToken&, uint32_t) {}

    /// Startup defaults must not overwrite explicit HAL writes/restoration.
    [[nodiscard]] virtual bool HasUserFeaturePreference(uint8_t, uint8_t) const noexcept { return false; }

    /// Dispatch a STATUS command.
    template <AvcCommand Cmd, typename Callback>
    void Status(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendCommand(*this, cmd, CommandType::kStatus, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Status(const Cmd& cmd, Callback&& completion) {
        Status(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }

    /// Dispatch a CONTROL command.
    template <AvcCommand Cmd, typename Callback>
    void Control(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendCommand(*this, cmd, CommandType::kControl, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Control(const Cmd& cmd, Callback&& completion) {
        Control(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }

    /// Dispatch a SPECIFIC INQUIRY command.
    template <AvcCommand Cmd, typename Callback>
    void Inquiry(const Cmd& cmd, FW::Generation generation, Callback&& completion) {
        SendCommand(*this, cmd, CommandType::kSpecificInquiry, generation, std::forward<Callback>(completion));
    }

    template <AvcCommand Cmd, typename Callback>
    void Inquiry(const Cmd& cmd, Callback&& completion) {
        Inquiry(cmd, CurrentGeneration(), std::forward<Callback>(completion));
    }

    /// Which stream-format opcode this unit is asked with: EXTENDED STREAM
    /// FORMAT (0xBF, TA 2001002) or STREAM FORMAT SUPPORT (0x2F, BridgeCo).
    /// Fixed by the catalog, or learned once (see Cmd::SendStreamFormat).
    enum class StreamFormatOpcodePolicy : uint8_t {
        /// 0xBF until the unit refuses it with NOT IMPLEMENTED and then answers
        /// 0x2F; 0x2F for good after that (AVCVideoServices
        /// MusicSubunitController.cpp:150, 1784-1789).
        kLearn,
        /// Always 0x2F (BridgeCo: Linux bebob_command.c sends only 0x2F).
        kSupportOnly,
        /// Always 0xBF; a refusal is an answer, not a reason to switch.
        kExtendedOnly,
    };

    void SetStreamFormatOpcodePolicy(StreamFormatOpcodePolicy policy) noexcept { opcodePolicy_ = policy; }
    [[nodiscard]] StreamFormatOpcodePolicy GetStreamFormatOpcodePolicy() const noexcept { return opcodePolicy_; }

    /// True when stream-format commands go out as 0x2F: fixed so, or learned.
    [[nodiscard]] bool UsesStreamFormatSupportOpcode() const noexcept {
        return opcodePolicy_ == StreamFormatOpcodePolicy::kSupportOnly || learnedSupportOpcode_;
    }
    /// True when a NOT IMPLEMENTED 0xBF may be retried once as 0x2F.
    [[nodiscard]] bool MayLearnStreamFormatOpcode() const noexcept {
        return opcodePolicy_ == StreamFormatOpcodePolicy::kLearn && !learnedSupportOpcode_;
    }
    /// The unit answered 0x2F where it had refused 0xBF. Only under kLearn.
    void LearnStreamFormatSupportOpcode() noexcept {
        if (opcodePolicy_ == StreamFormatOpcodePolicy::kLearn) {
            learnedSupportOpcode_ = true;
        }
    }

protected:
    /// A concrete unit calls this first in its destructor, before any teardown
    /// that can reach external completions, so no LiveRef sees a dying unit.
    void RetireLifetime() noexcept { lifetime_.Invalidate(); }

private:
    Common::LifetimeAnchor lifetime_;
    StreamFormatOpcodePolicy opcodePolicy_{StreamFormatOpcodePolicy::kLearn};
    bool learnedSupportOpcode_{false};
};

/// Helper to asynchronously dispatch a strongly typed AV/C command to an IAvcUnit with a specific CommandType.
template <AvcCommand Cmd, typename Callback>
void SendCommand(IAvcUnit& unit, const Cmd& cmd, CommandType type, FW::Generation generation, Callback&& completion) {
    auto encoded = cmd.Encode(type);

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
                    if (!IsResponseCodeAccepted(response->code, type)) {
                        cb(std::unexpected(AvcError::Unexpected(response->code)));
                        return;
                    }
                    cb(cmd.Decode(response->operands));
                });
}

} // namespace ASFW::AVC
