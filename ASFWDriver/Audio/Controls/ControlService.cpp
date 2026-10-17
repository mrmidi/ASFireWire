// SPDX-License-Identifier: Apache-2.0
#include "ControlService.hpp"
#include "AvcControlProjection.hpp"
#include "AvcControlWrite.hpp"
#include "CoreAudioProjection.hpp"
#include <algorithm>
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Model/Topology/Validate.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include <cmath>

namespace ASFW::Audio::Controls {
namespace AM = AudioModel;
std::optional<ControlSnapshot> ControlService::CopySnapshot(uint64_t guid) const {
    const auto protocol = lookup_ ? lookup_(guid) : nullptr;
    if (protocol) if (const auto* provider = protocol->Controls()) return provider->CopySnapshot();
    const auto unit = discovery_ ? discovery_->Unit(guid) : nullptr;
    const auto snapshot = unit ? unit->GetDiscoverySnapshot() : nullptr;
    const auto graph = unit ? unit->GetDiscoveredGraph() : nullptr;
    if (!snapshot || !graph || !unit->IsCurrentRoute(snapshot->route)) return {};
    auto model = ProjectAvcControls(*graph, snapshot->session.value);
    return ControlSnapshot{std::move(model.topology), std::move(model.state), "avc", graph->featureChangeSequence};
}
std::optional<AM::ParameterValue> ControlService::ReadParameter(uint64_t guid, AM::ParameterId id) const {
    const auto snapshot = CopySnapshot(guid);
    if (!snapshot) return {};
    if (const auto found = snapshot->state.parameters.find(id); found != snapshot->state.parameters.end())
        return found->second.value;
    for (const auto& type : ProjectInputTypes(snapshot->topology, snapshot->state))
        if (InputTypeParameterId(type.router) == id && type.selected) return *type.selected;
    return {};
}
void ControlService::SetRoute(uint64_t guid, uint64_t revision, AM::NodeId node, AM::PortId output,
    std::optional<AM::PortId> input, IControlProvider::Completion done) const {
    const auto snapshot = CopySnapshot(guid);
    if (!snapshot) { done(kIOReturnNotReady, false); return; }
    if (snapshot->topology.revision != revision) { done(kIOReturnNotPermitted, false); return; }
    if (const auto requested = AM::prepareRouteWrite(snapshot->topology, snapshot->state, node, output, input); !requested) {
        done(requested.error().kind == AM::StateErrorKind::ReadOnlyParameter ? kIOReturnNotWritable : kIOReturnBadArgument, false);
        return;
    }
    const auto protocol = lookup_ ? lookup_(guid) : nullptr;
    if (!protocol || !protocol->Controls()) { done(kIOReturnUnsupported, false); return; }
    protocol->Controls()->SetRoute(node, output, input, [protocol, done = std::move(done)](auto status, auto value) {
        done(status, std::move(value));
    });
}

void ControlService::SetParameter(uint64_t guid, uint64_t revision, AM::ParameterId id,
    const AM::ParameterValue& value, IControlProvider::Completion done, std::function<bool()> active) const {
    const auto snapshot = CopySnapshot(guid);
    if (!snapshot) { done(kIOReturnNotReady, false); return; }
    if (snapshot->topology.revision != revision) { done(kIOReturnNotPermitted, false); return; }
    if ((id.value & 0x80000000u) != 0) {
        const auto types = ProjectInputTypes(snapshot->topology, snapshot->state);
        const auto type = std::ranges::find_if(types, [&](const auto& type) { return InputTypeParameterId(type.router) == id; });
        const auto* item = std::get_if<int64_t>(&value);
        if (type == types.end() || !item) { done(kIOReturnBadArgument, false); return; }
        const auto choice = std::ranges::find(type->choices, *item, &InputTypeChoice::value);
        if (choice == type->choices.end()) { done(kIOReturnBadArgument, false); return; }
        const auto routerNode = std::ranges::find(snapshot->topology.nodes, type->router, &AM::Node::id);
        const auto& router = std::get<AM::RouterNode>(routerNode->body);
        if (!router.writable) { done(kIOReturnNotWritable, false); return; }
        const auto bundle = std::ranges::find(router.legalBundles, choice->bundle, &AM::RouteBundle::id);
        if (bundle == router.legalBundles.end() || bundle->routes.size() != 1) { done(kIOReturnBadArgument, false); return; }
        // Each step copies the service's lookup; no callback borrows this temporary service.
        const auto service = *this;
        auto switchSource = [service, guid, revision, node = type->router, output = type->output,
                             input = bundle->routes.front().input, id, done](IOReturn status, auto) {
            if (status != kIOReturnSuccess) { done(status, false); return; }
            service.SetRoute(guid, revision, node, output, input, [service, guid, id, done](auto status, auto) {
                const auto confirmed = service.ReadParameter(guid, id);
                done(status == kIOReturnSuccess && !confirmed ? kIOReturnError : status, confirmed.value_or(AM::ParameterValue{false}));
            });
        };
        if (choice->levelParameter) SetParameter(guid, revision, *choice->levelParameter, *choice->levelValue, std::move(switchSource), active);
        else switchSource(kIOReturnSuccess, false);
        return;
    }
    if (const auto valid = AM::validateParameterWrite(snapshot->topology, snapshot->state, id, value); !valid) {
        const auto kind = valid.error().kind;
        done(kind == AM::StateErrorKind::ReadOnlyParameter || kind == AM::StateErrorKind::ParameterNotApplicable
            ? kIOReturnNotWritable : kIOReturnBadArgument, false);
        return;
    }
    const auto protocol = lookup_ ? lookup_(guid) : nullptr;
    if (protocol) if (auto* provider = protocol->Controls()) {
        provider->SetParameter(id, value, [protocol, done = std::move(done)](auto status, auto confirmed) {
            done(status, std::move(confirmed));
        });
        return;
    }
    const auto unit = discovery_ ? discovery_->Unit(guid) : nullptr;
    const auto graph = unit ? unit->GetDiscoveredGraph() : nullptr;
    const auto route = unit ? unit->CurrentRoute() : std::nullopt;
    if (!unit || !graph || !route || !unit->IsCurrentRoute(*route)) { done(kIOReturnNotReady, false); return; }
    const auto model = ProjectAvcControls(*graph, revision);
    const auto* binding = model.FindBinding(id);
    if (!binding) { done(kIOReturnUnsupported, false); return; }
    const auto* channel = FindAvcFeatureChannel(*graph, binding->Token());
    if (!channel) { done(kIOReturnNotFound, false); return; }
    const bool mute = binding->kind == AvcBindingKind::kFeatureMute;
    const int32_t raw = mute ? (std::get<bool>(value) ? 1 : 0)
        : static_cast<int32_t>(std::lround(std::get<double>(value) * 256.0));
    WriteAvcFeature(unit, *route, *channel, mute, raw, [done = std::move(done), mute](auto status, int32_t raw) {
        done(status, mute ? AM::ParameterValue{raw != 0} : AM::ParameterValue{static_cast<double>(raw) / 256.0});
    }, active ? std::move(active) : std::function<bool()>([] { return true; }));
}
} // namespace ASFW::Audio::Controls
