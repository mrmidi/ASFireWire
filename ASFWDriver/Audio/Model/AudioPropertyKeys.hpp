// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

namespace ASFW::Audio::Model::PropertyKeys {

inline constexpr const char* kDeviceName = "ASFWDeviceName";
inline constexpr const char* kChannelCount = "ASFWChannelCount";
inline constexpr const char* kSampleRates = "ASFWSampleRates";
inline constexpr const char* kGuid = "ASFWGUID";
inline constexpr const char* kVendorId = "ASFWVendorID";
inline constexpr const char* kModelId = "ASFWModelID";
// The profile builder the device catalog resolved for this device, as a
// uint32. Carried across the nub so the AudioDriverKit side does not repeat
// the identity match from (vendorId, modelId) -- a pair that cannot express a
// published model_id of 0 and so cannot identify a MOTU device at all.
// Absent or zero means "not resolved"; see DeviceProfiles/Audio/AudioDeviceCatalog.hpp.
inline constexpr const char* kProfileBuilderId = "ASFWProfileBuilderID";
inline constexpr const char* kInputChannelCount = "ASFWInputChannelCount";
inline constexpr const char* kOutputChannelCount = "ASFWOutputChannelCount";
inline constexpr const char* kInputPlugName = "ASFWInputPlugName";
inline constexpr const char* kOutputPlugName = "ASFWOutputPlugName";
// Optional per-channel device labels (OSArray of OSString, in channel order).
// When present they override the synthesized "<plug> N" names; missing/empty
// entries fall back to the synthesized name.
inline constexpr const char* kInputChannelNames = "ASFWInputChannelNames";
inline constexpr const char* kOutputChannelNames = "ASFWOutputChannelNames";
inline constexpr const char* kCurrentSampleRate = "ASFWCurrentSampleRate";
inline constexpr const char* kStreamMode = "ASFWStreamMode";

// Per-stream wire geometry, resolved from the device's own registers and
// validated against the profile before publication. OSArray of OSDictionary,
// one entry per isochronous stream, in stream order.
//
// This exists because the aggregate channel counts above cannot describe an
// asymmetric device. The recorded Midas Venice F24 carries 16 + 8 playback
// channels; an aggregate of 24 is true but unusable for framing, and the
// profile's own constants describe an F32 (16 + 16). Without this the audio
// side had no way to learn the real per-stream shape, so it built packets from
// compiled-in constants while the transport reserved bandwidth from the device.
//
// Directions are named for the HOST: playback is host -> device (DICE RX),
// capture is device -> host (DICE TX).
inline constexpr const char* kPlaybackStreams = "ASFWPlaybackStreams";
inline constexpr const char* kCaptureStreams = "ASFWCaptureStreams";

// Keys within one stream entry.
inline constexpr const char* kStreamPcmChannels = "PCM";
inline constexpr const char* kStreamAm824Slots = "Slots";
inline constexpr const char* kStreamMidiPorts = "MIDI";
/// First host channel this stream carries: the running sum of the PCM channel
/// counts of the streams before it, NOT index * width. Those coincide only
/// while every stream is the same width. The vendor drivers carry the same
/// running base (AlesisFirewireAudioEngine::CreateStreams advances it by each
/// stream's own count).
inline constexpr const char* kStreamChannelOffset = "Offset";

/// Set when the publisher resolved per-stream geometry from the device and the
/// audio side must NOT fall back to its profile's constants.
///
/// Without this, losing the stream arrays — a failed allocation, a property the
/// nub rejected — is indistinguishable from a family that never had geometry to
/// publish, and the fallback silently reinstates exactly the mismatch the
/// resolution removed. A DICE device that reaches publication has always
/// resolved its geometry, so absence of the arrays alongside this flag is a
/// transport fault and must fail rather than degrade.
inline constexpr const char* kResolvedGeometryRequired = "ASFWResolvedGeometryRequired";

inline constexpr const char* kBoolClassId = "ClassID";
inline constexpr const char* kBoolScope = "Scope";
inline constexpr const char* kBoolElement = "Element";
inline constexpr const char* kBoolSettable = "Settable";
inline constexpr const char* kBoolInitial = "Initial";

} // namespace ASFW::Audio::Model::PropertyKeys
