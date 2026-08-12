// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../Devices/ResolvedProfileBuilder.hpp"

namespace ASFW::Audio::Families::DICE {
[[nodiscard]] std::expected<Devices::ResolvedAudioEndpointProfile,
                            Devices::ProfileBuildError>
BuildProfile(const Devices::ProfileBuildContext& context) noexcept;
}
