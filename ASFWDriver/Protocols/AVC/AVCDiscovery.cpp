//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Audio/Model/ASFWAudioDevice.hpp"
#include "../Audio/DeviceProtocolFactory.hpp"
#include "../Audio/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "../Audio/DeviceStreamModeQuirks.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
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
#include <atomic>
#include <functional>

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
constexpr uint32_t kDuetPrefetchTimeoutMs = 1200;
constexpr uint32_t kClassIdPhantomPower = static_cast<uint32_t>('phan');
constexpr uint32_t kClassIdPhaseInvert = static_cast<uint32_t>('phsi');
constexpr uint32_t kScopeInput = static_cast<uint32_t>('inpt');
constexpr uint32_t kDuetPhantomMask = 0x3u;

void ConfigureDuetPhantomOverrides(
    ASFW::Audio::Model::ASFWAudioDevice& config,
    const std::optional<ASFW::Audio::Oxford::Apogee::InputParams>& inputParams) {
    config.hasPhantomOverride = true;
    config.phantomSupportedMask = kDuetPhantomMask;

    uint32_t initialMask = 0;
    if (inputParams.has_value()) {
        const auto& params = *inputParams;
        for (uint32_t index = 0; index < 2; ++index) {
            if (params.phantomPowerings[index]) {
                initialMask |= (1u << index);
            }
        }
    }
    config.phantomInitialMask = initialMask;

    config.boolControlOverrides.clear();
    config.boolControlOverrides.reserve(4);
    for (uint32_t element = 1; element <= 2; ++element) {
        const uint32_t bit = 1u << (element - 1u);
        bool polarityInitial = false;
        if (inputParams.has_value()) {
            polarityInitial = inputParams->polarities[element - 1u];
        }
        config.boolControlOverrides.push_back({
            .classIdFourCC = kClassIdPhantomPower,
            .scopeFourCC = kScopeInput,
            .element = element,
            .isSettable = true,
            .initialValue = (initialMask & bit) != 0u,
        });
        config.boolControlOverrides.push_back({
            .classIdFourCC = kClassIdPhaseInvert,
            .scopeFourCC = kScopeInput,
            .element = element,
            .isSettable = true,
            .initialValue = polarityInitial,
        });
    }
}

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(IOService* driver,
                           Discovery::IDeviceManager& deviceManager,
                           Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           ASFW::Audio::IAVCAudioConfigListener* audioConfigListener)
    : driver_(driver)
    , deviceManager_(deviceManager)
    , busOps_(busOps)
    , busInfo_(busInfo)
    , audioConfigListener_(audioConfigListener) {

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

    // Register as discovery observers
    deviceManager_.RegisterUnitObserver(this);
    deviceManager_.RegisterDeviceObserver(this);

    os_log_info(log_, "AVCDiscovery: Initialized");
}

AVCDiscovery::~AVCDiscovery() {
    // Unregister discovery observers
    deviceManager_.UnregisterDeviceObserver(this);
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
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, busOps_, busInfo_);

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

    auto* musicSubunit = FindAudioMusicSubunit(*avcUnit);
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

    if (lock_) {
        IOLockLock(lock_);
        rescanAttempts_.erase(guid);
        IOLockUnlock(lock_);
    }

    PopulateMusicSubunitCapabilities(guid, *device, *musicSubunit);
    UpdateCurrentSampleRate(*musicSubunit);
    ApplyTargetSampleRateIfSupported(avcUnit, *musicSubunit);

    auto audioDeviceConfig = BuildAudioDeviceConfig(guid, *device, *musicSubunit);
    if (IsApogeeDuet(*device)) {
        ConfigureDuetPhantomOverrides(audioDeviceConfig, std::nullopt);
        ASFW_LOG(Audio,
                 "AVCDiscovery: Apogee Duet detected (GUID=%llx) - prefetching vendor config before publishing config",
                 guid);
        PrefetchDuetStateAndCreateNub(guid, avcUnit, audioDeviceConfig);
        return;
    }

    PublishReadyAudioConfig(guid, audioDeviceConfig);
}

