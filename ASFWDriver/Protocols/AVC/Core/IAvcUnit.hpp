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

#pragma once

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
};

/// Concept defining a strongly-typed AV/C command.
/// A command type provides:
/// - `Encode() const` returning either `CommandFrame` or `Expected<CommandFrame>`
/// - An associated `Reply` type
/// - `static Decode(const Response&)` or `static Decode(std::span<const uint8_t>)` returning `Expected<Reply>`
template <typename T>
concept AvcCommand = requires(const T& cmd, const Response& response, std::span<const uint8_t> span) {
    typename T::Reply;
    { cmd.Encode() };
    requires (
        requires { { T::Decode(response) } -> std::same_as<Expected<typename T::Reply>>; } ||
        requires { { T::Decode(span) } -> std::same_as<Expected<typename T::Reply>>; }
    );
};

/// Helper to asynchronously dispatch a strongly typed AV/C command to an IAvcUnit.
template <AvcCommand Cmd, typename Callback>
void Send(IAvcUnit& unit, const Cmd& cmd, FW::Generation generation, Callback&& completion) {
    auto encoded = [&]() -> Expected<CommandFrame> {
        if constexpr (std::is_same_v<std::decay_t<decltype(cmd.Encode())>, CommandFrame>) {
            return cmd.Encode();
        } else {
            return cmd.Encode();
        }
    }();

    if (!encoded) {
        std::forward<Callback>(completion)(std::unexpected(encoded.error()));
        return;
    }

    unit.Submit(*encoded, generation,
                [cb = std::forward<Callback>(completion)](Expected<Response> response) mutable {
                    if (!response) {
                        cb(std::unexpected(response.error()));
                        return;
                    }
                    if constexpr (requires { { Cmd::Decode(*response) } -> std::same_as<Expected<typename Cmd::Reply>>; }) {
                        cb(Cmd::Decode(*response));
                    } else {
                        cb(Cmd::Decode(response->operands));
                    }
                });
}

/// Overload using the unit's current bus generation.
template <AvcCommand Cmd, typename Callback>
void Send(IAvcUnit& unit, const Cmd& cmd, Callback&& completion) {
    Send(unit, cmd, unit.CurrentGeneration(), std::forward<Callback>(completion));
}

} // namespace ASFW::AVC
