//
//  AVCHandler.cpp
//  ASFWDriver
//
//  Handler for AV/C Protocol API
//

#include "AVCHandler.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include "../../Protocols/AVC/Music/MusicSubunit.hpp"
#include "../../Protocols/AVC/Audio/AudioSubunit.hpp"
#include "../../Protocols/AVC/AVCDefs.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/SharedDataModels.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOUserClient.h>

namespace ASFW::UserClient {

namespace {

using namespace ASFW::Shared;
constexpr size_t kMaxWireSize = 4096;  // DriverKit will drop larger structure outputs
using MusicSubunit = ASFW::Protocols::AVC::Music::MusicSubunit;
using MusicPlugInfo = MusicSubunit::PlugInfo;
using MusicPlugChannel = MusicSubunit::MusicPlugChannel;
using SubunitPtr = std::shared_ptr<ASFW::Protocols::AVC::Subunit>;

kern_return_t FCPStatusToIOReturn(ASFW::Protocols::AVC::FCPStatus status) {
    using ASFW::Protocols::AVC::FCPStatus;

    switch (status) {
        case FCPStatus::kOk:
            return kIOReturnSuccess;
        case FCPStatus::kTimeout:
            return kIOReturnTimeout;
        case FCPStatus::kBusReset:
            return kIOReturnAborted;
        case FCPStatus::kTransportError:
            return kIOReturnIOError;
        case FCPStatus::kInvalidPayload:
            return kIOReturnBadArgument;
        case FCPStatus::kResponseMismatch:
            return kIOReturnInvalid;
        case FCPStatus::kBusy:
            return kIOReturnBusy;
    }

    return kIOReturnError;
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

struct PlugSerializeInfo {
    size_t plugSize{sizeof(PlugInfoWire)};
    uint8_t numBlocks{0};
    std::vector<uint8_t> channelCounts;
    uint8_t numSupportedFormats{0};
};

struct MusicRateSummary {
    uint8_t currentRate{0xFF};
    uint32_t supportedMask{0};
};

struct MusicSerializationPlan {
    MusicRateSummary rates{};
    size_t totalSize{sizeof(AVCMusicCapabilitiesWire)};
    size_t numPlugsToSerialize{0};
    std::vector<PlugSerializeInfo> plugInfos;
};

bool IsMusicSubunitType(ASFW::Protocols::AVC::AVCSubunitType type) noexcept {
    using ASFW::Protocols::AVC::AVCSubunitType;
    return type == AVCSubunitType::kMusic || type == AVCSubunitType::kMusic0C;
}

std::optional<SubunitLookupRequest>
ParseSubunitLookupRequest(IOUserClientMethodArguments* args, const char* operation) {
    if (!args) {
        ASFW_LOG(UserClient, "%s: null arguments", operation);
        return std::nullopt;
    }
    if (args->scalarInputCount < 4) {
        ASFW_LOG(UserClient, "%s: missing inputs", operation);
        return std::nullopt;
    }

    return SubunitLookupRequest{
        .guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1],
        .type = static_cast<uint8_t>(args->scalarInput[2]),
        .id = static_cast<uint8_t>(args->scalarInput[3]),
    };
}

SubunitPtr FindRequestedSubunit(Protocols::AVC::IAVCDiscovery& discovery,
                                const SubunitLookupRequest& request) {
    const auto allUnits = discovery.GetAllAVCUnits();
    for (auto* unit : allUnits) {
        if (!unit) {
            continue;
        }

        auto device = unit->GetDevice();
        if (!device || device->GetGUID() != request.guid) {
            continue;
        }

        for (const auto& subunit : unit->GetSubunits()) {
            if (!subunit) {
                continue;
            }
            if (static_cast<uint8_t>(subunit->GetType()) == request.type &&
                subunit->GetID() == request.id) {
                return subunit;
            }
        }
    }

    return {};
}

MusicRateSummary CollectMusicRateSummary(const std::vector<MusicPlugInfo>& plugs) {
    using SampleRate = ASFW::Protocols::AVC::StreamFormats::SampleRate;

    MusicRateSummary summary{};
    for (const auto& plug : plugs) {
        if (plug.currentFormat && plug.currentFormat->sampleRate != SampleRate::kUnknown &&
            summary.currentRate == 0xFF) {
            summary.currentRate = static_cast<uint8_t>(plug.currentFormat->sampleRate);
        }

        for (const auto& fmt : plug.supportedFormats) {
            if (fmt.sampleRate == SampleRate::kUnknown) {
                continue;
            }
            const uint8_t rate = static_cast<uint8_t>(fmt.sampleRate);
            if (rate < 32) {
                summary.supportedMask |= (1u << rate);
            }
        }
    }

    return summary;
}

PlugSerializeInfo BuildPlugSerializeInfo(const MusicPlugInfo& plug) {
    PlugSerializeInfo info{};

    if (plug.currentFormat) {
        if (plug.currentFormat->IsCompound()) {
            info.numBlocks = static_cast<uint8_t>(
                std::min(plug.currentFormat->channelFormats.size(), size_t(255)));
            for (size_t b = 0; b < info.numBlocks; ++b) {
                const auto& block = plug.currentFormat->channelFormats[b];
                const uint8_t numChannelDetails = static_cast<uint8_t>(
                    std::min(block.channels.size(), size_t(255)));
                info.channelCounts.push_back(numChannelDetails);
                info.plugSize += sizeof(SignalBlockWire) +
                    numChannelDetails * sizeof(ChannelDetailWire);
            }
        } else if (plug.currentFormat->totalChannels > 0) {
            info.numBlocks = 1;
            info.channelCounts.push_back(0);
            info.plugSize += sizeof(SignalBlockWire);
        }
    }

    info.numSupportedFormats = static_cast<uint8_t>(
        std::min(plug.supportedFormats.size(), size_t(32)));
    info.plugSize += info.numSupportedFormats * sizeof(SupportedFormatWire);
    return info;
}

MusicSerializationPlan BuildMusicSerializationPlan(const std::vector<MusicPlugInfo>& plugs) {
    MusicSerializationPlan plan{};
    plan.rates = CollectMusicRateSummary(plugs);
    plan.plugInfos.reserve(plugs.size());

    for (const auto& plug : plugs) {
        const auto info = BuildPlugSerializeInfo(plug);
        if (plan.totalSize + info.plugSize > kMaxWireSize) {
            break;
        }

        plan.totalSize += info.plugSize;
        plan.plugInfos.push_back(info);
        ++plan.numPlugsToSerialize;
    }

    return plan;
}

std::unordered_map<uint16_t, std::string>
BuildChannelNameLookup(const std::vector<MusicPlugChannel>& channels) {
    std::unordered_map<uint16_t, std::string> lookup;
    for (const auto& channel : channels) {
        lookup[channel.musicPlugID] = channel.name;
    }
    return lookup;
}

std::string ResolveChannelName(
    const std::string& channelName,
    uint16_t musicPlugID,
    const std::unordered_map<uint16_t, std::string>& channelNameLookup) {
    if (!channelName.empty()) {
        return channelName;
    }

    const auto it = channelNameLookup.find(musicPlugID);
    if (it != channelNameLookup.end()) {
        return it->second;
    }

    return {};
}

bool AppendMusicCapabilitiesHeader(
    OSData* data,
    const ASFW::Protocols::AVC::Music::MusicSubunitCapabilities& caps,
    const MusicSerializationPlan& plan) {
    AVCMusicCapabilitiesWire wire{};
    wire.hasAudio = caps.hasAudioCapability ? 1 : 0;
    wire.hasMIDI = caps.hasMidiCapability ? 1 : 0;
    wire.hasSMPTE = caps.hasSmpteTimeCodeCapability ? 1 : 0;
    wire.audioInputPorts = caps.maxAudioInputChannels.value_or(0);
    wire.audioOutputPorts = caps.maxAudioOutputChannels.value_or(0);
    wire.midiInputPorts = caps.maxMidiInputPorts.value_or(0);
    wire.midiOutputPorts = caps.maxMidiOutputPorts.value_or(0);
    wire.smpteInputPorts = 0;
    wire.smpteOutputPorts = 0;
    wire.currentRate = plan.rates.currentRate;
    wire.supportedRatesMask = plan.rates.supportedMask;
    wire.numPlugs = static_cast<uint8_t>(plan.numPlugsToSerialize);
    wire._reserved = 0;
    return data->appendBytes(&wire, sizeof(wire));
}

bool AppendCompoundSignalBlocks(
    OSData* data,
    const MusicPlugInfo& plug,
    const PlugSerializeInfo& info,
    const std::unordered_map<uint16_t, std::string>& channelNameLookup) {
    for (size_t b = 0; b < info.numBlocks; ++b) {
        if (b >= plug.currentFormat->channelFormats.size()) {
            break;
        }

        const auto& block = plug.currentFormat->channelFormats[b];
        const uint8_t numChannelDetails =
            (b < info.channelCounts.size()) ? info.channelCounts[b] : 0;

        SignalBlockWire blockWire{};
        blockWire.formatCode = static_cast<uint8_t>(block.formatCode);
        blockWire.channelCount = block.channelCount;
        blockWire.numChannelDetails = numChannelDetails;
        blockWire._padding = 0;
        if (!data->appendBytes(&blockWire, sizeof(blockWire))) {
            return false;
        }

        for (size_t c = 0; c < blockWire.numChannelDetails; ++c) {
            if (c >= block.channels.size()) {
                break;
            }

            const auto& channel = block.channels[c];
            ChannelDetailWire channelWire{};
            channelWire.musicPlugID = channel.musicPlugID;
            channelWire.position = channel.position;
            const std::string channelName =
                ResolveChannelName(channel.name, channel.musicPlugID, channelNameLookup);

            const size_t nameCopyLen = std::min(channelName.length(), sizeof(channelWire.name) - 1);
            std::memcpy(channelWire.name, channelName.c_str(), nameCopyLen);
            channelWire.name[nameCopyLen] = '\0';
            channelWire.nameLength = static_cast<uint8_t>(nameCopyLen);
            if (!data->appendBytes(&channelWire, sizeof(channelWire))) {
                return false;
            }
        }
    }

    return true;
}

bool AppendSignalBlocks(OSData* data,
                        const MusicPlugInfo& plug,
                        const PlugSerializeInfo& info,
                        const std::unordered_map<uint16_t, std::string>& channelNameLookup) {
    if (info.numBlocks == 0 || !plug.currentFormat) {
        return true;
    }

    if (plug.currentFormat->IsCompound()) {
        return AppendCompoundSignalBlocks(data, plug, info, channelNameLookup);
    }

    SignalBlockWire blockWire{};
    blockWire.formatCode = 0x06;
    blockWire.channelCount = plug.currentFormat->totalChannels;
    blockWire.numChannelDetails = 0;
    blockWire._padding = 0;
    return data->appendBytes(&blockWire, sizeof(blockWire));
}

bool AppendSupportedFormats(OSData* data,
                            const MusicPlugInfo& plug,
                            uint8_t numSupportedFormats) {
    using StreamFormatCode = ASFW::Protocols::AVC::StreamFormats::StreamFormatCode;

    for (size_t s = 0; s < numSupportedFormats; ++s) {
        if (s >= plug.supportedFormats.size()) {
            break;
        }

        const auto& fmt = plug.supportedFormats[s];
        SupportedFormatWire formatWire{};
        formatWire.sampleRateCode = static_cast<uint8_t>(fmt.sampleRate);
        formatWire.formatCode = static_cast<uint8_t>(
            fmt.channelFormats.empty() ? StreamFormatCode::kMBLA : fmt.channelFormats[0].formatCode);
        formatWire.channelCount = fmt.totalChannels;
        formatWire._padding = 0;
        if (!data->appendBytes(&formatWire, sizeof(formatWire))) {
            return false;
        }
    }

    return true;
}

bool AppendMusicPlug(OSData* data,
                     const MusicPlugInfo& plug,
                     const PlugSerializeInfo& info,
                     const std::unordered_map<uint16_t, std::string>& channelNameLookup) {
    PlugInfoWire plugWire{};
    plugWire.plugID = plug.plugID;
    plugWire.isInput = plug.IsInput() ? 1 : 0;
    plugWire.type = static_cast<uint8_t>(plug.type);
    plugWire.numSignalBlocks = info.numBlocks;
    plugWire.numSupportedFormats = info.numSupportedFormats;

    const size_t copyLen = std::min(plug.name.length(), sizeof(plugWire.name) - 1);
    std::memcpy(plugWire.name, plug.name.c_str(), copyLen);
    plugWire.name[copyLen] = '\0';
    plugWire.nameLength = static_cast<uint8_t>(copyLen);
    if (!data->appendBytes(&plugWire, sizeof(plugWire))) {
        return false;
    }

    if (!AppendSignalBlocks(data, plug, info, channelNameLookup)) {
        return false;
    }

    return AppendSupportedFormats(data, plug, info.numSupportedFormats);
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

Protocols::AVC::AVCUnit* FindAVCUnitByGuid(Protocols::AVC::IAVCDiscovery& discovery, uint64_t guid) {
    const auto allUnits = discovery.GetAllAVCUnits();
    for (auto* unit : allUnits) {
        if (!unit) {
            continue;
        }

        auto device = unit->GetDevice();
        if (device && device->GetGUID() == guid) {
            return unit;
        }
    }

    return nullptr;
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
                           Protocols::AVC::FCPStatus status,
                           const Protocols::AVC::FCPFrame& response) {
    auto& resultStore = GetRawFCPResultStore();
    if (!resultStore.lock) {
        return;
    }

    IOLockLock(resultStore.lock);
    const auto it = resultStore.results.find(requestID);
    if (it != resultStore.results.end()) {
        it->second.ready = true;
        it->second.status = FCPStatusToIOReturn(status);
        if (status == Protocols::AVC::FCPStatus::kOk && response.IsValid()) {
            it->second.responseLength = static_cast<uint32_t>(response.length);
            std::memcpy(it->second.response.data(), response.data.data(), response.length);
        } else {
            it->second.responseLength = 0;
        }
    }
    IOLockUnlock(resultStore.lock);
}

void MarkRawFCPRequestFailed(RawFCPResultStore& store, uint64_t requestID) {
    IOLockLock(store.lock);
    const auto it = store.results.find(requestID);
    if (it != store.results.end()) {
        it->second.ready = true;
        if (it->second.status == kIOReturnNotReady) {
            it->second.status = kIOReturnIOError;
        }
    }
    IOLockUnlock(store.lock);
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

    // Get all AV/C units
    auto allUnits = discovery_->GetAllAVCUnits();

    ASFW_LOG(UserClient, "GetAVCUnits: found %zu AV/C units", allUnits.size());

    // Calculate total size
    // We send an OSData containing a sequence of AVCUnitInfoWire structures.
    // Each AVCUnitInfoWire is followed by N * AVCSubunitInfoWire.
    size_t totalSize = 0;
    
    // Add header size (count of units)? 
    // The previous implementation had a header. The new spec doesn't explicitly define a top-level header,
    // but usually we send an array.
    // Let's assume the UI expects just the sequence of units, or we can add a simple count at the start.
    // The proposed AVCUnitInfoWire doesn't have a "next" pointer, so we rely on the buffer size or a count.
    // Let's prepend a uint32_t count for safety/easier parsing.
    totalSize += sizeof(uint32_t);

    for (auto* avcUnit : allUnits) {
        if (avcUnit) {
            totalSize += sizeof(AVCUnitInfoWire);
            totalSize += avcUnit->GetSubunits().size() * sizeof(AVCSubunitInfoWire);
        }
    }

    ASFW_LOG(UserClient, "GetAVCUnits: total wire format size=%zu bytes", totalSize);

    // Create OSData buffer
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) {
        ASFW_LOG(UserClient, "GetAVCUnits: failed to allocate OSData");
        return kIOReturnNoMemory;
    }

    // Write unit count
    uint32_t unitCount = static_cast<uint32_t>(allUnits.size());
    if (!data->appendBytes(&unitCount, sizeof(unitCount))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write each AV/C unit + its subunits
    for (auto* avcUnit : allUnits) {
        if (!avcUnit) continue;

        AVCUnitInfoWire unitWire{};
        
        // Get device from AVCUnit
        auto device = avcUnit->GetDevice();
        if (device) {
            unitWire.guid = device->GetGUID();
            unitWire.nodeID = device->GetNodeID();
            unitWire.vendorID = device->GetVendorID();
            unitWire.modelID = device->GetModelID();
        } else {
            unitWire.guid = 0;
            unitWire.nodeID = 0xFFFF;
            unitWire.vendorID = 0;
            unitWire.modelID = 0;
        }

        const auto& subunits = avcUnit->GetSubunits();
        unitWire.subunitCount = static_cast<uint8_t>(subunits.size());
        
        // Populate unit-level plug counts from AVCUnitPlugInfoCommand results
        const auto& plugCounts = avcUnit->GetCachedPlugCounts();
        unitWire.isoInputPlugs = plugCounts.isoInputPlugs;
        unitWire.isoOutputPlugs = plugCounts.isoOutputPlugs;
        unitWire.extInputPlugs = plugCounts.extInputPlugs;
        unitWire.extOutputPlugs = plugCounts.extOutputPlugs;
        // unitWire._reserved is zero-init

        if (!data->appendBytes(&unitWire, sizeof(unitWire))) {
            data->release();
            return kIOReturnNoMemory;
        }

        // Write subunits for this unit
        for (const auto& subunitPtr : subunits) {
            if (!subunitPtr) continue;

            AVCSubunitInfoWire subunitWire{};
            subunitWire.type = static_cast<uint8_t>(subunitPtr->GetType());
            subunitWire.subunitID = subunitPtr->GetID();
            subunitWire.numDestPlugs = subunitPtr->GetNumDestPlugs();
            subunitWire.numSrcPlugs = subunitPtr->GetNumSrcPlugs();

            if (!data->appendBytes(&subunitWire, sizeof(subunitWire))) {
                data->release();
                return kIOReturnNoMemory;
            }
        }
    }

    // Return data through structureOutput
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    ASFW_LOG(UserClient, "GetAVCUnits: returning %zu units in %zu bytes",
             allUnits.size(), data->getLength());
    return kIOReturnSuccess;
}

kern_return_t AVCHandler::GetSubunitCapabilities(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }

    const auto request = ParseSubunitLookupRequest(args, "GetSubunitCapabilities");
    if (!request) {
        return kIOReturnBadArgument;
    }

    const auto subunit = FindRequestedSubunit(*discovery_, *request);
    if (!subunit) {
        return kIOReturnNotFound;
    }
    if (!IsMusicSubunitType(subunit->GetType())) {
        ASFW_LOG(UserClient,
                 "GetSubunitCapabilities: not implemented for subunit type 0x%02x",
                 static_cast<uint8_t>(subunit->GetType()));
        return kIOReturnUnsupported;
    }

    const auto musicSubunit = std::static_pointer_cast<MusicSubunit>(subunit);
    return SerializeMusicCapabilities(musicSubunit->GetCapabilities(),
                                      musicSubunit->GetPlugs(),
                                      musicSubunit->GetMusicChannels(),
                                      args);
}

kern_return_t AVCHandler::SerializeMusicCapabilities(
    const ASFW::Protocols::AVC::Music::MusicSubunitCapabilities& caps,
    const std::vector<ASFW::Protocols::AVC::Music::MusicSubunit::PlugInfo>& plugs,
    const std::vector<ASFW::Protocols::AVC::Music::MusicSubunit::MusicPlugChannel>& channels,
    IOUserClientMethodArguments* args) 
{
    const auto channelNameLookup = BuildChannelNameLookup(channels);
    const auto plan = BuildMusicSerializationPlan(plugs);

    OSData* data = OSData::withCapacity(static_cast<uint32_t>(plan.totalSize));
    if (!data) return kIOReturnNoMemory;

    if (!AppendMusicCapabilitiesHeader(data, caps, plan)) {
        data->release();
        return kIOReturnNoMemory;
    }

    for (size_t i = 0; i < plan.numPlugsToSerialize; ++i) {
        if (!AppendMusicPlug(data, plugs[i], plan.plugInfos[i], channelNameLookup)) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t AVCHandler::GetSubunitDescriptor(IOUserClientMethodArguments* args) {
    if (!discovery_) {
        return kIOReturnNotReady;
    }

    const auto request = ParseSubunitLookupRequest(args, "GetSubunitDescriptor");
    if (!request) {
        return kIOReturnBadArgument;
    }

    const auto subunit = FindRequestedSubunit(*discovery_, *request);
    if (!subunit) {
        ASFW_LOG(UserClient,
                 "GetSubunitDescriptor: subunit not found (GUID=0x%llx type=0x%02x id=%d)",
                 request->guid,
                 request->type,
                 request->id);
        return kIOReturnNotFound;
    }
    if (!IsMusicSubunitType(subunit->GetType())) {
        ASFW_LOG(UserClient,
                 "GetSubunitDescriptor: not implemented for subunit type 0x%02x",
                 static_cast<uint8_t>(subunit->GetType()));
        return kIOReturnUnsupported;
    }

    const auto musicSubunit = std::static_pointer_cast<MusicSubunit>(subunit);
    const auto& descriptorData = musicSubunit->GetStatusDescriptorData();
    if (!descriptorData) {
        ASFW_LOG(UserClient, "GetSubunitDescriptor: descriptor data not available");
        return kIOReturnNotFound;
    }

    const auto& dataVec = descriptorData.value();
    if (dataVec.size() > kMaxWireSize) {
        ASFW_LOG_ERROR(UserClient,
                       "GetSubunitDescriptor: descriptor size %zu exceeds wire limit %zu",
                       dataVec.size(),
                       kMaxWireSize);
        return kIOReturnMessageTooLarge;
    }

    OSData* osData = OSData::withBytes(dataVec.data(), static_cast<uint32_t>(dataVec.size()));
    if (!osData) {
        return kIOReturnNoMemory;
    }

    args->structureOutput = osData;
    args->structureOutputDescriptor = nullptr;
    ASFW_LOG(UserClient, "GetSubunitDescriptor: returning %zu bytes", dataVec.size());
    return kIOReturnSuccess;
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

    auto* targetUnit = FindAVCUnitByGuid(*discovery_, request->guid);
    if (!targetUnit) {
        ASFW_LOG(UserClient,
                 "SendRawFCPCommand: target unit not found (guid=0x%llx)",
                 request->guid);
        return kIOReturnNotFound;
    }

    Protocols::AVC::FCPFrame command{};
    command.length = request->commandLength;
    std::memcpy(command.data.data(), request->commandData->getBytesNoCopy(), command.length);

    auto& store = GetRawFCPResultStore();
    if (!store.lock) {
        ASFW_LOG(UserClient, "SendRawFCPCommand: result store lock unavailable");
        return kIOReturnNoMemory;
    }

    const uint64_t requestID = ReserveRawFCPRequestSlot(store);

    const auto handle = targetUnit->GetFCPTransport().SubmitCommand(
        command,
        [requestID](Protocols::AVC::FCPStatus status, const Protocols::AVC::FCPFrame& response) {
            StoreRawFCPCompletion(requestID, status, response);
        }
    );

    if (!handle.IsValid()) {
        MarkRawFCPRequestFailed(store, requestID);
    }

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

kern_return_t AVCHandler::ReScanAVCUnits(IOUserClientMethodArguments* args) {
    (void)args; // Unused
    
    if (!discovery_) return kIOReturnNotReady;

    ASFW_LOG(UserClient, "ReScanAVCUnits: triggering re-scan");
    discovery_->ReScanAllUnits();
    
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
