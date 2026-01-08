//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Logging/Logging.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include "Music/MusicSubunit.hpp"
#include "StreamFormats/AVCSignalFormatCommand.hpp"
#include <DriverKit/IOService.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSDictionary.h>
#include <set>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constants
//==============================================================================

/// 1394 Trade Association spec ID (24-bit)
constexpr uint32_t kAVCSpecID = 0x00A02D;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(IOService* driver,
                           Discovery::IDeviceManager& deviceManager,
                           Async::AsyncSubsystem& asyncSubsystem)
    : driver_(driver), deviceManager_(deviceManager), asyncSubsystem_(asyncSubsystem) {

    // Allocate lock
    lock_ = IOLockAlloc();
    if (!lock_) {
        os_log_error(log_, "AVCDiscovery: Failed to allocate lock");
    }

    // Register as unit observer
    deviceManager_.RegisterUnitObserver(this);

    os_log_info(log_, "AVCDiscovery: Initialized");
}

AVCDiscovery::~AVCDiscovery() {
    // Unregister observer
    deviceManager_.UnregisterUnitObserver(this);

    // Clean up lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    os_log_info(log_, "AVCDiscovery: Destroyed");
}

//==============================================================================
// IUnitObserver Interface
//==============================================================================

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!IsAVCUnit(unit)) {
        return;
    }

    uint64_t guid = GetUnitGUID(unit);

    ASFW_LOG(Async,
             "✅ AV/C DETECTED: GUID=%llx, specID=0x%06x - SCANNING...",
             guid, unit->GetUnitSpecID());

    // Get parent device
    auto device = unit->GetDevice();
    if (!device) {
        os_log_error(log_, "AVCDiscovery: Unit has no parent device");
        return;
    }

    // Create AVCUnit
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, asyncSubsystem_);

    // Initialize (probe subunits, plugs)
    auto* avcUnitPtr = avcUnit.get();
    auto* devicePtr = device.get();
    avcUnit->Initialize([this, avcUnitPtr, devicePtr, guid](bool success) {
        if (!success) {
            os_log_error(log_,
                         "AVCDiscovery: AVCUnit initialization failed: GUID=%llx",
                         guid);
            return;
        }

        os_log_info(log_,
                    "AVCDiscovery: AVCUnit initialized: GUID=%llx, "
                    "%zu subunits, %d inputs, %d outputs",
                    guid,
                    avcUnitPtr->GetSubunits().size(),
                    avcUnitPtr->IsInitialized() ? 2 : 0,  // Placeholder
                    avcUnitPtr->IsInitialized() ? 2 : 0); // Placeholder

        // Look for Music Subunit with audio capability
        Music::MusicSubunit* musicSubunit = nullptr;
        for (auto& subunit : avcUnitPtr->GetSubunits()) {
            ASFW_LOG(Audio, "AVCDiscovery: Checking subunit type=0x%02x (kMusic=0x%02x)",
                    static_cast<uint8_t>(subunit->GetType()),
                    static_cast<uint8_t>(AVCSubunitType::kMusic));

            // Check if this is a Music subunit by type
            // Some devices report 0x0C, others 0x1C (both are valid Music subunit types)
            if (subunit->GetType() == AVCSubunitType::kMusic ||
                subunit->GetType() == AVCSubunitType::kMusic0C) {
                auto* music = static_cast<Music::MusicSubunit*>(subunit.get());
                const auto& caps = music->GetCapabilities();

                ASFW_LOG(Audio, "AVCDiscovery: Found Music subunit - hasAudioCapability=%d",
                        caps.HasAudioCapability());

                if (caps.HasAudioCapability()) {
                    musicSubunit = music;
                    break;
                }
            }
        }

        if (!musicSubunit) {
            os_log_debug(log_,
                        "AVCDiscovery: No audio-capable music subunit found (GUID=%llx)",
                        guid);
            return;
        }

        ASFW_LOG(Audio, "AVCDiscovery: Creating ASFWAudioNub for GUID=%llx", guid);

        //======================================================================
        // Populate MusicSubunitCapabilities with discovery data
        //======================================================================
        
        // Get mutable reference to capabilities for population
        auto& mutableCaps = const_cast<Music::MusicSubunitCapabilities&>(musicSubunit->GetCapabilities());
        
        // Populate device identity from FWDevice (Config ROM)
        mutableCaps.guid = guid;
        mutableCaps.vendorName = std::string(devicePtr->GetVendorName());
        mutableCaps.modelName = std::string(devicePtr->GetModelName());
        
        // Extract sample rates from supported formats (deduplicated, sorted)
        std::set<double> rateSet;
        for (const auto& plug : musicSubunit->GetPlugs()) {
            for (const auto& format : plug.supportedFormats) {
                uint32_t rateHz = format.GetSampleRateHz();
                if (rateHz > 0) {
                    rateSet.insert(static_cast<double>(rateHz));
                }
            }
        }
        mutableCaps.supportedSampleRates.assign(rateSet.begin(), rateSet.end());
        if (mutableCaps.supportedSampleRates.empty()) {
            mutableCaps.supportedSampleRates = {44100.0, 48000.0};  // Fallback
        }
        
        // Extract plug names (first input/output found, or keep defaults)
        // NOTE: Subunit perspective vs Host perspective are opposite!
        // - MusicSubunit "Input" = audio FROM host to device = HOST OUTPUT
        // - MusicSubunit "Output" = audio TO host from device = HOST INPUT
        for (const auto& plug : musicSubunit->GetPlugs()) {
            if (plug.IsInput() && !plug.name.empty() && mutableCaps.outputPlugName == "Output") {
                // Subunit Input (from host) = Host Output
                mutableCaps.outputPlugName = plug.name;
            }
            if (plug.IsOutput() && !plug.name.empty() && mutableCaps.inputPlugName == "Input") {
                // Subunit Output (to host) = Host Input
                mutableCaps.inputPlugName = plug.name;
            }
        }
        
        // Extract current sample rate from first plug's current format
        // The device reports its active sample rate in the currentFormat
        bool foundCurrentRate = false;
        for (const auto& plug : musicSubunit->GetPlugs()) {
            if (plug.currentFormat.has_value()) {
                uint32_t rateHz = plug.currentFormat->GetSampleRateHz();
                if (rateHz > 0) {
                    mutableCaps.currentSampleRate = static_cast<double>(rateHz);
                    ASFW_LOG(Audio, "AVCDiscovery: Current sample rate from plug %u: %u Hz",
                             plug.plugID, rateHz);
                    foundCurrentRate = true;
                    break;  // Use first found
                }
            }
        }
        
        // Fallback: Use first supported sample rate if no current rate found
        if (!foundCurrentRate && !mutableCaps.supportedSampleRates.empty()) {
            mutableCaps.currentSampleRate = mutableCaps.supportedSampleRates[0];
            ASFW_LOG(Audio, "AVCDiscovery: Using first supported rate as current: %.0f Hz",
                     mutableCaps.currentSampleRate);
        }
        
        // Use GetAudioDeviceConfiguration() for device creation
        auto audioConfig = mutableCaps.GetAudioDeviceConfiguration();
        std::string deviceName = audioConfig.GetDeviceName();
        uint32_t channelCount = audioConfig.GetMaxChannelCount();
        
        //======================================================================
        // Phase 1.5: Set sample rate to 48kHz before creating audio nub
        //======================================================================
        // TODO: Make this configurable. For now, force 48kHz for encoding testing.
        constexpr double kTargetSampleRate = 48000.0;
        
        // Check if device supports 48kHz
        bool supports48k = false;
        for (double rate : mutableCaps.supportedSampleRates) {
            if (rate == kTargetSampleRate) {
                supports48k = true;
                break;
            }
        }
        
        if (supports48k && mutableCaps.currentSampleRate != kTargetSampleRate) {
            ASFW_LOG(Audio, "AVCDiscovery: Switching sample rate from %.0f Hz to %.0f Hz (fire-and-forget)",
                     mutableCaps.currentSampleRate, kTargetSampleRate);
            
            // Use Unit Plug Signal Format (Oxford/Linux style) - opcode 0x19
            // This sets the format on Unit Plug 0 which controls both input and output
            using namespace ASFW::Protocols::AVC;
            
            // Build AV/C CDB for INPUT PLUG SIGNAL FORMAT (0x19) CONTROL command
            // Subunit 0xFF = Unit level (not Music Subunit)
            AVCCdb cdb;
            cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
            cdb.subunit = 0xFF;  // Unit level (not Music Subunit 0x60)
            cdb.opcode = 0x19;   // INPUT PLUG SIGNAL FORMAT (Oxford/Linux style)
            cdb.operands[0] = 0x00;  // Plug 0
            cdb.operands[1] = 0x90;  // AM824 format
            cdb.operands[2] = 0x02;  // 48kHz (SFC code per IEC 61883-6)
            cdb.operands[3] = 0xFF;  // Padding/Sync
            cdb.operands[4] = 0xFF;  // Padding/Sync
            cdb.operandLength = 5;
            
            // Create command with shared ownership (required by AVCCommand)
            auto setRateCmd = std::make_shared<AVCCommand>(
                avcUnitPtr->GetFCPTransport(),
                cdb
            );
            
            // Fire-and-forget: Submit and assume success
            // Don't wait - the device will switch asynchronously
            // The callback just logs the result
            setRateCmd->Submit([setRateCmd](AVCResult result, const AVCCdb&) {
                // Capture setRateCmd to keep it alive until completion
                if (IsSuccess(result)) {
                    ASFW_LOG(Audio, "✅ AVCDiscovery: Sample rate change command accepted");
                } else {
                    ASFW_LOG_WARNING(Audio, "AVCDiscovery: Sample rate change command response: %d",
                                     static_cast<int>(result));
                }
            });
            
            // Assume success - set to 48kHz
            // The device typically switches within milliseconds
            mutableCaps.currentSampleRate = kTargetSampleRate;
            ASFW_LOG(Audio, "AVCDiscovery: Assuming 48kHz - nub will use this rate");
            
        } else if (!supports48k) {
            ASFW_LOG(Audio, "AVCDiscovery: Device does not support 48kHz, using %.0f Hz",
                     mutableCaps.currentSampleRate);
        } else {
            ASFW_LOG(Audio, "AVCDiscovery: Device already at 48kHz");
        }
        
        // Build sample rates vector for OSArray (need uint32_t for OSNumber)
        // IMPORTANT: Put the current/target sample rate FIRST so CoreAudio HAL selects it
        std::vector<uint32_t> sampleRates;
        
        // First, add the current sample rate (48kHz if we switched)
        uint32_t currentRate = static_cast<uint32_t>(mutableCaps.currentSampleRate);
        sampleRates.push_back(currentRate);
        
        // Then add other supported rates (excluding the one already added)
        for (double rate : mutableCaps.supportedSampleRates) {
            uint32_t rateHz = static_cast<uint32_t>(rate);
            if (rateHz != currentRate) {
                sampleRates.push_back(rateHz);
            }
        }

        ASFW_LOG(Audio,
                 "AVCDiscovery: Creating ASFWAudioNub for GUID=%llx: %{public}s, %u channels, %zu sample rates",
                 guid, deviceName.c_str(), channelCount, sampleRates.size());

        // Create property objects using OSSharedPtr for automatic memory management
        OSSharedPtr<OSString> deviceNameStr = OSSharedPtr(
            OSString::withCString(deviceName.c_str()), OSNoRetain);
        OSSharedPtr<OSNumber> channelCountNum = OSSharedPtr(
            OSNumber::withNumber(channelCount, 32), OSNoRetain);
        OSSharedPtr<OSNumber> guidNum = OSSharedPtr(
            OSNumber::withNumber(guid, 64), OSNoRetain);
        OSSharedPtr<OSArray> sampleRatesArray = OSSharedPtr(
            OSArray::withCapacity(static_cast<uint32_t>(sampleRates.size())), OSNoRetain);

        if (!sampleRatesArray) {
            os_log_error(log_, "AVCDiscovery: Failed to create sample rates array");
            return;
        }

        for (uint32_t rate : sampleRates) {
            OSSharedPtr<OSNumber> rateNum = OSSharedPtr(
                OSNumber::withNumber(rate, 32), OSNoRetain);
            if (rateNum) {
                sampleRatesArray->setObject(rateNum.get());
            }
        }

        // Create the nub using IOService::Create() with plist properties
        IOService* nub = nullptr;
        kern_return_t error = driver_->Create(
            driver_,                      // provider
            "ASFWAudioNubProperties",     // propertiesKey from Info.plist
            &nub                          // result
        );

        if (error != kIOReturnSuccess || !nub) {
            os_log_error(log_,
                        "AVCDiscovery: Failed to create ASFWAudioNub (error=%d)",
                        error);
            return;
        }

        // Set properties on the nub BEFORE it starts
        // These properties will be read by ASFWAudioDriver when it matches
        // In DriverKit, we need to create a properties dictionary
        OSDictionary* propertiesRaw = nullptr;
        error = nub->CopyProperties(&propertiesRaw);
        OSSharedPtr<OSDictionary> properties = OSSharedPtr(propertiesRaw, OSNoRetain);
        if (error == kIOReturnSuccess && properties) {
            if (deviceNameStr) {
                properties->setObject("ASFWDeviceName", deviceNameStr.get());
            }
            if (channelCountNum) {
                properties->setObject("ASFWChannelCount", channelCountNum.get());
            }
            if (sampleRatesArray) {
                properties->setObject("ASFWSampleRates", sampleRatesArray.get());
            }
            if (guidNum) {
                properties->setObject("ASFWGUID", guidNum.get());
            }
            
            // Add plug names for stream/channel naming
            OSSharedPtr<OSString> inputPlugNameStr = OSSharedPtr(
                OSString::withCString(mutableCaps.inputPlugName.c_str()), OSNoRetain);
            OSSharedPtr<OSString> outputPlugNameStr = OSSharedPtr(
                OSString::withCString(mutableCaps.outputPlugName.c_str()), OSNoRetain);
            
            if (inputPlugNameStr) {
                properties->setObject("ASFWInputPlugName", inputPlugNameStr.get());
            }
            if (outputPlugNameStr) {
                properties->setObject("ASFWOutputPlugName", outputPlugNameStr.get());
            }
            
            // Add current sample rate (from device's active format)
            OSSharedPtr<OSNumber> currentRateNum = OSSharedPtr(
                OSNumber::withNumber(static_cast<uint32_t>(mutableCaps.currentSampleRate), 32), OSNoRetain);
            if (currentRateNum) {
                properties->setObject("ASFWCurrentSampleRate", currentRateNum.get());
                ASFW_LOG(Audio, "AVCDiscovery: Set current sample rate property: %u Hz",
                         static_cast<uint32_t>(mutableCaps.currentSampleRate));
            }
            
            // Update the nub's properties
            nub->SetProperties(properties.get());
        }

        // Store reference for cleanup (nub is retained by IOKit)
        // The nub's Start() method will be called automatically by DriverKit
        // and will call RegisterService() to publish itself for matching
        IOLockLock(lock_);
        audioNubs_[guid] = OSDynamicCast(ASFWAudioNub, nub);
        IOLockUnlock(lock_);

        // Release our creation reference - IOKit retains it
        nub->release();

        ASFW_LOG(Audio,
                 "✅ ASFWAudioNub created for GUID=%llx - waiting for Start/RegisterService",
                 guid);
    });

    // Store AVCUnit
    IOLockLock(lock_);
    units_[guid] = std::move(avcUnit);
    IOLockUnlock(lock_);

    // Rebuild node ID map (unit now has transport)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit suspended: GUID=%llx",
                    guid);
        // Unit remains in map but operations will fail until resumed
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (suspended units removed from routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) {
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit resumed: GUID=%llx",
                    guid);
        // Unit is now available again
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (resumed units back in routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    uint64_t guid = GetUnitGUID(unit);

    IOLockLock(lock_);

    // Clean up audio nub if exists
    auto nubIt = audioNubs_.find(guid);
    if (nubIt != audioNubs_.end()) {
        ASFWAudioNub* nub = nubIt->second;
        if (nub) {
            ASFW_LOG(Audio, "AVCDiscovery: Terminating ASFWAudioNub for GUID=%llx", guid);

            // Remove from map first
            audioNubs_.erase(nubIt);

            // Unlock before calling IOKit methods to avoid deadlock
            IOLockUnlock(lock_);

            // Terminate the nub service
            // In DriverKit, Terminate() with 0 means standard termination
            nub->Terminate(0);

            // Re-lock for units_ cleanup
            IOLockLock(lock_);
        } else {
            audioNubs_.erase(nubIt);
        }
    }

    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    IOLockUnlock(lock_);

    // Rebuild node ID map (terminated unit removed)
    RebuildNodeIDMap();
}