Music::MusicSubunit* AVCDiscovery::FindAudioMusicSubunit(const AVCUnit& avcUnit) const {
    for (const auto& subunit : avcUnit.GetSubunits()) {
        ASFW_LOG(Audio, "AVCDiscovery: Checking subunit type=0x%02x (kMusic=0x%02x)",
                 static_cast<uint8_t>(subunit->GetType()),
                 static_cast<uint8_t>(AVCSubunitType::kMusic));

        if (subunit->GetType() != AVCSubunitType::kMusic &&
            subunit->GetType() != AVCSubunitType::kMusic0C) {
            continue;
        }

        auto* music = static_cast<Music::MusicSubunit*>(subunit.get());
        const auto& caps = music->GetCapabilities();
        ASFW_LOG(Audio, "AVCDiscovery: Found Music subunit - hasAudioCapability=%d",
                 caps.HasAudioCapability());
        if (caps.HasAudioCapability()) {
            return music;
        }
    }

    return nullptr;
}

void AVCDiscovery::PopulateMusicSubunitCapabilities(uint64_t guid,
                                                    const Discovery::FWDevice& device,
                                                    Music::MusicSubunit& musicSubunit) const {
    auto& mutableCaps = const_cast<Music::MusicSubunitCapabilities&>(musicSubunit.GetCapabilities());
    mutableCaps.guid = guid;
    mutableCaps.vendorName = std::string(device.GetVendorName());
    mutableCaps.modelName = std::string(device.GetModelName());

    std::set<double> rateSet;
    for (const auto& plug : musicSubunit.GetPlugs()) {
        for (const auto& format : plug.supportedFormats) {
            const uint32_t rateHz = format.GetSampleRateHz();
            if (rateHz > 0) {
                rateSet.insert(static_cast<double>(rateHz));
            }
        }
    }

    mutableCaps.supportedSampleRates.assign(rateSet.begin(), rateSet.end());
    if (mutableCaps.supportedSampleRates.empty()) {
        mutableCaps.supportedSampleRates = {44100.0, 48000.0};
    }

    for (const auto& plug : musicSubunit.GetPlugs()) {
        if (plug.IsInput() && !plug.name.empty() && mutableCaps.outputPlugName == "Output") {
            mutableCaps.outputPlugName = plug.name;
        }
        if (plug.IsOutput() && !plug.name.empty() && mutableCaps.inputPlugName == "Input") {
            mutableCaps.inputPlugName = plug.name;
        }
    }
}

void AVCDiscovery::UpdateCurrentSampleRate(Music::MusicSubunit& musicSubunit) const {
    auto& mutableCaps = const_cast<Music::MusicSubunitCapabilities&>(musicSubunit.GetCapabilities());

    for (const auto& plug : musicSubunit.GetPlugs()) {
        if (!plug.currentFormat.has_value()) {
            continue;
        }

        const uint32_t rateHz = plug.currentFormat->GetSampleRateHz();
        if (rateHz == 0) {
            continue;
        }

        mutableCaps.currentSampleRate = static_cast<double>(rateHz);
        ASFW_LOG(Audio, "AVCDiscovery: Current sample rate from plug %u: %u Hz",
                 plug.plugID, rateHz);
        return;
    }

    if (!mutableCaps.supportedSampleRates.empty()) {
        mutableCaps.currentSampleRate = mutableCaps.supportedSampleRates[0];
        ASFW_LOG(Audio, "AVCDiscovery: Using first supported rate as current: %.0f Hz",
                 mutableCaps.currentSampleRate);
    }
}

void AVCDiscovery::ApplyTargetSampleRateIfSupported(const std::shared_ptr<AVCUnit>& avcUnit,
                                                    Music::MusicSubunit& musicSubunit) const {
    constexpr double kTargetSampleRate = 48000.0;
    auto& mutableCaps = const_cast<Music::MusicSubunitCapabilities&>(musicSubunit.GetCapabilities());
    const bool supports48k = std::find(mutableCaps.supportedSampleRates.begin(),
                                       mutableCaps.supportedSampleRates.end(),
                                       kTargetSampleRate) != mutableCaps.supportedSampleRates.end();

    if (!supports48k) {
        ASFW_LOG(Audio, "AVCDiscovery: Device does not support 48kHz, using %.0f Hz",
                 mutableCaps.currentSampleRate);
        return;
    }

    if (mutableCaps.currentSampleRate == kTargetSampleRate) {
        ASFW_LOG(Audio, "AVCDiscovery: Device already at 48kHz");
        return;
    }

    ASFW_LOG(Audio, "AVCDiscovery: Switching sample rate from %.0f Hz to %.0f Hz (fire-and-forget)",
             mutableCaps.currentSampleRate, kTargetSampleRate);

    AVCCdb cdb;
    cdb.ctype = static_cast<uint8_t>(AVCCommandType::kControl);
    cdb.subunit = 0xFF;
    cdb.opcode = 0x19;
    cdb.operands[0] = 0x00;
    cdb.operands[1] = 0x90;
    cdb.operands[2] = 0x02;
    cdb.operands[3] = 0xFF;
    cdb.operands[4] = 0xFF;
    cdb.operandLength = 5;

    auto setRateCmd = std::make_shared<AVCCommand>(avcUnit->GetFCPTransport(), cdb);
    setRateCmd->Submit([setRateCmd](AVCResult result, const AVCCdb&) {
        if (IsSuccess(result)) {
            ASFW_LOG(Audio, "✅ AVCDiscovery: Sample rate change command accepted");
        } else {
            ASFW_LOG_WARNING(Audio, "AVCDiscovery: Sample rate change command response: %d",
                             static_cast<int>(result));
        }
    });

    mutableCaps.currentSampleRate = kTargetSampleRate;
    ASFW_LOG(Audio, "AVCDiscovery: Assuming 48kHz - nub will use this rate");
}

