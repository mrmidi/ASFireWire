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
inline constexpr const char* kInputChannelCount = "ASFWInputChannelCount";
inline constexpr const char* kOutputChannelCount = "ASFWOutputChannelCount";
inline constexpr const char* kInputPlugName = "ASFWInputPlugName";
inline constexpr const char* kOutputPlugName = "ASFWOutputPlugName";
inline constexpr const char* kCurrentSampleRate = "ASFWCurrentSampleRate";
inline constexpr const char* kStreamMode = "ASFWStreamMode";
inline constexpr const char* kHasPhantomOverride = "ASFWHasPhantomOverride";
inline constexpr const char* kPhantomSupportedMask = "ASFWPhantomSupportedMask";
inline constexpr const char* kPhantomInitialMask = "ASFWPhantomInitialMask";
inline constexpr const char* kBoolControlOverrides = "ASFWBoolControlOverrides";

inline constexpr const char* kBoolClassId = "ClassID";
inline constexpr const char* kBoolScope = "Scope";
inline constexpr const char* kBoolElement = "Element";
inline constexpr const char* kBoolSettable = "Settable";
inline constexpr const char* kBoolInitial = "Initial";

} // namespace ASFW::Audio::Model::PropertyKeys
