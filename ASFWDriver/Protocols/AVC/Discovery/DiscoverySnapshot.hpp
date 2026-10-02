// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../Core/AvcUnitModel.hpp"
#include "../Commands/StreamFormatCommand.hpp"
#include "../Commands/SignalSourceCommand.hpp"
#include "../Commands/FunctionBlockCommand.hpp"
#include "../Descriptors/DescriptorAccessor.hpp"
#include "../Descriptors/MusicSubunitDescriptor.hpp"
#include "../Descriptors/AudioSubunitDescriptor.hpp"
#include <memory>

namespace ASFW::AVC::DiscoveryEngine {
namespace ParsedDescriptors = ASFW::Protocols::AVC::Descriptors;
struct SessionId { uint64_t value{}; friend bool operator==(SessionId, SessionId) = default; };
struct OperationSerial { uint64_t value{}; friend bool operator==(OperationSerial, OperationSerial) = default; };
struct PlugId { uint8_t value{}; friend bool operator==(PlugId, PlugId) = default; };
struct OperationIdentity {
    SessionId session;
    OperationSerial serial;
    ASFW::Discovery::DeviceRouteToken route;
    friend bool operator==(const OperationIdentity&, const OperationIdentity&) = default;
};
struct PlugContents {
    SubunitAddress address{SubunitAddress::Unit()};
    Cmd::PlugDirection direction{Cmd::PlugDirection::kInput};
    PlugId id;
    std::optional<Cmd::PlugSignalFormat> signalFormat;
    std::optional<Cmd::StreamFormat> current;
    std::vector<Cmd::StreamFormat> formations;
    std::optional<Cmd::SignalSource> route;
};
struct DescriptorBlob {
    SubunitId subunit;
    Cmd::DescriptorSpecifier specifier;
    std::vector<uint8_t> bytes;
    std::optional<AvcError> primaryError, cleanupError;
    std::optional<ParsedDescriptors::ParseError> parseError;
};
struct FeatureStatus {
    SubunitId subunit;
    uint8_t blockId{}, channel{};
    Cmd::FeatureControl control{Cmd::FeatureControl::kMute};
    std::optional<Cmd::FeatureReply> value;
    std::optional<AvcError> error;
};
struct SelectorStatus {
    SubunitId subunit;
    uint8_t blockId{};
    std::optional<Cmd::SelectorValue> value;
    std::vector<uint8_t> selectableInputs;
};
struct SubunitContents {
    SubunitId id;
    std::optional<ParsedDescriptors::MusicSubunitStatus> music;
    std::optional<ParsedDescriptors::AudioSubunitIdentifier> audio;
    ParsedDescriptors::TextDatabase text;
};
enum class TextReferenceKind : uint8_t { Duplicate, Cycle, BudgetExceeded };
struct TextReferenceDiagnostic { SubunitId subunit; uint16_t target; TextReferenceKind kind; };
struct ProbeOutcome {
    SubunitAddress address{SubunitAddress::Unit()};
    Opcode opcode{Opcode::kUnitInfo};
    std::optional<AvcError> error;
};
struct DiscoverySnapshot {
    SessionId session;
    ASFW::Discovery::DeviceRouteToken route;
    UnitModel unit;
    std::vector<SubunitContents> contents;
    std::vector<PlugContents> plugs;
    std::vector<DescriptorBlob> descriptors;
    std::vector<FeatureStatus> features;
    std::vector<SelectorStatus> selectors;
    std::vector<Cmd::SignalSource> confirmedClockRoutes;
    std::vector<ProbeOutcome> outcomes;
    std::vector<TextReferenceDiagnostic> textReferences;
    bool complete{false}, cancelled{false};
    std::optional<AvcError> terminalError;
};
using SnapshotLease = std::shared_ptr<const DiscoverySnapshot>;
} // namespace ASFW::AVC::DiscoveryEngine
