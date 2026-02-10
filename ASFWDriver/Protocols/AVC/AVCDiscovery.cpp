//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Audio/Model/ASFWAudioDevice.hpp"
#include "../Audio/DeviceStreamModeQuirks.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
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
#include <algorithm>

using namespace ASFW::Protocols::AVC;

namespace {

ASFW::Audio::Model::StreamMode ResolveStreamMode(
    const ASFW::Protocols::AVC::Music::MusicSubunitCapabilities& caps,
    uint32_t vendorId,
    uint32_t modelId,
    const char*& reason) noexcept {
    if (auto forced = ASFW::Audio::Quirks::LookupForcedStreamMode(vendorId, modelId); forced.has_value()) {
        reason = "quirk";
        ASFW_LOG_WARNING(Audio,
                         "AVCDiscovery: QUIRK OVERRIDE stream mode vendor=0x%06x model=0x%06x forced=%{public}s",
                         vendorId, modelId,
                         ASFW::Audio::Quirks::StreamModeToString(*forced));
        return *forced;
    }

    // Use transmit capability as mode selection signal. This mode is currently
    // used by the host IT stream and is expected to match RX in practical devices.
    const bool supportsBlocking = caps.SupportsBlockingTransmit();
    const bool supportsNonBlocking = caps.SupportsNonBlockingTransmit();

    if (supportsBlocking && !supportsNonBlocking) {
        reason = "avc-blocking-only";
        return ASFW::Audio::Model::StreamMode::kBlocking;
    }

    if (supportsNonBlocking) {
        reason = supportsBlocking ? "avc-both-prefer-nonblocking" : "avc-nonblocking-only";
        return ASFW::Audio::Model::StreamMode::kNonBlocking;
    }

    reason = "default-nonblocking";
    return ASFW::Audio::Model::StreamMode::kNonBlocking;
}

struct PlugChannelSummary {
    uint32_t inputAudioMaxChannels{0};   // Subunit input audio stream width
    uint32_t outputAudioMaxChannels{0};  // Subunit output audio stream width
    uint32_t inputAudioPlugs{0};
    uint32_t outputAudioPlugs{0};
};

[[nodiscard]] uint32_t ExtractPlugChannelCount(
    const ASFW::Protocols::AVC::StreamFormats::PlugInfo& plug) noexcept {
    if (!plug.currentFormat.has_value()) {
        return 0;
    }

    const auto& fmt = *plug.currentFormat;
    if (fmt.totalChannels > 0) {
        return fmt.totalChannels;
    }

    uint32_t sum = 0;
    for (const auto& block : fmt.channelFormats) {
        sum += block.channelCount;
    }
    return sum;
}

[[nodiscard]] PlugChannelSummary SummarizePlugChannels(
    const std::vector<ASFW::Protocols::AVC::StreamFormats::PlugInfo>& plugs) noexcept {
    PlugChannelSummary summary{};
    for (const auto& plug : plugs) {
        if (plug.type != ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kAudio) {
            continue;
        }

        const uint32_t channels = ExtractPlugChannelCount(plug);
        if (channels == 0) {
            continue;
        }

        if (plug.IsInput()) {
            ++summary.inputAudioPlugs;
            summary.inputAudioMaxChannels = std::max(summary.inputAudioMaxChannels, channels);
        } else if (plug.IsOutput()) {
            ++summary.outputAudioPlugs;
            summary.outputAudioMaxChannels = std::max(summary.outputAudioMaxChannels, channels);
        }
    }
    return summary;
}

} // namespace

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

    IODispatchQueue* queue = nullptr;
    auto kr = IODispatchQueue::Create("com.asfw.avc.rescan", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        rescanQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else if (kr != kIOReturnSuccess) {
        os_log_error(log_, "AVCDiscovery: Failed to create rescan queue (0x%x)", kr);
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
    avcUnit->Initialize([this, avcUnit, guid](bool success) {
        if (!success) {
            os_log_error(log_,
                         "AVCDiscovery: AVCUnit initialization failed: GUID=%llx",
                         guid);
            return;
        }

        HandleInitializedUnit(guid, avcUnit);
    });

    // Store AVCUnit
    IOLockLock(lock_);
    units_[guid] = std::move(avcUnit);
    IOLockUnlock(lock_);

    // Rebuild node ID map (unit now has transport)
    RebuildNodeIDMap();
}

void AVCDiscovery::HandleInitializedUnit(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (!avcUnit) {
        return;
    }

    auto device = avcUnit->GetDevice();
    if (!device) {
        os_log_error(log_, "AVCDiscovery: AVCUnit missing parent device: GUID=%llx", guid);
        return;
    }

    os_log_info(log_,
                "AVCDiscovery: AVCUnit initialized: GUID=%llx, "
                "%zu subunits, %d inputs, %d outputs",
                guid,
                avcUnit->GetSubunits().size(),
                avcUnit->IsInitialized() ? 2 : 0,  // Placeholder
                avcUnit->IsInitialized() ? 2 : 0); // Placeholder

    // Look for Music Subunit with audio capability
    Music::MusicSubunit* musicSubunit = nullptr;
    for (auto& subunit : avcUnit->GetSubunits()) {
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

    if (!musicSubunit->HasCompleteDescriptorParse()) {
        ASFW_LOG(Audio,
                 "AVCDiscovery: MusicSubunit descriptor incomplete - scheduling re-scan (GUID=%llx)",
                 guid);
        // TODO: Remove this duct-tape once AV/C discovery is reliable.
        ScheduleRescan(guid, avcUnit);
        return;
    }

    IOLockLock(lock_);
    rescanAttempts_.erase(guid);
    const bool hasNub = (audioNubs_.find(guid) != audioNubs_.end());
    IOLockUnlock(lock_);

    if (hasNub) {
        ASFW_LOG(Audio, "AVCDiscovery: Audio nub already exists for GUID=%llx", guid);
        return;
    }

    ASFW_LOG(Audio, "AVCDiscovery: Creating ASFWAudioNub for GUID=%llx", guid);

    //======================================================================
    // Populate MusicSubunitCapabilities with discovery data
    //======================================================================

    auto* devicePtr = device.get();
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

    // Use GetAudioDeviceConfiguration() for device creation.
    auto audioConfig = mutableCaps.GetAudioDeviceConfiguration();
    std::string deviceName = audioConfig.GetDeviceName();

    // Derive transport channel width from current plug formats.
    // For the host TX path, subunit input plugs are the authoritative view.
    const auto plugSummary = SummarizePlugChannels(musicSubunit->GetPlugs());
    const uint32_t plugsDerivedMax = std::max(plugSummary.inputAudioMaxChannels,
                                              plugSummary.outputAudioMaxChannels);
    uint32_t channelCount = plugsDerivedMax;
    const char* channelCountSource = "audio-plug-max-channels";

    if (channelCount == 0) {
        channelCount = audioConfig.GetMaxChannelCount();
        channelCountSource = "capability-fallback";
    }

    if (plugSummary.inputAudioMaxChannels > 0) {
        mutableCaps.maxAudioInputChannels = static_cast<uint16_t>(
            std::min<uint32_t>(plugSummary.inputAudioMaxChannels, 0xFFFFu));
    }
    if (plugSummary.outputAudioMaxChannels > 0) {
        mutableCaps.maxAudioOutputChannels = static_cast<uint16_t>(
            std::min<uint32_t>(plugSummary.outputAudioMaxChannels, 0xFFFFu));
    }

    ASFW_LOG(Audio,
             "AVCDiscovery: audio plug summary in=max%u/%u plugs out=max%u/%u plugs -> selected=%u (%{public}s)",
             plugSummary.inputAudioMaxChannels, plugSummary.inputAudioPlugs,
             plugSummary.outputAudioMaxChannels, plugSummary.outputAudioPlugs,
             channelCount, channelCountSource);

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
            avcUnit->GetFCPTransport(),
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

    const uint32_t vendorId = devicePtr->GetVendorID();
    const uint32_t modelId = devicePtr->GetModelID();
    const char* streamModeReason = "default-nonblocking";
    const auto streamMode = ResolveStreamMode(mutableCaps, vendorId, modelId, streamModeReason);

    ASFW_LOG(Audio,
             "AVCDiscovery: stream mode selected vendor=0x%06x model=0x%06x mode=%{public}s reason=%{public}s",
             vendorId, modelId,
             ASFW::Audio::Quirks::StreamModeToString(streamMode),
             streamModeReason);
    ASFW_LOG(Audio,
             "AVCDiscovery: Creating ASFWAudioNub for GUID=%llx: %{public}s, %u channels, %zu sample rates",
             guid, deviceName.c_str(), channelCount, sampleRates.size());

    ASFW::Audio::Model::ASFWAudioDevice audioDeviceConfig;
    audioDeviceConfig.guid = guid;
    audioDeviceConfig.deviceName = deviceName;
    audioDeviceConfig.channelCount = channelCount;
    audioDeviceConfig.sampleRates = sampleRates;
    audioDeviceConfig.currentSampleRate = currentRate;
    audioDeviceConfig.inputPlugName = mutableCaps.inputPlugName;
    audioDeviceConfig.outputPlugName = mutableCaps.outputPlugName;
    audioDeviceConfig.streamMode = streamMode;
    if (!CreateAudioNubFromModel(guid, audioDeviceConfig, "AVC")) {
        ASFW_LOG(Audio, "AVCDiscovery: CreateAudioNubFromModel failed for GUID=%llx", guid);
    }
}

bool AVCDiscovery::CreateAudioNubFromModel(uint64_t guid,
                                           const Audio::Model::ASFWAudioDevice& config,
                                           const char* sourceTag) {
    if (!driver_ || !lock_) {
        return false;
    }

    // Reserve the GUID slot under lock so AV/C and hardcoded paths cannot race-create duplicates.
    IOLockLock(lock_);
    const bool inserted = audioNubs_.emplace(guid, nullptr).second;
    IOLockUnlock(lock_);
    if (!inserted) {
        ASFW_LOG(Audio,
                 "AVCDiscovery[%{public}s]: Audio nub already exists for GUID=%llx",
                 sourceTag ? sourceTag : "unknown",
                 guid);
        return true;
    }

    IOService* nub = nullptr;
    kern_return_t error = driver_->Create(
        driver_,                      // provider
        "ASFWAudioNubProperties",     // propertiesKey from Info.plist
        &nub                          // result
    );

    if (error != kIOReturnSuccess || !nub) {
        os_log_error(log_,
                     "AVCDiscovery[%{public}s]: Failed to create ASFWAudioNub (GUID=%llx error=%d)",
                     sourceTag ? sourceTag : "unknown",
                     guid,
                     error);
        IOLockLock(lock_);
        audioNubs_.erase(guid);
        IOLockUnlock(lock_);
        return false;
    }

    // Set properties on the nub BEFORE it starts.
    OSDictionary* propertiesRaw = nullptr;
    error = nub->CopyProperties(&propertiesRaw);
    OSSharedPtr<OSDictionary> properties = OSSharedPtr(propertiesRaw, OSNoRetain);
    if (error == kIOReturnSuccess && properties) {
        if (!config.PopulateNubProperties(properties.get())) {
            ASFW_LOG(Audio,
                     "AVCDiscovery[%{public}s]: Failed to populate ASFWAudioDevice properties for GUID=%llx",
                     sourceTag ? sourceTag : "unknown",
                     guid);
        } else {
            nub->SetProperties(properties.get());
            ASFW_LOG(Audio,
                     "AVCDiscovery[%{public}s]: ASFWAudioDevice properties set (GUID=%llx rate=%u Hz ch=%u)",
                     sourceTag ? sourceTag : "unknown",
                     guid,
                     config.currentSampleRate,
                     config.channelCount);
        }
    }

    ASFWAudioNub* audioNub = OSDynamicCast(ASFWAudioNub, nub);
    if (!audioNub) {
        ASFW_LOG(Audio,
                 "AVCDiscovery[%{public}s]: Created service is not ASFWAudioNub for GUID=%llx",
                 sourceTag ? sourceTag : "unknown",
                 guid);
        IOLockLock(lock_);
        audioNubs_.erase(guid);
        IOLockUnlock(lock_);
        nub->release();
        return false;
    }
    audioNub->SetChannelCount(config.channelCount);
    audioNub->SetStreamMode(static_cast<uint32_t>(config.streamMode));

    IOLockLock(lock_);
    audioNubs_[guid] = audioNub;
    IOLockUnlock(lock_);

    // Release our creation reference - IOKit retains it.
    nub->release();

    ASFW_LOG(Audio,
             "✅ AVCDiscovery[%{public}s]: ASFWAudioNub ready for GUID=%llx",
             sourceTag ? sourceTag : "unknown",
             guid);
    return true;
}

void AVCDiscovery::ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (!avcUnit) {
        return;
    }

    constexpr uint8_t kMaxAutoRescanAttempts = 1;
    constexpr uint32_t kRescanDelayMs = 250;

    uint8_t attempt = 0;
    IOLockLock(lock_);
    auto& count = rescanAttempts_[guid];
    if (count >= kMaxAutoRescanAttempts) {
        IOLockUnlock(lock_);
        ASFW_LOG(Audio,
                 "AVCDiscovery: Auto re-scan limit reached for GUID=%llx (attempts=%u)",
                 guid, count);
        return;
    }
    count++;
    attempt = count;
    IOLockUnlock(lock_);

    auto unit = avcUnit;
    auto rescanWork = [this, guid, attempt, unit]() {
        if (kRescanDelayMs > 0) {
            IOSleep(kRescanDelayMs);
        }

        ASFW_LOG(Audio, "AVCDiscovery: Auto re-scan attempt %u for GUID=%llx", attempt, guid);
        unit->ReScan([this, guid, unit](bool success) {
            if (!success) {
                os_log_error(log_,
                             "AVCDiscovery: AVCUnit re-scan failed: GUID=%llx",
                             guid);
                return;
            }

            HandleInitializedUnit(guid, unit);
        });
    };

    if (rescanQueue_) {
        rescanQueue_->DispatchAsync(^{ rescanWork(); });
    } else {
        rescanWork();
    }
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
    rescanAttempts_.erase(guid);
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

void AVCDiscovery::EnsureHardcodedAudioNubForDevice(const Discovery::DeviceRecord& deviceRecord) {
    if (!driver_ || deviceRecord.guid == 0) {
        return;
    }

    ASFW::Audio::Model::ASFWAudioDevice hardcoded;
    hardcoded.guid = deviceRecord.guid;
    if (!deviceRecord.vendorName.empty() || !deviceRecord.modelName.empty()) {
        hardcoded.deviceName = deviceRecord.vendorName + " " + deviceRecord.modelName;
    } else {
        hardcoded.deviceName = "Focusrite Saffire Pro 24 DSP";
    }

    // Hardcoded bring-up profile (v1):
    // - advertise single 48kHz / 24-bit stream format
    // - use 16 channels end-to-end until asymmetric in/out is modeled in ADK path
    hardcoded.channelCount = 16;
    hardcoded.sampleRates = {48000};
    hardcoded.currentSampleRate = 48000;
    hardcoded.inputPlugName = "Saffire Input";
    hardcoded.outputPlugName = "Saffire Output";
    hardcoded.streamMode = Audio::Model::StreamMode::kNonBlocking;

    ASFW_LOG(Audio,
             "AVCDiscovery[Hardcoded]: ensuring audio nub for GUID=%llx (%{public}s)",
             deviceRecord.guid,
             hardcoded.deviceName.c_str());

    if (!CreateAudioNubFromModel(deviceRecord.guid, hardcoded, "Hardcoded")) {
        ASFW_LOG(Audio,
                 "AVCDiscovery[Hardcoded]: failed to create audio nub for GUID=%llx",
                 deviceRecord.guid);
    }
}

void AVCDiscovery::ReScanAllUnits() {
    IOLockLock(lock_);
    
    os_log_info(log_, "AVCDiscovery: Re-scanning all %zu units", units_.size());
    rescanAttempts_.clear();

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