//==============================================================================
// Public API
//==============================================================================

AVCUnit* AVCDiscovery::GetAVCUnit(uint64_t guid) {
    IOLockLock(lock_);

    auto it = units_.find(guid);
    AVCUnit* result = (it != units_.end()) ? it->second.get() : nullptr;

    IOLockUnlock(lock_);

    return result;
}

AVCUnit* AVCDiscovery::GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) {
    if (!unit) {
        return nullptr;
    }

    uint64_t guid = GetUnitGUID(unit);
    return GetAVCUnit(guid);
}

std::vector<AVCUnit*> AVCDiscovery::GetAllAVCUnits() {
    IOLockLock(lock_);

    std::vector<AVCUnit*> result;
    result.reserve(units_.size());

    for (auto& [guid, avcUnit] : units_) {
        result.push_back(avcUnit.get());
    }

    IOLockUnlock(lock_);

    return result;
}

void AVCDiscovery::ReScanAllUnits() {
    IOLockLock(lock_);
    
    os_log_info(log_, "AVCDiscovery: Re-scanning all %zu units", units_.size());

    for (auto& [guid, avcUnit] : units_) {
        if (avcUnit) {
            // Trigger re-scan (async)
            avcUnit->ReScan([guid](bool success) {
                // Logging handled inside AVCUnit
            });
        }
    }

    IOLockUnlock(lock_);
}

