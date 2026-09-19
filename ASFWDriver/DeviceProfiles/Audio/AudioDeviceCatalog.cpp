// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioDeviceCatalog.hpp"

#include "Definitions/Alesis.hpp"
#include "Definitions/Apogee.hpp"
#include "Definitions/Focusrite.hpp"
#include "Definitions/MAudio.hpp"
#include "Definitions/Mackie.hpp"
#include "Definitions/Midas.hpp"
#include "Definitions/MOTU.hpp"
#include "Definitions/PreSonus.hpp"
#include "Definitions/TerraTec.hpp"
#include "Definitions/Weiss.hpp"

#include <array>
#include <span>

namespace ASFW::DeviceProfiles::Audio {

namespace {

template <typename T, size_t... Ns>
constexpr auto ConcatArrays(const std::array<T, Ns>&... arrays) {
    std::array<T, (Ns + ...)> result{};
    size_t offset = 0;
    auto copy = [&result, &offset]<size_t N>(const std::array<T, N>& arr) {
        for (size_t i = 0; i < N; ++i) {
            result[offset + i] = arr[i];
        }
        offset += N;
    };
    (copy(arrays), ...);
    return result;
}

constexpr auto kDefinitions = ConcatArrays(
    Definitions::kFocusriteDefinitions,
    Definitions::kWeissDefinitions,
    Definitions::kApogeeDefinitions,
    Definitions::kTerraTecDefinitions,
    Definitions::kAlesisDefinitions,
    Definitions::kMidasDefinitions,
    Definitions::kPreSonusDefinitions,
    Definitions::kMAudioDefinitions,
    Definitions::kMotuDefinitions,
    Definitions::kMackieDefinitions
);

static_assert(kDefinitions.size() == 39U, "Catalog definition count mismatch");

// No safety rule is currently defined.
//
// The M-Audio special-firmware personas (0x00010071, 0x00010091) used to live
// here. A safety rule is a device-level kill switch: it quarantines the record,
// which stops the audio session, the family adapter, *and* FCP transport
// construction alike. That was too blunt in both directions — it refused the one
// command those devices tolerate (AVC_DEVICE_HAZARDS.md H1) while doing nothing
// to distinguish the commands that actually freeze them. They now carry
// ProbePolicyId::BeBoBFilteredCommandSet, which bounds them per-frame at the
// point frames are sent.
//
// Keep the mechanism. It remains the right expression for an identity that must
// never be touched at all, as opposed to one that must be touched carefully.
constexpr std::array<AudioSafetyRule, 0> kSafetyRules{};

} // namespace

std::span<const AudioDeviceDefinition> AudioDeviceCatalog::Definitions() noexcept {
    return kDefinitions;
}

std::span<const AudioSafetyRule> AudioDeviceCatalog::SafetyRules() noexcept {
    return kSafetyRules;
}

const char* AudioDeviceCatalog::MotuModelNameForSwVersion(uint32_t swVersion) noexcept {
    return Definitions::MotuModelNameForSwVersion(swVersion);
}

} // namespace ASFW::DeviceProfiles::Audio
