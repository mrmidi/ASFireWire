//
//  AVCHandler.cpp
//  ASFWDriver
//
//  Handler for AV/C Protocol API
//

#include "AVCHandler.hpp"
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
#include <unordered_map>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOUserClient.h>

namespace ASFW::UserClient {

namespace {

using namespace ASFW::Shared;
constexpr size_t kMaxWireSize = 4096;  // DriverKit will drop larger structure outputs

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
            // TODO: Get Vendor/Model ID from ConfigROM or device
            unitWire.vendorID = 0; 
            unitWire.modelID = 0;
        } else {
            unitWire.guid = 0;
            unitWire.nodeID = 0xFFFF;
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
    if (!args) return kIOReturnBadArgument;
    if (!discovery_) return kIOReturnNotReady;

    // Expect 4 scalar inputs: GUID_hi, GUID_lo, Type, ID
    if (args->scalarInputCount < 4) {
        ASFW_LOG(UserClient, "GetSubunitCapabilities: missing inputs");
        return kIOReturnBadArgument;
    }

    uint64_t guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1];
    uint8_t type = static_cast<uint8_t>(args->scalarInput[2]);
    uint8_t id = static_cast<uint8_t>(args->scalarInput[3]);

    auto allUnits = discovery_->GetAllAVCUnits();

    for (auto* unit : allUnits) {
        if (unit && unit->GetDevice() && unit->GetDevice()->GetGUID() == guid) {
            // Check subunits
            const auto& subunits = unit->GetSubunits();
            for (const auto& subunit : subunits) {
                if (subunit && 
                    static_cast<uint8_t>(subunit->GetType()) == type && 
                    subunit->GetID() == id) {
                    
                    // Found the subunit!
                    const auto subunitType = subunit->GetType();

                    if (subunitType == Protocols::AVC::AVCSubunitType::kMusic ||
                        subunitType == Protocols::AVC::AVCSubunitType::kMusic0C) {
                        auto musicSubunit = std::static_pointer_cast<Protocols::AVC::Music::MusicSubunit>(subunit);
                        const auto& caps = musicSubunit->GetCapabilities();
                        const auto& plugs = musicSubunit->GetPlugs();
                        const auto& channels = musicSubunit->GetMusicChannels();
                        
                        // 1. Determine Global Rates (from Plug 0 if available)
                        // In Music Subunits, all plugs usually share the same clock domain.
                        uint8_t globalCurrentRate = 0xFF;
                        uint32_t globalSupportedMask = 0;

                        // 1. Calculate Sizes & Global Rates
                        for (const auto& plug : plugs) {
                             if (plug.currentFormat && plug.currentFormat->sampleRate != ASFW::Protocols::AVC::StreamFormats::SampleRate::kUnknown) {
                                uint8_t rate = static_cast<uint8_t>(plug.currentFormat->sampleRate);
                                if (globalCurrentRate == 0xFF) globalCurrentRate = rate;
                            }
                            for (const auto& fmt : plug.supportedFormats) {
                                if (fmt.sampleRate != ASFW::Protocols::AVC::StreamFormats::SampleRate::kUnknown) {
                                    uint8_t rate = static_cast<uint8_t>(fmt.sampleRate);
                                    if (rate < 32) globalSupportedMask |= (1 << rate);
                                }
                            }
                        }

                        // Call static helper
                        return SerializeMusicCapabilities(caps, plugs, channels, args);
                    } else {
                        ASFW_LOG(UserClient, "GetSubunitCapabilities: not implemented for subunit type 0x%02x", static_cast<uint8_t>(subunitType));
                        return kIOReturnUnsupported;
                    }
                }
            }
        }
    }
    
    return kIOReturnNotFound;
}

