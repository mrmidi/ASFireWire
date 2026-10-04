//
//  AVCHandler.cpp
//  ASFWDriver
//
//  Handler for AV/C Protocol API
//

#include "AVCHandler.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include "../../Protocols/AVC/AVCDefs.hpp"
#include "../../Protocols/AVC/Core/AvcFrame.hpp"
#include "../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/AVCExchangeLogWire.hpp"
#include "../WireFormats/AVCDiscoveryDocument.hpp"
#include "../WireFormats/AVCMusicCapabilities.hpp"
#include "../../Shared/SharedDataModels.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOUserClient.h>

namespace ASFW::UserClient {

namespace {

using namespace ASFW::Shared;
constexpr size_t kMaxWireSize = 4096;  // DriverKit will drop larger structure outputs
namespace E = ASFW::AVC::DiscoveryEngine;
using UnitPtr = std::shared_ptr<Protocols::AVC::AVCUnit>;

kern_return_t AvcErrorToIOReturn(const ASFW::AVC::AvcError& error) noexcept {
    return ASFW::AVC::ToIOReturn(error);
}


struct RawFCPResult {
    bool ready{false};
    kern_return_t status{kIOReturnNotReady};
    std::array<uint8_t, ASFW::Protocols::AVC::kAVCFrameMaxSize> response{};
    uint32_t responseLength{0};
};

struct RawFCPResultStore {
    IOLock* lock{IOLockAlloc()};
    uint64_t nextRequestID{1};
    std::unordered_map<uint64_t, RawFCPResult> results;
};

RawFCPResultStore& GetRawFCPResultStore() {
    static RawFCPResultStore store{};
    return store;
}

#ifdef ASFW_HOST_TEST
OSData* CastStructureInputToOSData(IOUserClientMethodArguments* args) {
    return static_cast<OSData*>(args->structureInput);
}
#else
OSData* CastStructureInputToOSData(IOUserClientMethodArguments* args) {
    return OSDynamicCast(OSData, args->structureInput);
}
#endif

struct SubunitLookupRequest {
    uint64_t guid{0};
    uint8_t type{0};
    uint8_t id{0};
};

struct RawFCPSubmissionRequest {
    uint64_t guid{0};
    OSData* commandData{nullptr};
    size_t commandLength{0};
};

std::optional<SubunitLookupRequest>
ParseSubunitLookupRequest(IOUserClientMethodArguments* args, const char* operation) {
    if (!args) {
        ASFW_LOG(UserClient, "%{public}s: null arguments", operation);
        return std::nullopt;
    }
    if (args->scalarInputCount < 4) {
        ASFW_LOG(UserClient, "%{public}s: missing inputs", operation);
        return std::nullopt;
    }

    return SubunitLookupRequest{
        .guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1],
        .type = static_cast<uint8_t>(args->scalarInput[2]),
        .id = static_cast<uint8_t>(args->scalarInput[3]),
    };
}

/// The unit with this GUID whose discovery is not running (a running
/// discovery reports nothing until it commits), or null.
UnitPtr FindFinishedUnit(Protocols::AVC::IAVCDiscovery& discovery, uint64_t guid) {
    for (auto& unit : discovery.Units()) {
        if (!unit || unit->GetDiscoveryStatus() == Protocols::AVC::AVCDiscoveryStatus::Running) continue;
        const auto device = unit->GetDevice();
        if (device && device->GetGUID() == guid) return unit;
    }
    return nullptr;
}

/// The committed snapshot holding the requested subunit, or null.
E::SnapshotLease FindSubunitSnapshot(Protocols::AVC::IAVCDiscovery& discovery,
                                     const SubunitLookupRequest& request) {
    const auto unit = FindFinishedUnit(discovery, request.guid);
    auto snapshot = unit ? unit->GetDiscoverySnapshot() : nullptr;
    if (!snapshot || !snapshot->unit.FindSubunit(static_cast<ASFW::AVC::SubunitType>(request.type), request.id))
        return nullptr;
    return snapshot;
}

kern_return_t ReturnBytes(IOUserClientMethodArguments* args, std::span<const uint8_t> bytes) {
    OSData* data = OSData::withBytes(bytes.data(), static_cast<uint32_t>(bytes.size()));
    if (!data) return kIOReturnNoMemory;
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

std::optional<RawFCPSubmissionRequest>
ParseRawFCPSubmissionRequest(IOUserClientMethodArguments* args) {
    if (!args) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: null arguments");
        return std::nullopt;
    }
    if (!args->scalarInput || args->scalarInputCount < 2) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: missing scalar inputs");
        return std::nullopt;
    }
    if (!args->scalarOutput || args->scalarOutputCount < 1) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: missing scalar output buffer");
        return std::nullopt;
    }
    if (!args->structureInput) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: missing command payload");
        return std::nullopt;
    }

    OSData* commandData = CastStructureInputToOSData(args);
    if (!commandData) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: structureInput is not OSData");
        return std::nullopt;
    }

    const size_t commandLength = static_cast<size_t>(commandData->getLength());
    if (commandLength < ASFW::Protocols::AVC::kAVCFrameMinSize ||
        commandLength > ASFW::Protocols::AVC::kAVCFrameMaxSize) {
        ASFW_LOG(UserClient,
                 "SendRawFCPCommand: invalid payload size=%llu",
                 static_cast<unsigned long long>(commandLength));
        return std::nullopt;
    }

    return RawFCPSubmissionRequest{
        .guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1],
        .commandData = commandData,
        .commandLength = commandLength,
    };
}

