// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../Devices/ResolvedProfileBuilder.hpp"

namespace ASFW::Audio::Families::Common {

[[nodiscard]] std::expected<Devices::ResolvedAudioEndpointProfile,
                            Devices::ProfileBuildError>
BuildBase(const Devices::ProfileBuildContext& context,
          const AudioStreamRuntimeCaps& caps,
          std::span<const uint32_t> supportedRates) noexcept;

void AddDefaultTiming(Devices::ResolvedAudioEndpointProfile& profile,
                      uint32_t anchorTimeoutMs) noexcept;

} // namespace ASFW::Audio::Families::Common