ASFW::Audio::Model::ASFWAudioDevice AVCDiscovery::BuildAudioDeviceConfig(
    uint64_t guid,
    const Discovery::FWDevice& device,
    const Music::MusicSubunit& musicSubunit) const {
    auto& mutableCaps = const_cast<Music::MusicSubunitCapabilities&>(musicSubunit.GetCapabilities());
    auto audioConfig = mutableCaps.GetAudioDeviceConfiguration();
    const std::string deviceName = audioConfig.GetDeviceName();

    const auto plugSummary = SummarizePlugChannels(musicSubunit.GetPlugs());
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

    std::vector<uint32_t> sampleRates;
    const uint32_t currentRate = static_cast<uint32_t>(mutableCaps.currentSampleRate);
    sampleRates.push_back(currentRate);
    for (double rate : mutableCaps.supportedSampleRates) {
        const uint32_t rateHz = static_cast<uint32_t>(rate);
        if (rateHz != currentRate) {
            sampleRates.push_back(rateHz);
        }
    }

    const uint32_t vendorId = device.GetVendorID();
    const uint32_t modelId = device.GetModelID();
    const char* streamModeReason = "default-nonblocking";
    const auto streamMode = ResolveStreamMode(mutableCaps, vendorId, modelId, streamModeReason);

    ASFW_LOG(Audio,
             "AVCDiscovery: stream mode selected vendor=0x%06x model=0x%06x mode=%{public}s reason=%{public}s",
             vendorId, modelId,
             ASFW::Audio::Quirks::StreamModeToString(streamMode),
             streamModeReason);
    ASFW_LOG(Audio,
             "AVCDiscovery: Publishing audio configuration for GUID=%llx: %{public}s, %u channels, %zu sample rates",
             guid, deviceName.c_str(), channelCount, sampleRates.size());

    ASFW::Audio::Model::ASFWAudioDevice config;
    config.guid = guid;
    config.vendorId = vendorId;
    config.modelId = modelId;
    config.deviceName = deviceName;
    config.channelCount = channelCount;
    config.inputChannelCount =
        (plugSummary.outputAudioMaxChannels > 0) ? plugSummary.outputAudioMaxChannels : channelCount;
    config.outputChannelCount =
        (plugSummary.inputAudioMaxChannels > 0) ? plugSummary.inputAudioMaxChannels : channelCount;
    config.sampleRates = std::move(sampleRates);
    config.currentSampleRate = currentRate;
    config.inputPlugName = mutableCaps.inputPlugName;
    config.outputPlugName = mutableCaps.outputPlugName;
    config.streamMode = streamMode;
    return config;
}

void AVCDiscovery::PublishReadyAudioConfig(uint64_t guid, const Audio::Model::ASFWAudioDevice& config) {
    if (!audioConfigListener_) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: no audio config listener; dropping config for GUID=%llx",
                       guid);
        return;
    }

    audioConfigListener_->OnAVCAudioConfigurationReady(guid, config);
}