uint64_t ReserveRawFCPRequestSlot(RawFCPResultStore& store) {
    IOLockLock(store.lock);
    if (store.results.size() > 256) {
        for (auto it = store.results.begin(); it != store.results.end();) {
            if (it->second.ready) {
                it = store.results.erase(it);
            } else {
                ++it;
            }
        }
    }

    const uint64_t requestID = store.nextRequestID++;
    store.results.emplace(requestID, RawFCPResult{});
    IOLockUnlock(store.lock);
    return requestID;
}

void StoreRawFCPCompletion(uint64_t requestID,
                           const ASFW::AVC::Expected<ASFW::AVC::Response>& response) {
    auto& resultStore = GetRawFCPResultStore();
    if (!resultStore.lock) {
        return;
    }

    IOLockLock(resultStore.lock);
    const auto it = resultStore.results.find(requestID);
    if (it != resultStore.results.end()) {
        it->second.ready = true;
        if (!response) {
            it->second.status = AvcErrorToIOReturn(response.error());
            it->second.responseLength = 0;
        } else {
            it->second.status = kIOReturnSuccess;
            it->second.response[0] = static_cast<uint8_t>(response->code);
            it->second.response[1] = response->address.Byte();
            it->second.response[2] = static_cast<uint8_t>(response->opcode);
            const size_t opsLen = std::min(response->operands.size(), it->second.response.size() - 3);
            if (opsLen > 0) {
                std::memcpy(it->second.response.data() + 3, response->operands.data(), opsLen);
            }
            it->second.responseLength = static_cast<uint32_t>(3 + opsLen);
        }
    }
    IOLockUnlock(resultStore.lock);
}


} // anonymous namespace

AVCHandler::AVCHandler(Protocols::AVC::IAVCDiscovery* discovery)
    : discovery_(discovery)
{
}

kern_return_t AVCHandler::GetAVCUnits(IOUserClientMethodArguments* args) {
    if (!args) {
        ASFW_LOG(UserClient, "GetAVCUnits: null arguments");
        return kIOReturnBadArgument;
    }
    if (!discovery_) {
        ASFW_LOG(UserClient, "GetAVCUnits: discovery not available");
        return kIOReturnNotReady;
    }

    // uint32 unit count, then per unit an AVCUnitInfoWire followed by its
    // AVCSubunitInfoWire entries. A running discovery reports no subunits or
    // plug counts until it commits.
    const auto units = discovery_->Units();
    std::vector<uint8_t> out(sizeof(uint32_t));
    const auto count = static_cast<uint32_t>(units.size());
    std::memcpy(out.data(), &count, sizeof(count));
    for (const auto& unit : units) {
        if (!unit) continue;
        AVCUnitInfoWire unitWire{};
        if (const auto device = unit->GetDevice()) {
            unitWire.guid = device->GetGUID();
            unitWire.nodeID = device->GetNodeID();
            unitWire.vendorID = device->GetVendorID();
            unitWire.modelID = device->GetModelID();
        } else {
            unitWire.nodeID = 0xFFFF;
        }
        const auto status = unit->GetDiscoveryStatus();
        const auto snapshot = status == Protocols::AVC::AVCDiscoveryStatus::Running ? nullptr : unit->GetDiscoverySnapshot();
        if (snapshot) {
            unitWire.subunitCount = static_cast<uint8_t>(snapshot->unit.subunits.size());
            const auto& plugs = snapshot->unit.unitPlugs;
            unitWire.isoInputPlugs = plugs.isochronousInputs;
            unitWire.isoOutputPlugs = plugs.isochronousOutputs;
            unitWire.extInputPlugs = plugs.externalInputs;
            unitWire.extOutputPlugs = plugs.externalOutputs;
        }
        // Status is an additive diagnostic in the former reserved byte.
        unitWire.discoveryStatus = static_cast<uint8_t>(0x80u | static_cast<uint8_t>(status));
        const auto* unitBytes = reinterpret_cast<const uint8_t*>(&unitWire);
        out.insert(out.end(), unitBytes, unitBytes + sizeof(unitWire));
        if (!snapshot) continue;
        for (const auto& sub : snapshot->unit.subunits) {
            AVCSubunitInfoWire subunitWire{};
            subunitWire.type = static_cast<uint8_t>(sub.id.type);
            subunitWire.subunitID = sub.id.id;
            subunitWire.numDestPlugs = sub.plugs.destinationPlugs;
            subunitWire.numSrcPlugs = sub.plugs.sourcePlugs;
            const auto* subunitBytes = reinterpret_cast<const uint8_t*>(&subunitWire);
            out.insert(out.end(), subunitBytes, subunitBytes + sizeof(subunitWire));
        }
    }
    ASFW_LOG_V3(UserClient, "GetAVCUnits: returning %zu units in %zu bytes", units.size(), out.size());
    return ReturnBytes(args, out);
}

