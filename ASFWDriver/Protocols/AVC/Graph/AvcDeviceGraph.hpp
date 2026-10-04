// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcDeviceGraph.hpp - Device topology and graph representation derived
// from AV/C Music Subunit Status and Audio Subunit Identifier descriptors.
//
// References:
// - Apple AppleFWAudio graph rules (applefwaudio-graph-rules.md)
// - TA Document 2001007 (AV/C Music Subunit 1.0)
// - TA Document 1999008 (AV/C Audio Subunit 1.0)
//

#pragma once

#include "../../../Common/PcmSlotMap.hpp"
#include "../Descriptors/AudioSubunitDescriptor.hpp"
#include "../Descriptors/MusicSubunitDescriptor.hpp"

#include "../Core/AvcError.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Graph {

/// Our "not set" markers for ids that are optional in the graph. They are not wire values.
inline constexpr uint16_t kUnsetMusicPlugId = 0xFFFF;
inline constexpr uint8_t kUnsetFunctionBlockType = 0xFF;

/// A single audio channel in a stream
struct StreamChannelInfo {
    uint32_t logicalIndex{0};       ///< 0-based audio channel index
    uint8_t slotIndex{0};           ///< AM824 stream position / slot index
    std::string name;               ///< Resolved channel name
    std::string clusterName;        ///< Name of the containing cluster
    uint16_t musicPlugId{kUnsetMusicPlugId};
    uint8_t formatCode{0};          ///< Stream format code (e.g. 0x06 MBLA)
};

/// A stream direction (playback or capture)
enum class SlotMapValidation : uint8_t {
    kNoDataBlockSize,
    kValidated,
    kRejectedFallback,
};

enum class StreamSelectionEvidence : uint8_t {
    kUnresolved,
    kSignalSourceInquiry,
    kDescriptorDefaultAssumption,
    kUnitPlugFormat,
};

struct StreamGraph {
    uint8_t subunitPlugId{0};
    StreamSelectionEvidence selectionEvidence{StreamSelectionEvidence::kUnresolved};
    bool isDestination{false};      ///< true = playback (dest plug), false = capture (src plug)
    uint32_t dataBlockSize{0};
    uint32_t currentSampleRate{0};
    std::vector<uint32_t> supportedSampleRates;
    uint32_t channelCount{0};       ///< Total PCM audio channels
    uint32_t midiStreamCount{0};    ///< MIDI port/stream count
    ASFW::Common::PcmSlotMap slotMap; ///< Mapped AM824 slots for PCM channels
    std::vector<StreamChannelInfo> channels;
    std::vector<std::string> channelNames;
    std::vector<Descriptors::MusicClusterInfo> clusters;
    bool routeAmbiguous{false};
    bool usingFallbackMap{false};   ///< true if descriptor map was rejected or unavailable
    SlotMapValidation slotMapValidation{SlotMapValidation::kNoDataBlockSize};
};

enum class ClockEndpointKind : uint8_t {
    kUnitIsochronousInput,
    kUnitIsochronousOutput,
    kUnitExternalInput,
    kMusicSubunitPlug,
    kAudioSubunitPlug,
    kAudioFunctionBlock,
};

struct ClockEndpointId {
    ClockEndpointKind kind{ClockEndpointKind::kUnitIsochronousInput};
    uint8_t subunitId{0};
    uint8_t endpointId{0};
    uint8_t functionBlockType{kUnsetFunctionBlockType};

    friend constexpr bool operator==(const ClockEndpointId&, const ClockEndpointId&) noexcept = default;
};

/// A source returned by an explicit completed clock inquiry.
struct ClockSourceInfo {
    std::string name;
    ClockEndpointId endpoint{};
    bool isCurrent{false};
};

/// A music-subunit destination carrying synchronization content. This is a
/// routing destination; it is not evidence that a clock source is selectable.
struct SyncDestinationInfo {
    uint8_t subunitPlugId{0};
    std::string name;
};