kern_return_t AVCHandler::SerializeMusicCapabilities(
    const ASFW::Protocols::AVC::Music::MusicSubunitCapabilities& caps,
    const std::vector<ASFW::Protocols::AVC::Music::MusicSubunit::PlugInfo>& plugs,
    const std::vector<ASFW::Protocols::AVC::Music::MusicSubunit::MusicPlugChannel>& channels,
    IOUserClientMethodArguments* args) 
{
    using namespace ASFW::Shared;
    using namespace ASFW::Protocols::AVC::StreamFormats;
    
    // Build a lookup map from musicPlugID -> channel name
    std::unordered_map<uint16_t, std::string> channelNameLookup;
    for (const auto& ch : channels) {
        channelNameLookup[ch.musicPlugID] = ch.name;
    }
    
    uint8_t globalCurrentRate = 0xFF;
    uint32_t globalSupportedMask = 0;
    
    // 1. Calculate total size and gather global rate info
    size_t totalSize = sizeof(AVCMusicCapabilitiesWire);
    size_t numPlugsToSerialize = 0;
    
    // Pre-calculate sizes for each plug
    struct PlugSerializeInfo {
        size_t plugSize;
        uint8_t numBlocks;
        std::vector<uint8_t> channelCounts; // channels per block
        uint8_t numSupportedFormats;        // supported format count
    };
    std::vector<PlugSerializeInfo> plugInfos;
    plugInfos.reserve(plugs.size());
    
    for (const auto& plug : plugs) {
        // Aggregate Global Rate Info
        if (plug.currentFormat && plug.currentFormat->sampleRate != SampleRate::kUnknown) {
            uint8_t rate = static_cast<uint8_t>(plug.currentFormat->sampleRate);
            if (globalCurrentRate == 0xFF) globalCurrentRate = rate;
        }
        
        for (const auto& fmt : plug.supportedFormats) {
            if (fmt.sampleRate != SampleRate::kUnknown) {
                uint8_t rate = static_cast<uint8_t>(fmt.sampleRate);
                if (rate < 32) globalSupportedMask |= (1 << rate);
            }
        }
    
        // Calculate Wire Size for this plug
        PlugSerializeInfo info{};
        info.plugSize = sizeof(PlugInfoWire);
        info.numBlocks = 0;
        
        if (plug.currentFormat) {
            if (plug.currentFormat->IsCompound()) {
                info.numBlocks = static_cast<uint8_t>(
                    std::min(plug.currentFormat->channelFormats.size(), size_t(255)));
                
                for (size_t b = 0; b < info.numBlocks; ++b) {
                    const auto& blk = plug.currentFormat->channelFormats[b];
                    // Each signal block has its header plus channel details
                    uint8_t numChannelDetails = static_cast<uint8_t>(
                        std::min(blk.channels.size(), size_t(255)));
                    info.channelCounts.push_back(numChannelDetails);
                    info.plugSize += sizeof(SignalBlockWire) + 
                                     numChannelDetails * sizeof(ChannelDetailWire);
                }
            } else if (plug.currentFormat->totalChannels > 0) {
                // Simple format - 1 signal block, no channel details
                info.numBlocks = 1;
                info.channelCounts.push_back(0); // No channel details for simple
                info.plugSize += sizeof(SignalBlockWire);
            }
        }
        
        // Add supported formats size
        info.numSupportedFormats = static_cast<uint8_t>(
            std::min(plug.supportedFormats.size(), size_t(32))); // max 32
        info.plugSize += info.numSupportedFormats * sizeof(SupportedFormatWire);
        
        if (totalSize + info.plugSize > kMaxWireSize) break;
        
        totalSize += info.plugSize;
        plugInfos.push_back(info);
        numPlugsToSerialize++;
    }

    // 2. Serialize
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) return kIOReturnNoMemory;
    
    // Header
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
    wire.currentRate = globalCurrentRate;
    wire.supportedRatesMask = globalSupportedMask;
    wire.numPlugs = static_cast<uint8_t>(numPlugsToSerialize);
    wire._reserved = 0;
    
    data->appendBytes(&wire, sizeof(wire));
    
    // Serialize Plugs with nested Signal Blocks and Channel Details
    for (size_t i = 0; i < numPlugsToSerialize; ++i) {
        const auto& plug = plugs[i];
        const auto& info = plugInfos[i];
        
        // Plug header
        PlugInfoWire plugWire{};
        plugWire.plugID = plug.plugID;
        plugWire.isInput = plug.IsInput() ? 1 : 0;
        plugWire.type = static_cast<uint8_t>(plug.type);
        plugWire.numSignalBlocks = info.numBlocks;
        plugWire.numSupportedFormats = info.numSupportedFormats;
        
        size_t copyLen = std::min(plug.name.length(), sizeof(plugWire.name) - 1);
        std::memcpy(plugWire.name, plug.name.c_str(), copyLen);
        plugWire.name[copyLen] = '\0';
        plugWire.nameLength = static_cast<uint8_t>(copyLen);
        
        data->appendBytes(&plugWire, sizeof(plugWire));
        
        // Signal Blocks with nested Channel Details
        if (info.numBlocks > 0 && plug.currentFormat) {
            if (plug.currentFormat->IsCompound()) {
                for (size_t b = 0; b < info.numBlocks; ++b) {
                    if (b >= plug.currentFormat->channelFormats.size()) break;
                    const auto& blk = plug.currentFormat->channelFormats[b];
                    
                    uint8_t numChannelDetails = 0;
                    if (b < info.channelCounts.size()) {
                        numChannelDetails = info.channelCounts[b];
                    }
                    
                    SignalBlockWire blkWire{};
                    blkWire.formatCode = static_cast<uint8_t>(blk.formatCode);
                    blkWire.channelCount = blk.channelCount;
                    blkWire.numChannelDetails = numChannelDetails;
                    blkWire._padding = 0;
                    
                    data->appendBytes(&blkWire, sizeof(blkWire));
                    
                    // Channel Details
                    for (size_t c = 0; c < blkWire.numChannelDetails; ++c) {
                        if (c >= blk.channels.size()) break;
                        const auto& ch = blk.channels[c];
                        
                        ChannelDetailWire chWire{};
                        chWire.musicPlugID = ch.musicPlugID;
                        chWire.position = ch.position;
                        
                        // Get channel name - prefer from ChannelDetail, fallback to lookup
                        std::string chName = ch.name;
                        if (chName.empty()) {
                            auto it = channelNameLookup.find(ch.musicPlugID);
                            if (it != channelNameLookup.end()) {
                                chName = it->second;
                            }
                        }
                        
                        size_t nameCopyLen = std::min(chName.length(), sizeof(chWire.name) - 1);
                        std::memcpy(chWire.name, chName.c_str(), nameCopyLen);
                        chWire.name[nameCopyLen] = '\0';
                        chWire.nameLength = static_cast<uint8_t>(nameCopyLen);
                        
                        data->appendBytes(&chWire, sizeof(chWire));
                    }
                }
            } else {
                // Simple format - 1 signal block, no channel details
                SignalBlockWire blkWire{};
                blkWire.formatCode = 0x06; // MBLA for generic audio
                blkWire.channelCount = plug.currentFormat->totalChannels;
                blkWire.numChannelDetails = 0;
                blkWire._padding = 0;
                
                data->appendBytes(&blkWire, sizeof(blkWire));
            }
        }
        
        // Supported Formats (from 0xBF STREAM FORMAT queries)
        for (size_t s = 0; s < info.numSupportedFormats; ++s) {
            if (s >= plug.supportedFormats.size()) break;
            const auto& fmt = plug.supportedFormats[s];
            
            SupportedFormatWire fmtWire{};
            fmtWire.sampleRateCode = static_cast<uint8_t>(fmt.sampleRate);
            fmtWire.formatCode = static_cast<uint8_t>(
                fmt.channelFormats.empty() ? StreamFormatCode::kMBLA : fmt.channelFormats[0].formatCode);
            fmtWire.channelCount = fmt.totalChannels;
            fmtWire._padding = 0;
            
            data->appendBytes(&fmtWire, sizeof(fmtWire));
        }
    }
    
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t AVCHandler::GetSubunitDescriptor(IOUserClientMethodArguments* args) {
    if (!args) return kIOReturnBadArgument;
    if (!discovery_) return kIOReturnNotReady;

    // Expect 4 scalar inputs: GUID_hi, GUID_lo, Type, ID
    if (args->scalarInputCount < 4) {
        ASFW_LOG(UserClient, "GetSubunitDescriptor: missing inputs");
        return kIOReturnBadArgument;
    }

    uint64_t guid = (static_cast<uint64_t>(args->scalarInput[0]) << 32) | args->scalarInput[1];
    uint8_t type = static_cast<uint8_t>(args->scalarInput[2]);
    uint8_t id = static_cast<uint8_t>(args->scalarInput[3]);

    auto allUnits = discovery_->GetAllAVCUnits();

    for (auto* unit : allUnits) {
        if (unit && unit->GetDevice() && unit->GetDevice()->GetGUID() == guid) {
            const auto& subunits = unit->GetSubunits();
            for (const auto& subunit : subunits) {
                if (subunit && 
                    static_cast<uint8_t>(subunit->GetType()) == type && 
                    subunit->GetID() == id) {
                    
                    const auto subunitType = subunit->GetType();

                    if (subunitType == Protocols::AVC::AVCSubunitType::kMusic ||
                        subunitType == Protocols::AVC::AVCSubunitType::kMusic0C) {
                        auto musicSubunit = std::static_pointer_cast<Protocols::AVC::Music::MusicSubunit>(subunit);
                        
                        const auto& descriptorData = musicSubunit->GetStatusDescriptorData();
                        if (!descriptorData) {
                            ASFW_LOG(UserClient, "GetSubunitDescriptor: descriptor data not available");
                            return kIOReturnNotFound; // Or some other error indicating "no data"
                        }
                        
                        const auto& dataVec = descriptorData.value();
                        if (dataVec.size() > kMaxWireSize) {
                            ASFW_LOG_ERROR(UserClient, "GetSubunitDescriptor: descriptor size %zu exceeds wire limit %zu", dataVec.size(), kMaxWireSize);
                            return kIOReturnMessageTooLarge;
                        }

                        OSData* osData = OSData::withBytes(dataVec.data(), static_cast<uint32_t>(dataVec.size()));
                        if (!osData) return kIOReturnNoMemory;
                        
                        args->structureOutput = osData;
                        args->structureOutputDescriptor = nullptr;
                        
                        ASFW_LOG(UserClient, "GetSubunitDescriptor: returning %zu bytes", dataVec.size());
                        return kIOReturnSuccess;
                    } else {
                        // TODO: Support other subunits if they have descriptors
                        ASFW_LOG(UserClient, "GetSubunitDescriptor: not implemented for subunit type 0x%02x", static_cast<uint8_t>(subunitType));
                        return kIOReturnUnsupported;
                    }
                }
            }
        }
    }

    ASFW_LOG(UserClient, "GetSubunitDescriptor: subunit not found (GUID=0x%llx type=0x%02x id=%d)", guid, type, id);
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