kern_return_t AVCHandler::GetSubunitCapabilities(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }
    const auto request = ParseSubunitLookupRequest(args, "GetSubunitCapabilities");
    if (!request) {
        return kIOReturnBadArgument;
    }
    const auto snapshot = FindSubunitSnapshot(*discovery_, *request);
    if (!snapshot) {
        return kIOReturnNotFound;
    }
    if (request->type != static_cast<uint8_t>(ASFW::AVC::SubunitType::kMusic)) {
        ASFW_LOG(UserClient, "GetSubunitCapabilities: not implemented for subunit type 0x%02x", request->type);
        return kIOReturnUnsupported;
    }
    const auto blob = Wire::BuildMusicCapabilities(
        *snapshot, {ASFW::AVC::SubunitType::kMusic, request->id}, kMaxWireSize);
    if (!blob) {
        return kIOReturnNotFound;
    }
    return ReturnBytes(args, *blob);
}

kern_return_t AVCHandler::GetSubunitDescriptor(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }
    const auto request = ParseSubunitLookupRequest(args, "GetSubunitDescriptor");
    if (!request) {
        return kIOReturnBadArgument;
    }
    const auto snapshot = FindSubunitSnapshot(*discovery_, *request);
    if (!snapshot) {
        ASFW_LOG(UserClient, "GetSubunitDescriptor: subunit not found (GUID=0x%llx type=0x%02x id=%d)",
                 request->guid, request->type, request->id);
        return kIOReturnNotFound;
    }
    // The music subunit's status descriptor, or the audio subunit's identifier
    // descriptor, as discovery read it (cached; nothing is sent).
    const ASFW::AVC::SubunitId id{static_cast<ASFW::AVC::SubunitType>(request->type), request->id};
    const bool music = id.type == ASFW::AVC::SubunitType::kMusic;
    if (!music && id.type != ASFW::AVC::SubunitType::kAudio) {
        ASFW_LOG(UserClient, "GetSubunitDescriptor: not implemented for subunit type 0x%02x", request->type);
        return kIOReturnUnsupported;
    }
    const std::vector<uint8_t>* bytes = nullptr;
    for (const auto& blob : snapshot->descriptors) {
        if (blob.subunit != id || blob.primaryError) continue;
        // The Music subunit's STATUS descriptor (its identifier descriptor is read too, and is in the discovery
        // document); the Audio subunit's identifier.
        const auto wanted = music ? ASFW::AVC::Cmd::DescriptorSpecifier::SubunitStatus()
                                  : ASFW::AVC::Cmd::DescriptorSpecifier::SubunitIdentifier();
        if (blob.specifier == wanted && !blob.bytes.empty())
            bytes = &blob.bytes; // The last read wins, as discovery recorded them.
    }
    if (!bytes) {
        ASFW_LOG(UserClient, "GetSubunitDescriptor: descriptor data not available");
        return kIOReturnNotFound;
    }
    if (bytes->size() > kMaxWireSize) {
        ASFW_LOG_ERROR(UserClient, "GetSubunitDescriptor: descriptor size %zu exceeds wire limit %zu",
                       bytes->size(), kMaxWireSize);
        return kIOReturnMessageTooLarge;
    }
    ASFW_LOG(UserClient, "GetSubunitDescriptor: returning %zu bytes", bytes->size());
    return ReturnBytes(args, *bytes);
}