FCPTransport* AVCDiscovery::GetFCPTransportForNodeID(uint16_t nodeID) {
    IOLockLock(lock_);

    // Normalize to node number (low 6 bits) to match map keys
    const uint16_t nodeNumber = static_cast<uint16_t>(nodeID & 0x3Fu);

    auto it = fcpTransportsByNodeID_.find(nodeNumber);
    FCPTransport* result = (it != fcpTransportsByNodeID_.end())
                               ? it->second
                               : nullptr;

    IOLockUnlock(lock_);

    return result;
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    os_log_info(log_,
                "AVCDiscovery: Bus reset (generation %u)",
                newGeneration);

    // Notify all AVCUnits of bus reset
    IOLockLock(lock_);

    for (auto& [guid, avcUnit] : units_) {
        avcUnit->OnBusReset(newGeneration);
    }

    IOLockUnlock(lock_);

    // Rebuild node ID map (node IDs changed)
    RebuildNodeIDMap();
}

//==============================================================================
// Private Helpers
//==============================================================================

bool AVCDiscovery::IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return false;
    }

    // Check unit spec ID (24-bit, should be 0x00A02D for AV/C)
    uint32_t specID = unit->GetUnitSpecID() & 0xFFFFFF;

    return specID == kAVCSpecID;
}