void AVCDiscovery::PrefetchDuetStateAndCreateNub(
    uint64_t guid,
    const std::shared_ptr<AVCUnit>& avcUnit,
    const Audio::Model::ASFWAudioDevice& config) {
    if (!avcUnit) {
        if (!audioConfigListener_) {
            ASFW_LOG_ERROR(Audio,
                           "AVCDiscovery: no audio config listener; dropping Duet fallback config for GUID=%llx",
                           guid);
            return;
        }
        audioConfigListener_->OnAVCAudioConfigurationReady(guid, config);
        return;
    }

    auto device = avcUnit->GetDevice();
    if (!device) {
        if (!audioConfigListener_) {
            ASFW_LOG_ERROR(Audio,
                           "AVCDiscovery: no audio config listener; dropping Duet fallback config for GUID=%llx",
                           guid);
            return;
        }
        audioConfigListener_->OnAVCAudioConfigurationReady(guid, config);
        return;
    }

    auto protocol = std::make_shared<Audio::Oxford::Apogee::ApogeeDuetProtocol>(
        busOps_,
        busInfo_,
        device->GetNodeID(),
        &avcUnit->GetFCPTransport());
    auto state = std::make_shared<DuetPrefetchState>();
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto finish = std::make_shared<std::function<void(const char*)>>();

    *finish = [this, guid, config, state, completed](const char* reason) {
        if (completed->exchange(true)) {
            return;
        }

        if (lock_) {
            IOLockLock(lock_);
            duetPrefetchByGuid_[guid] = *state;
            IOLockUnlock(lock_);
        }

        ASFW_LOG(Audio,
                 "AVCDiscovery: Duet prefetch complete GUID=%llx reason=%{public}s input=%d mixer=%d output=%d display=%d fw=%d hw=%d timedOut=%d",
                 guid,
                 reason ? reason : "unknown",
                 state->inputParams.has_value(),
                 state->mixerParams.has_value(),
                 state->outputParams.has_value(),
                 state->displayParams.has_value(),
                 state->firmwareId.has_value(),
                 state->hardwareId.has_value(),
                 state->timedOut);

        auto finalConfig = config;
        ConfigureDuetPhantomOverrides(finalConfig, state->inputParams);

        if (!audioConfigListener_) {
            ASFW_LOG_ERROR(Audio,
                           "AVCDiscovery: no audio config listener; dropping Duet config for GUID=%llx",
                           guid);
            return;
        }
        audioConfigListener_->OnAVCAudioConfigurationReady(guid, finalConfig);
    };

    if (rescanQueue_) {
        auto timeoutState = state;
        auto timeoutDone = completed;
        auto timeoutFinish = finish;
        rescanQueue_->DispatchAsync(^{
            IOSleep(kDuetPrefetchTimeoutMs);
            if (timeoutDone->load()) {
                return;
            }
            timeoutState->timedOut = true;
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet prefetch timeout GUID=%llx after %u ms (continuing)",
                             guid, kDuetPrefetchTimeoutMs);
            (*timeoutFinish)("timeout");
        });
    } else {
        ASFW_LOG_WARNING(Audio,
                         "AVCDiscovery: no rescan queue for Duet timeout guard (GUID=%llx) - using fallback defaults",
                         guid);
        state->timedOut = true;
        (*finish)("missing-timeout-queue");
        return;
    }

    protocol->GetInputParams([this, guid, protocol, state, completed, finish](
                                 IOReturn status,
                                 Audio::Oxford::Apogee::InputParams params) {
        if (completed->load()) {
            return;
        }
        if (status == kIOReturnSuccess) {
            state->inputParams = params;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet input prefetch failed GUID=%llx status=0x%x",
                             guid, status);
        }
        ContinueDuetPrefetchMixer(guid, protocol, state, completed, finish);
    });
}

void AVCDiscovery::ContinueDuetPrefetchMixer(
    uint64_t guid,
    const std::shared_ptr<Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchState>& state,
    const std::shared_ptr<std::atomic<bool>>& completed,
    const std::shared_ptr<std::function<void(const char*)>>& finish) {
    protocol->GetMixerParams([this, guid, protocol, state, completed, finish](
                                 IOReturn mixerStatus,
                                 Audio::Oxford::Apogee::MixerParams mixerParams) {
        if (completed->load()) {
            return;
        }
        if (mixerStatus == kIOReturnSuccess) {
            state->mixerParams = mixerParams;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet mixer prefetch failed GUID=%llx status=0x%x",
                             guid, mixerStatus);
        }
        ContinueDuetPrefetchOutput(guid, protocol, state, completed, finish);
    });
}

