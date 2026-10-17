// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlsHandler.cpp - see ControlsHandler.hpp.

#include "ControlsHandler.hpp"
#include "../../Audio/Controls/ControlService.hpp"
#include "../../Audio/Controls/CoreAudioProjection.hpp"

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
#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace ASFW::UserClient {
namespace {

namespace AM = ASFW::AudioModel;
namespace C = ASFW::Audio::Controls;

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

std::optional<AM::ParameterValue> DecodeValue(uint64_t kind, uint64_t bits) {
    switch (static_cast<ControlValueKind>(kind)) {
        case ControlValueKind::kBool: if (bits > 1) return std::nullopt; return AM::ParameterValue{bits != 0};
        case ControlValueKind::kDouble: return AM::ParameterValue{std::bit_cast<double>(bits)};
        case ControlValueKind::kInt: return AM::ParameterValue{static_cast<int64_t>(bits)};
        case ControlValueKind::kAction: if (bits != 0) return std::nullopt; return AM::ActionTrigger{};
    }
    return std::nullopt;
}

} // namespace

kern_return_t ControlsHandler::GetControlSnapshot(IOUserClientMethodArguments* args) {
    if (!discovery_) return kIOReturnNotReady;
    if (!args || !args->scalarInput || args->scalarInputCount < 3) return kIOReturnBadArgument;
    const uint64_t guid = GuidOf(args);
    const auto current = C::ControlService(discovery_, lookup_).CopySnapshot(guid);
    if (!current) return kIOReturnUnsupported;
    const auto document = Wire::BuildControlSnapshotDocument(guid, current->family, current->topology,
                                                             current->state, current->sequence);
    const auto page = Wire::SerializeDiscoveryPage(document, static_cast<uint32_t>(current->topology.revision),
                                                   0,
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
    if (args->scalarInput[3] > UINT32_MAX) return kIOReturnBadArgument;
    const AM::ParameterId id{static_cast<uint32_t>(args->scalarInput[3])};
    const auto value = DecodeValue(args->scalarInput[4], args->scalarInput[5]);
    if (!value) return kIOReturnBadArgument;

    const auto current = C::ControlService(discovery_, lookup_).CopySnapshot(guid);
    if (!current) return kIOReturnUnsupported;
    if (revision != current->topology.revision) return kIOReturnNotPermitted;
    if ((id.value & 0x80000000u) != 0) {
        const auto controls = C::ProjectCoreAudioControls(current->topology, current->state);
        const auto found = std::ranges::find(controls, id, &C::CoreAudioControl::parameter);
        if (found == controls.end()) return kIOReturnNotFound;
        if (!found->settable) return kIOReturnNotWritable;
        const AM::Parameter projected{id, AM::NodeId{0}, AM::ParameterSemantic::NominalLevel, found->domain, found->name};
        if (!AM::validateParameterValue(projected, *value)) return kIOReturnBadArgument;
    } else if (const auto valid = AM::validateParameterWrite(current->topology, current->state, id, *value); !valid) {
        const auto kind = valid.error().kind;
        if (kind == AM::StateErrorKind::NonexistentParameter) return kIOReturnNotFound;
        return kind == AM::StateErrorKind::ReadOnlyParameter || kind == AM::StateErrorKind::ParameterNotApplicable
            ? kIOReturnNotWritable : kIOReturnBadArgument;
    }
    const uint64_t request = Reserve();
    C::ControlService(discovery_, lookup_).SetParameter(guid, revision, id, *value,
        [request](IOReturn status, AM::ParameterValue confirmed) {
            WriteResult result{.status = status};
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool>) { result.kind = ControlValueKind::kBool; result.bits = value; }
                else if constexpr (std::is_same_v<T, double>) { result.kind = ControlValueKind::kDouble; result.bits = std::bit_cast<uint64_t>(value); }
                else if constexpr (std::is_same_v<T, int64_t>) { result.kind = ControlValueKind::kInt; result.bits = static_cast<uint64_t>(value); }
                else { result.kind = ControlValueKind::kAction; result.bits = 0; }
            }, confirmed);
            Complete(request, result);
        });
    args->scalarOutput[0] = request;
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

kern_return_t ControlsHandler::SetControlRoute(IOUserClientMethodArguments* args) {
    if (!args || !args->scalarInput || args->scalarInputCount != 6 || !args->scalarOutput ||
        args->scalarOutputCount < 1 || args->scalarInput[3] > UINT32_MAX || args->scalarInput[4] > UINT32_MAX ||
        (args->scalarInput[5] != UINT64_MAX && args->scalarInput[5] > UINT32_MAX)) return kIOReturnBadArgument;
    const uint64_t guid = GuidOf(args), revision = args->scalarInput[2];
    const AM::NodeId router{static_cast<uint32_t>(args->scalarInput[3])};
    const AM::PortId output{static_cast<uint32_t>(args->scalarInput[4])};
    const auto input = args->scalarInput[5] == UINT64_MAX ? std::nullopt :
        std::optional{AM::PortId{static_cast<uint32_t>(args->scalarInput[5])}};
    const auto current = C::ControlService(discovery_, lookup_).CopySnapshot(guid);
    if (!current) return kIOReturnUnsupported;
    if (current->topology.revision != revision) return kIOReturnNotPermitted;
    if (const auto valid = AM::prepareRouteWrite(current->topology, current->state, router, output, input); !valid)
        return valid.error().kind == AM::StateErrorKind::ReadOnlyParameter ? kIOReturnNotWritable : kIOReturnBadArgument;
    const auto request = Reserve();
    C::ControlService(discovery_, lookup_).SetRoute(guid, revision, router, output, input,
        [request](auto status, const auto& confirmed) {
            const auto* port = std::get_if<int64_t>(&confirmed);
            Complete(request, WriteResult{.status = status == kIOReturnSuccess && !port ? kIOReturnError : status,
                .kind = ControlValueKind::kInt, .bits = port ? static_cast<uint64_t>(*port) : 0});
        });
    args->scalarOutput[0] = request; args->scalarOutputCount = 1;
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
