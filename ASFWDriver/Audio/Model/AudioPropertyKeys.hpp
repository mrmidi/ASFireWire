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

inline constexpr const char* kBoolClassId = "ClassID";
inline constexpr const char* kBoolScope = "Scope";
inline constexpr const char* kBoolElement = "Element";
inline constexpr const char* kBoolSettable = "Settable";
inline constexpr const char* kBoolInitial = "Initial";

} // namespace ASFW::Audio::Model::PropertyKeys