void AVCDiscovery::ContinueDuetPrefetchOutput(
    uint64_t guid,
    const std::shared_ptr<Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchState>& state,
    const std::shared_ptr<std::atomic<bool>>& completed,
    const std::shared_ptr<std::function<void(const char*)>>& finish) {
    protocol->GetOutputParams([this, guid, protocol, state, completed, finish](
                                  IOReturn outputStatus,
                                  Audio::Oxford::Apogee::OutputParams outputParams) {
        if (completed->load()) {
            return;
        }
        if (outputStatus == kIOReturnSuccess) {
            state->outputParams = outputParams;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet output prefetch failed GUID=%llx status=0x%x",
                             guid, outputStatus);
        }
        ContinueDuetPrefetchDisplay(guid, protocol, state, completed, finish);
    });
}

void AVCDiscovery::ContinueDuetPrefetchDisplay(
    uint64_t guid,
    const std::shared_ptr<Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchState>& state,
    const std::shared_ptr<std::atomic<bool>>& completed,
    const std::shared_ptr<std::function<void(const char*)>>& finish) {
    protocol->GetDisplayParams([this, guid, protocol, state, completed, finish](
                                   IOReturn displayStatus,
                                   Audio::Oxford::Apogee::DisplayParams displayParams) {
        if (completed->load()) {
            return;
        }
        if (displayStatus == kIOReturnSuccess) {
            state->displayParams = displayParams;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet display prefetch failed GUID=%llx status=0x%x",
                             guid, displayStatus);
        }
        ContinueDuetPrefetchFirmware(guid, protocol, state, completed, finish);
    });
}

void AVCDiscovery::ContinueDuetPrefetchFirmware(
    uint64_t guid,
    const std::shared_ptr<Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchState>& state,
    const std::shared_ptr<std::atomic<bool>>& completed,
    const std::shared_ptr<std::function<void(const char*)>>& finish) {
    protocol->GetFirmwareId([this, guid, protocol, state, completed, finish](
                                IOReturn fwStatus,
                                uint32_t firmwareId) {
        if (completed->load()) {
            return;
        }
        if (fwStatus == kIOReturnSuccess) {
            state->firmwareId = firmwareId;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet firmware-id prefetch failed GUID=%llx status=0x%x",
                             guid, fwStatus);
        }
        ContinueDuetPrefetchHardware(guid, protocol, state, completed, finish);
    });
}

void AVCDiscovery::ContinueDuetPrefetchHardware(
    uint64_t guid,
    const std::shared_ptr<Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchState>& state,
    const std::shared_ptr<std::atomic<bool>>& completed,
    const std::shared_ptr<std::function<void(const char*)>>& finish) {
    protocol->GetHardwareId([guid, state, completed, finish](
                                IOReturn hwStatus,
                                uint32_t hardwareId) {
        if (completed->load()) {
            return;
        }
        if (hwStatus == kIOReturnSuccess) {
            state->hardwareId = hardwareId;
        } else {
            ASFW_LOG_WARNING(Audio,
                             "AVCDiscovery: Duet hardware-id prefetch failed GUID=%llx status=0x%x",
                             guid, hwStatus);
        }
        (*finish)("complete");
    });
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
    duetPrefetchByGuid_.erase(guid);
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

    auto it = units_.find(guid);
    if (it != units_.end()) {
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    duetPrefetchByGuid_.erase(guid);
    IOLockUnlock(lock_);

    // Rebuild node ID map (terminated unit removed)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    (void)device;
}

void AVCDiscovery::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    (void)device;
}

void AVCDiscovery::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    (void)device;
}

void AVCDiscovery::OnDeviceRemoved(Discovery::Guid64 guid) {
    IOLockLock(lock_);

    units_.erase(guid);
    rescanAttempts_.erase(guid);
    duetPrefetchByGuid_.erase(guid);
    IOLockUnlock(lock_);

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

bool AVCDiscovery::IsApogeeDuet(const Discovery::FWDevice& device) const noexcept {
    return device.GetVendorID() == Audio::DeviceProtocolFactory::kApogeeVendorId &&
           device.GetModelID() == Audio::DeviceProtocolFactory::kApogeeDuetModelId;
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

void AVCDiscovery::SetTransmitRingBufferOnNubs(uint8_t* ringBuffer) {
    // This method is deprecated - shared TX queue is now in ASFWAudioNub
    // Kept for backwards compatibility logging
    (void)ringBuffer;
    IOLockLock(lock_);

    os_log_info(log_,
                "AVCDiscovery: SetTransmitRingBufferOnNubs called (deprecated - using shared queue now)");

    IOLockUnlock(lock_);
}