kern_return_t AVCHandler::SendRawFCPCommand(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: discovery not available");
        return kIOReturnNotReady;
    }

    const auto request = ParseRawFCPSubmissionRequest(args);
    if (!request) {
        return kIOReturnBadArgument;
    }

    const auto targetUnit = FindFinishedUnit(*discovery_, request->guid);
    if (!targetUnit) {
        ASFW_LOG(UserClient,
                 "SendRawFCPCommand: target unit not found (guid=0x%llx)",
                 request->guid);
        return kIOReturnNotFound;
    }

    if (request->commandLength < 3) {
        return kIOReturnBadArgument;
    }

    const auto* rawBytes = static_cast<const uint8_t*>(request->commandData->getBytesNoCopy());
    const auto ctype = static_cast<ASFW::AVC::CommandType>(rawBytes[0] & 0x0F);
    const auto addr = ASFW::AVC::SubunitAddress::FromByte(rawBytes[1]);
    const auto opcode = static_cast<ASFW::AVC::Opcode>(rawBytes[2]);
    const std::span<const uint8_t> operands{rawBytes + 3, request->commandLength - 3};

    auto frame = ASFW::AVC::CommandFrame::Make(ctype, addr, opcode, operands);
    if (!frame) {
        return kIOReturnBadArgument;
    }

    auto& store = GetRawFCPResultStore();
    if (!store.lock) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: result store lock unavailable");
        return kIOReturnNoMemory;
    }

    const uint64_t requestID = ReserveRawFCPRequestSlot(store);

    targetUnit->Submit(
        *frame,
        targetUnit->CurrentGeneration(),
        [requestID](ASFW::AVC::Expected<ASFW::AVC::Response> response) {
            StoreRawFCPCompletion(requestID, response);
        });

    args->scalarOutput[0] = requestID;
    args->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

kern_return_t AVCHandler::GetRawFCPCommandResult(IOUserClientMethodArguments* args) {
    if (!args || !args->scalarInput || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }

    auto& store = GetRawFCPResultStore();
    if (!store.lock) {
        return kIOReturnNoMemory;
    }

    const uint64_t requestID = args->scalarInput[0];
    RawFCPResult result{};
    bool found = false;

    IOLockLock(store.lock);
    auto it = store.results.find(requestID);
    if (it != store.results.end()) {
        found = true;
        if (it->second.ready) {
            result = it->second;
            store.results.erase(it);
        }
    }
    IOLockUnlock(store.lock);

    if (!found) {
        return kIOReturnNotFound;
    }

    if (!result.ready) {
        return kIOReturnNotReady;
    }

    if (result.status != kIOReturnSuccess) {
        return result.status;
    }

    OSData* response = OSData::withBytes(result.response.data(), result.responseLength);
    if (!response) {
        return kIOReturnNoMemory;
    }

    args->structureOutput = response;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t AVCHandler::GetFCPExchangeLog(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }
    if (!args || args->scalarInputCount < 3) {
        return kIOReturnBadArgument;
    }
    const uint64_t guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1];
    const auto firstIndex = static_cast<uint32_t>(args->scalarInput[2]);

    for (const auto& unit : discovery_->Units()) {
        const auto device = unit ? unit->GetDevice() : nullptr;
        if (!device || device->GetGUID() != guid) {
            continue;
        }
        const auto page = Wire::SerializeExchangePage(unit->CopyExchangeLog(), firstIndex, kMaxWireSize);
        OSData* osData = OSData::withBytes(page.data(), static_cast<uint32_t>(page.size()));
        if (!osData) {
            return kIOReturnNoMemory;
        }
        args->structureOutput = osData;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }
    return kIOReturnNotFound;
}

kern_return_t AVCHandler::GetAVCDiscoveryDocument(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }
    if (!args || args->scalarInputCount < 3) {
        return kIOReturnBadArgument;
    }
    const uint64_t guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1];
    const auto offset = static_cast<uint32_t>(args->scalarInput[2]);

    for (const auto& unit : discovery_->Units()) {
        const auto device = unit ? unit->GetDevice() : nullptr;
        if (!device || device->GetGUID() != guid) {
            continue;
        }
        // Built from immutable discovery results; a running discovery shows the
        // previous committed snapshot, never a half-built one.
        const auto snapshot = unit->GetDiscoverySnapshot();
        const auto graph = unit->GetDiscoveredGraph();
        const auto document = Wire::BuildAVCDiscoveryDocument(snapshot.get(), graph.get(), unit->CopyExchangeLog());
        const auto page = Wire::SerializeDiscoveryPage(
            document, snapshot ? static_cast<uint32_t>(snapshot->session.value) : 0,
            snapshot ? snapshot->route.generation.value : 0, offset, kMaxWireSize);
        OSData* osData = OSData::withBytes(page.data(), static_cast<uint32_t>(page.size()));
        if (!osData) {
            return kIOReturnNoMemory;
        }
        args->structureOutput = osData;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }
    return kIOReturnNotFound;
}

kern_return_t AVCHandler::ReScanAVCUnits(IOUserClientMethodArguments* args) {
    (void)args; // Unused
    
    if (!discovery_) return kIOReturnNotReady;

    ASFW_LOG(UserClient, "ReScanAVCUnits: triggering re-scan");
    discovery_->ReScanAllUnits();
    
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