/// An Audio Subunit selector and its descriptor-declared input edges. The
/// descriptor edges retain their real FB/subunit plug identities, but do not
/// claim that each input was accepted by a selector inquiry.
struct AudioSelectorInfo {
    uint8_t audioSubunitId{0};
    uint8_t functionBlockId{0};
    std::string name;
    std::vector<Descriptors::AudioSourceId> declaredInputs;
    std::optional<uint8_t> currentInput; ///< Confirmed by STATUS.
};

enum class ConfirmedFeatureControl : uint8_t {
    kMute,
    kVolume,
    kLRBalance,
    kFRBalance,
    kBass,
    kMid,
    kTreble,
    kGraphicEQ,
    kAGC,
    kDelay,
    kBassBoost,
    kLoudness,
};

enum class FeatureStatusState : uint8_t {
    kNotProbed,
    kConfirmed,
    kUnsupported,
};

struct ConfirmedFeatureStatus {
    FeatureStatusState state{FeatureStatusState::kNotProbed};
    std::vector<ConfirmedFeatureControl> master;
    std::vector<std::vector<ConfirmedFeatureControl>> channels;
};

/// A discovered function block control
struct ControlBlockInfo {
    Descriptors::AudioFunctionBlockType type{Descriptors::AudioFunctionBlockType::kFeature};
    uint8_t id{0};
    std::string name;
    uint8_t channelCount{0};
    uint16_t advertisedMasterControls{0}; ///< Unverified descriptor hint
    std::vector<uint16_t> advertisedChannelControls;
    ConfirmedFeatureStatus confirmedControls{};
    std::vector<Descriptors::AudioSourceId> inputSources;
    uint8_t volumePurpose{0}; ///< Audio descriptor general_tag: general/master/input trim/output trim
    bool isMasterVolume{false};
    uint8_t audioSubunitId{0};
};

struct RoutingEdge { std::array<uint8_t, 2> source, destination; };

/// The observed answer is independent of whether the operation accepted it.
/// No answer (timeout/refusal) stays null; REJECTED/NOT IMPLEMENTED stay exact device codes.
struct ProbeResult {
    uint8_t address{}, opcode{};
    std::optional<::ASFW::AVC::AvcError> error;
    std::optional<::ASFW::AVC::ResponseCode> responseCode;
    std::vector<uint8_t> command;
    std::vector<uint8_t> responseOperands;
    std::optional<uint8_t> responseAddress, responseOpcode;
};

/// Values are signed 1/256 dB. Missing attributes remain unknown.
struct FeatureChannelState {
    uint8_t subunit{}, block{}, channel{};
    bool userPreference{false};
    std::optional<bool> mute;
    std::optional<int16_t> volume, minimum, maximum, resolution;
};

/// A PCM channel's confirmed boundary with the Audio subunit. Logical index
/// is Core Audio order; position is the descriptor channel position at that plug.
struct AudioStreamChannelBinding {
    uint32_t logicalIndex{};
    uint8_t audioSubunitId{}, plugId{}, position{};
};
struct AudioSourcePlugConnection {
    uint8_t audioSubunitId{}, plugId{};
    Descriptors::AudioSourceId source;
};

/// Complete device graph built from descriptor discovery
struct DeviceGraph {
    std::string modelName;
    bool supportsBlockingTransmit{false};
    std::optional<uint8_t> transmitModes, receiveModes;
    StreamGraph playback;
    StreamGraph capture;
    std::vector<ClockSourceInfo> clockSources;
    std::vector<SyncDestinationInfo> syncDestinations;
    std::vector<AudioSelectorInfo> selectors;
    std::vector<ControlBlockInfo> controls;
    std::vector<FeatureChannelState> featureChannels;
    std::vector<RoutingEdge> routes;
    std::vector<AudioStreamChannelBinding> playbackAudioChannels, captureAudioChannels;
    std::vector<AudioSourcePlugConnection> audioSourcePlugs;
    std::vector<ProbeResult> probeResults;
};

} // namespace ASFW::Protocols::AVC::Graph
