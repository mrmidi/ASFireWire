// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DiscoverySnapshot.hpp"
#include <variant>

namespace ASFW::AVC::DiscoveryEngine {
inline constexpr size_t kMaxProbeOperations = 8192;
inline constexpr uint8_t kMaxFormatEntries = 32;
/// Optional probes survive a timeout (the engine has already retried), but a
/// unit that stops answering altogether ends discovery after this many in a row.
inline constexpr uint8_t kMaxConsecutiveTimeouts = 2;
struct FormatProbe { Cmd::StreamFormatCommand command; bool fallback{false}; };
struct DescriptorProbe { SubunitId subunit; Cmd::DescriptorSpecifier specifier; size_t depth{0}; std::vector<uint16_t> ancestors; };
struct SelectorProbe { Cmd::SelectorCommand command; };
struct ClockProbe { Cmd::SignalSourceCommand command; };
enum class Checkpoint : uint8_t { Formats, Descriptors, Routes, Controls, Extension, Commit };
using Probe = std::variant<Cmd::UnitInfoCommand, Cmd::SubunitInfoCommand, Cmd::PlugInfoCommand,
    Cmd::PlugSignalFormatCommand, FormatProbe, DescriptorProbe, Cmd::SignalSourceCommand,
    Cmd::FeatureCommand, SelectorProbe, ClockProbe, Checkpoint>;
struct Idle {};
struct Probing { size_t index{}; OperationIdentity operation; };
struct Terminal {};
using Phase = std::variant<Idle, Probing, Terminal>;
static_assert(std::is_nothrow_move_constructible_v<Phase>);
static_assert(std::is_nothrow_move_constructible_v<Probe>);
struct State {
    Phase phase{Idle{}};
    DiscoverySnapshot builder;
    std::vector<Probe> probes;
    IAvcUnit::StreamFormatOpcodePolicy opcodePolicy{IAvcUnit::StreamFormatOpcodePolicy::kLearn};
    bool usesSupportOpcode{false};
    uint64_t serial{0};
    uint8_t consecutiveTimeouts{0};
};
struct Start {
    SessionId session;
    ASFW::Discovery::DeviceRouteToken route;
    AvcUnitIdentity identity;
    IAvcUnit::StreamFormatOpcodePolicy opcodePolicy;
    bool usesSupportOpcode{false};
};
struct OwnedResponse {
    ResponseCode code;
    SubunitAddress address;
    Opcode opcode;
    std::vector<uint8_t> operands;
};
struct Reply { OperationIdentity operation; Expected<OwnedResponse> response; };
struct DescriptorReply {
    OperationIdentity operation;
    ASFW::Protocols::AVC::DescriptorAccessor::ReadDescriptorResult result;
};
struct ExtensionComplete { OperationIdentity operation; ExtensionFacts facts; };
struct Cancel {};
struct RouteLost {};
using Event = std::variant<Start, Reply, DescriptorReply, ExtensionComplete, Cancel, RouteLost>;
struct Send { OperationIdentity operation; CommandFrame frame; };
struct ReadDescriptor { OperationIdentity operation; DescriptorProbe probe; };
struct RunExtension { OperationIdentity operation; };
struct LearnSupportOpcode {};
struct Commit { SnapshotLease snapshot; };
using Action = std::variant<Send, ReadDescriptor, RunExtension, LearnSupportOpcode, Commit>;
struct Transition { State state; std::vector<Action> actions; };
/// Pure deterministic transition. Input events own all bytes; actions contain no
/// callbacks, bus objects, timers, locks or device catalog knowledge.
[[nodiscard]] Transition Step(State state, Event event);
} // namespace ASFW::AVC::DiscoveryEngine
