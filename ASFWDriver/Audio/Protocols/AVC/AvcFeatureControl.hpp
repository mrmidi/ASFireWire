// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "../../../Protocols/AVC/Graph/AvcDeviceGraph.hpp"
#include "../../Model/AvcVolumeMapping.hpp"
#include <memory>

namespace ASFW::Audio {
// Called on the unit's controller queue. Both continuations own the unit and
// route. A CONTROL acknowledgement is not state: confirm it with CURRENT.
inline void SetAvcFeature(std::shared_ptr<AVC::IAvcUnit> unit, Discovery::DeviceRouteToken route,
    const Protocols::AVC::Graph::FeatureChannelState& channel, bool muteControl, int32_t value,
    std::function<void(AVC::Expected<AVC::Cmd::FeatureReply>)> done,
    std::function<bool()> active = [] { return true; }) {
    namespace A = AVC;
    const auto fail = [&](A::AvcErrorKind kind) { done(std::unexpected(A::AvcError::Of(kind))); };
    if (!active() || !unit || !unit->IsCurrentRoute(route)) { fail(A::AvcErrorKind::kBusReset); return; }
    A::Cmd::FeatureOperands operands;
    if (muteControl) {
        if (!channel.mute || (value != 0 && value != 1)) { fail(A::AvcErrorKind::kInvalidArgument); return; }
        operands = A::Cmd::FeatureOperands::Mute(channel.block, channel.channel, value != 0);
    } else {
        if (!channel.volume || !channel.minimum || !channel.maximum || !channel.resolution || value < INT16_MIN || value > INT16_MAX) {
            fail(A::AvcErrorKind::kInvalidArgument); return;
        }
        const Model::AvcVolumeRange range{*channel.minimum, *channel.maximum, *channel.resolution};
        const auto quantized = range.Quantize(static_cast<float>(value) / 256.0f);
        if (!quantized) { fail(A::AvcErrorKind::kInvalidArgument); return; }
        operands = A::Cmd::FeatureOperands::Volume(channel.block, channel.channel, A::AvcVolume::FromRaw(*quantized));
    }
    const A::Cmd::FeatureCommand command{.address = A::SubunitAddress::Of(A::SubunitType::kAudio, channel.subunit), .operands = operands};
    unit->Control(command, route.generation, [unit, route, command, muteControl, done, active](A::Expected<A::Cmd::FeatureReply> changed) {
        if (!active() || !unit->IsCurrentRoute(route)) { done(std::unexpected(A::AvcError::Of(A::AvcErrorKind::kBusReset))); return; }
        if (!changed) { done(std::unexpected(changed.error())); return; }
        auto read = command; read.operands.dataLength = 0;
        const auto frame = read.Encode(A::CommandType::kStatus);
        if (!frame) { done(std::unexpected(frame.error())); return; }
        unit->Submit(*frame, route.generation, [unit, route, read, muteControl, done, active](auto response) {
            A::Expected<A::Cmd::FeatureReply> reply = std::unexpected(A::AvcError::Of(A::AvcErrorKind::kUnexpectedResponse));
            if (!response) reply = std::unexpected(response.error());
            else if (response->code == A::ResponseCode::kImplementedStable && response->address == read.address &&
                     response->opcode == A::Opcode::kFunctionBlock) reply = read.Decode(response->operands);
            if (!active() || !unit->IsCurrentRoute(route)) { done(std::unexpected(A::AvcError::Of(A::AvcErrorKind::kBusReset))); return; }
            if (!reply) { done(std::unexpected(reply.error())); return; }
            if (reply->functionBlockId != read.operands.functionBlockId || reply->channel != read.operands.channel ||
                reply->control != read.operands.control || reply->attribute != A::Cmd::ControlAttribute::kCurrent ||
                (muteControl ? (reply->dataLength != 1 || (reply->data[0] != A::Cmd::kBooleanTrue && reply->data[0] != A::Cmd::kBooleanFalse)) :
                               (reply->dataLength != 2 || !reply->AsVolume().IsValid()))) {
                done(std::unexpected(A::AvcError::Of(A::AvcErrorKind::kMalformedOperands))); return;
            }
            done(std::move(reply));
        });
    });
}
} // namespace ASFW::Audio
