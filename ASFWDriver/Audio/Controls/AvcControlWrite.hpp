// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcControlWrite.hpp - the one AV/C feature write path (FW-206).
//
// CoreAudio (ASFWAudioNub::SetControlParameter) and the app/MCP (user-client
// SetControlParameter) both reach it through ControlService, so a value set from either side
// is confirmed the same way and lands in the same place: CONTROL, then a STATUS
// readback (SetAvcFeature), then the confirmed reply republishes the unit's
// immutable graph (AVCUnit::RememberConfirmedFeature). The graph is the only
// state; the next control projection reads it.
#pragma once

#include "../Model/AvcVolumeMapping.hpp"
#include "../Protocols/AVC/AvcFeatureControl.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"

#include <functional>
#include <memory>

namespace ASFW::Audio::Controls {

/// The graph's feature channel for a CoreAudio control token, or null.
[[nodiscard]] inline const Protocols::AVC::Graph::FeatureChannelState*
FindAvcFeatureChannel(const Protocols::AVC::Graph::DeviceGraph& graph, uint32_t token) noexcept {
    for (const auto& channel : graph.featureChannels)
        if (Model::AvcControlToken(channel.subunit, channel.block, channel.channel) == token) return &channel;
    return nullptr;
}

/// Called on the unit's controller queue. `value` is raw 1/256 dB for a
/// volume and 0/1 for a mute. `done` receives the hardware's confirmed value.
inline void WriteAvcFeature(std::shared_ptr<Protocols::AVC::AVCUnit> unit, Discovery::DeviceRouteToken route,
                            const Protocols::AVC::Graph::FeatureChannelState& channel, bool muteControl,
                            int32_t value, std::function<void(IOReturn status, int32_t confirmed)> done,
                            std::function<bool()> active = [] { return true; }) {
    const auto subunit = channel.subunit;
    const uint32_t token = Model::AvcControlToken(channel.subunit, channel.block, channel.channel);
    SetAvcFeature(unit, route, channel, muteControl, value,
        [unit, route, subunit, muteControl, token, done = std::move(done)](AVC::Expected<AVC::Cmd::FeatureReply> reply) {
            if (!reply) {
                const auto& error = reply.error();
                ASFW_LOG(Audio, "[AvcControl] transaction failed guid=0x%016llx token=0x%06x errorKind=%u response=%d operandOffset=%u",
                         route.guid, token, static_cast<unsigned>(error.kind),
                         error.response ? static_cast<int>(*error.response) : -1, error.operandOffset);
                done(AVC::ToIOReturn(error), 0);
                return;
            }
            const int32_t confirmed = muteControl ? static_cast<int32_t>(reply->AsMute()) : reply->AsVolume().Raw();
            ASFW_LOG(Audio, "[AvcControl] readback guid=0x%016llx token=0x%06x mute=%u confirmedRaw=%d generation=%u",
                     route.guid, token, muteControl, confirmed, route.generation.value);
            unit->RememberConfirmedFeature(route, subunit, *reply);
            done(kIOReturnSuccess, confirmed);
        }, std::move(active));
}

} // namespace ASFW::Audio::Controls
