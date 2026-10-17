// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlsHandler.cpp - see ControlsHandler.hpp.

#include "ControlsHandler.hpp"

#include "../WireFormats/AVCDiscoveryDocument.hpp"
#include "../WireFormats/ControlSnapshotDocument.hpp"
#include "../../Audio/Controls/AvcControlProjection.hpp"
#include "../../Audio/Controls/AvcControlWrite.hpp"
#include "../../Audio/Model/Topology/Validate.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"

#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include <bit>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace ASFW::UserClient {
namespace {

namespace AM = ASFW::AudioModel;
namespace C = ASFW::Audio::Controls;
using UnitPtr = std::shared_ptr<Protocols::AVC::AVCUnit>;

constexpr size_t kMaxWireSize = 4096; // DriverKit drops larger structure outputs

struct WriteResult {
    bool ready{false};
    kern_return_t status{kIOReturnSuccess};
    ControlValueKind kind{ControlValueKind::kBool};
    uint64_t bits{0};
};

/// Outlives every user client: a write may complete after its client closed,
/// exactly like the raw FCP store in AVCHandler.cpp.
struct WriteStore {
    IOLock* lock{IOLockAlloc()};
    uint64_t nextRequest{1};
    std::unordered_map<uint64_t, WriteResult> results;
};

WriteStore& Store() {
    static WriteStore store{};
    return store;
}

uint64_t Reserve() {
    auto& store = Store();
    IOLockLock(store.lock);
    if (store.results.size() > 256) std::erase_if(store.results, [](const auto& entry) { return entry.second.ready; });
    const uint64_t id = store.nextRequest++;
    store.results.emplace(id, WriteResult{});
    IOLockUnlock(store.lock);
    return id;
}

void Complete(uint64_t id, WriteResult result) {
    auto& store = Store();
    IOLockLock(store.lock);
    if (const auto found = store.results.find(id); found != store.results.end()) {
        result.ready = true;
        found->second = result;
    }
    IOLockUnlock(store.lock);
}

uint64_t GuidOf(const IOUserClientMethodArguments* args) {
    return (static_cast<uint64_t>(args->scalarInput[0]) << 32) | (args->scalarInput[1] & 0xFFFF'FFFFull);
}

UnitPtr FindUnit(Protocols::AVC::IAVCDiscovery& discovery, uint64_t guid) {
    for (auto& unit : discovery.Units()) {
        if (!unit || unit->GetDiscoveryStatus() == Protocols::AVC::AVCDiscoveryStatus::Running) continue;
        const auto device = unit->GetDevice();
        if (device && device->GetGUID() == guid) return unit;
    }
    return nullptr;
}

/// The unit's current control model. The discovery session is the revision:
/// ids are valid until the device is discovered again.
struct CurrentModel {
    UnitPtr unit;
    std::shared_ptr<const Protocols::AVC::Graph::DeviceGraph> graph;
    C::AvcControlModel model;
    uint32_t generation{0};
};

std::optional<CurrentModel> Current(Protocols::AVC::IAVCDiscovery& discovery, uint64_t guid) {
    auto unit = FindUnit(discovery, guid);
    if (!unit) return std::nullopt;
    const auto snapshot = unit->GetDiscoverySnapshot();
    auto graph = unit->GetDiscoveredGraph();
    if (!snapshot || !graph) return std::nullopt;
    CurrentModel current{.unit = std::move(unit), .graph = graph,
                         .model = C::ProjectAvcControls(*graph, snapshot->session.value),
                         .generation = snapshot->route.generation.value};
    return current;
}

std::optional<AM::ParameterValue> DecodeValue(uint64_t kind, uint64_t bits) {
    switch (static_cast<ControlValueKind>(kind)) {
        case ControlValueKind::kBool: return AM::ParameterValue{bits != 0};
        case ControlValueKind::kDouble: return AM::ParameterValue{std::bit_cast<double>(bits)};
        case ControlValueKind::kInt: return AM::ParameterValue{static_cast<int64_t>(bits)};
    }
    return std::nullopt;
}

} // namespace

kern_return_t ControlsHandler::GetControlSnapshot(IOUserClientMethodArguments* args) {
    if (!discovery_) return kIOReturnNotReady;
    if (!args || !args->scalarInput || args->scalarInputCount < 3) return kIOReturnBadArgument;
    const uint64_t guid = GuidOf(args);
    const auto current = Current(*discovery_, guid);
    if (!current) return kIOReturnUnsupported; // no AV/C projection for this device (yet)
    const auto document = Wire::BuildControlSnapshotDocument(guid, "avc", current->model.topology,
                                                             current->model.state, current->graph->featureChangeSequence);
    const auto page = Wire::SerializeDiscoveryPage(document, static_cast<uint32_t>(current->model.topology.revision),
                                                   current->generation,
                                                   static_cast<uint32_t>(args->scalarInput[2]), kMaxWireSize);
    OSData* data = OSData::withBytes(page.data(), static_cast<uint32_t>(page.size()));
    if (!data) return kIOReturnNoMemory;
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t ControlsHandler::SetControlParameter(IOUserClientMethodArguments* args) {
    if (!discovery_) return kIOReturnNotReady;
    if (!args || !args->scalarInput || args->scalarInputCount < 6 || !args->scalarOutput ||
        args->scalarOutputCount < 1) return kIOReturnBadArgument;
    const uint64_t guid = GuidOf(args);
    const uint64_t revision = args->scalarInput[2];
    const AM::ParameterId id{static_cast<uint32_t>(args->scalarInput[3])};
    const auto value = DecodeValue(args->scalarInput[4], args->scalarInput[5]);
    if (!value) return kIOReturnBadArgument;

    const auto current = Current(*discovery_, guid);
    if (!current) return kIOReturnUnsupported;
    // A different revision means the ids the caller holds belong to an older
    // discovery: refuse rather than guess what they now address.
    if (revision != current->model.topology.revision) {
        ASFW_LOG(UserClient, "[Controls] refused stale revision guid=0x%016llx have=%llu got=%llu",
                 guid, current->model.topology.revision, revision);
        return kIOReturnNotPermitted;
    }
    const auto* binding = current->model.FindBinding(id);
    if (!binding) return kIOReturnNotFound;
    if (const auto legal = AM::validateParameterWrite(current->model.topology, current->model.state, id, *value); !legal) {
        ASFW_LOG(UserClient, "[Controls] refused value guid=0x%016llx parameter=%u: %{public}s",
                 guid, id.value, legal.error().message.c_str());
        return kIOReturnBadArgument;
    }
    const auto* channel = C::FindAvcFeatureChannel(*current->graph, binding->Token());
    const auto route = current->unit->CurrentRoute();
    if (!channel || !route || !current->unit->IsCurrentRoute(*route)) return kIOReturnNotReady;

    const bool mute = binding->kind == C::AvcBindingKind::kFeatureMute;
    const int32_t raw = mute ? (std::get<bool>(*value) ? 1 : 0)
                             : static_cast<int32_t>(std::lround(std::get<double>(*value) * 256.0));
    const uint64_t request = Reserve();
    ASFW_LOG(UserClient, "[Controls] write guid=0x%016llx revision=%llu parameter=%u token=0x%06x raw=%d request=%llu",
             guid, revision, id.value, binding->Token(), raw, request);
    C::WriteAvcFeature(current->unit, *route, *channel, mute, raw, [request, mute](IOReturn status, int32_t confirmed) {
        WriteResult result{.status = status};
        if (status == kIOReturnSuccess) {
            result.kind = mute ? ControlValueKind::kBool : ControlValueKind::kDouble;
            result.bits = mute ? static_cast<uint64_t>(confirmed != 0)
                               : std::bit_cast<uint64_t>(static_cast<double>(confirmed) / 256.0);
        }
        Complete(request, result);
    });
    args->scalarOutput[0] = request;
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

kern_return_t ControlsHandler::GetControlWriteResult(IOUserClientMethodArguments* args) {
    if (!args || !args->scalarInput || args->scalarInputCount < 1 || !args->scalarOutput ||
        args->scalarOutputCount < 2) return kIOReturnBadArgument;
    auto& store = Store();
    WriteResult result{};
    bool found = false;
    IOLockLock(store.lock);
    if (const auto it = store.results.find(args->scalarInput[0]); it != store.results.end()) {
        found = true;
        result = it->second;
        if (result.ready) store.results.erase(it);
    }
    IOLockUnlock(store.lock);
    if (!found) return kIOReturnNotFound;
    if (!result.ready) return kIOReturnNotReady;
    if (result.status != kIOReturnSuccess) return result.status;
    args->scalarOutput[0] = static_cast<uint64_t>(result.kind);
    args->scalarOutput[1] = result.bits;
    args->scalarOutputCount = 2;
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