uint64_t AVCDiscovery::GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const {
    if (!unit) {
        return 0;
    }

    auto device = unit->GetDevice();
    if (!device) {
        return 0;
    }

    return device->GetGUID();
}

void AVCDiscovery::RebuildNodeIDMap() {
    IOLockLock(lock_);

    // Clear old mappings
    fcpTransportsByNodeID_.clear();

    // Rebuild from current units
    for (auto& [guid, avcUnit] : units_) {
        auto device = avcUnit->GetDevice();
        if (!device) {
            continue;  // Device destroyed
        }

        auto unit = avcUnit->GetFWUnit();
        if (!unit || !unit->IsReady()) {
            continue;  // Unit suspended or terminated
        }

        // Normalize to node number (low 6 bits) to tolerate full vs short IDs
        const uint16_t fullNodeID = device->GetNodeID();
        const uint16_t nodeNumber = static_cast<uint16_t>(fullNodeID & 0x3Fu);
        
        fcpTransportsByNodeID_[nodeNumber] = &avcUnit->GetFCPTransport();

        os_log_debug(log_,
                     "AVCDiscovery: Mapped fullNodeID=0x%04x (node=%u) → FCPTransport (GUID=%llx)",
                     fullNodeID, nodeNumber, guid);
    }

    IOLockUnlock(lock_);
}

void AVCDiscovery::SetTransmitRingBufferOnNubs(void* ringBuffer) {
    // This method is deprecated - shared TX queue is now in ASFWAudioNub
    // Kept for backwards compatibility logging
    IOLockLock(lock_);

    os_log_info(log_,
                "AVCDiscovery: SetTransmitRingBufferOnNubs called (deprecated - using shared queue now)");

    IOLockUnlock(lock_);
}

ASFWAudioNub* AVCDiscovery::GetFirstAudioNub() {
    IOLockLock(lock_);

    ASFWAudioNub* result = nullptr;
    for (auto& [guid, nub] : audioNubs_) {
        if (nub) {
            result = nub;
            os_log_debug(log_,
                        "AVCDiscovery: GetFirstAudioNub returning nub for GUID=%llx",
                        guid);
            break;
        }
    }

    IOLockUnlock(lock_);
    return result;
}
