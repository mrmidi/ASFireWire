//
// AVCDiscovery.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery implementation
//

#include "AVCDiscovery.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Audio/Model/ASFWAudioDevice.hpp"
#include "../../Audio/Protocols/DeviceProtocolFactory.hpp"
#include "../../Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "../../Audio/Protocols/Oxford/OxfwStreamFormats.hpp"
#include "../../Audio/Protocols/DeviceStreamModeQuirks.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include "Music/MusicSubunit.hpp"
#include "../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "../../Audio/DriverKit/Config/AudioProfileRegistry.hpp"
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
constexpr uint32_t kDuetPrefetchTimeoutMs = 5000;
constexpr uint32_t kDuetFixedSampleRateHz = 48000;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCDiscovery::AVCDiscovery(IOService* driver,
                           Discovery::DeviceRegistry& deviceRegistry,
                           Discovery::IDeviceManager& deviceManager,
                           Protocols::Ports::FireWireBusOps& busOps,
                           Protocols::Ports::FireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler& timerScheduler,
                           ASFW::Audio::IAVCAudioConfigListener* audioConfigListener)
    : driver_(driver)
    , deviceRegistry_(deviceRegistry)
    , deviceManager_(deviceManager)
    , busOps_(busOps)
    , busInfo_(busInfo)
    , timerScheduler_(timerScheduler)
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
    Shutdown();

    // Shutdown() unregisters observers before stopping outstanding FCP work.
    // Do not free the lock until that lifecycle boundary has been established.

    // Clean up lock
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }

    os_log_info(log_, "AVCDiscovery: Destroyed");
}

void AVCDiscovery::Shutdown() {
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // Remove external producers first. Device callbacks may otherwise enqueue
    // a fresh AV/C command after FCP has been shut down.
    deviceManager_.UnregisterDeviceObserver(this);
    deviceManager_.UnregisterUnitObserver(this);

    std::vector<std::shared_ptr<AVCUnit>> units;
    std::vector<Scheduling::TimerToken> tokensToCancel;
    if (lock_) {
        IOLockLock(lock_);
        units.reserve(units_.size());
        for (const auto& [guid, unit] : units_) {
            (void)guid;
            if (unit) {
                units.push_back(unit);
            }
        }
        for (const auto& [guid, op] : activeDuetPrefetchByGuid_) {
            (void)guid;
            if (op && op->timeoutToken != Scheduling::kInvalidTimerToken) {
                tokensToCancel.push_back(op->timeoutToken);
                op->timeoutToken = Scheduling::kInvalidTimerToken;
            }
        }
        for (const auto& [guid, token] : rescanTimersByGuid_) {
            (void)guid;
            if (token != Scheduling::kInvalidTimerToken) {
                tokensToCancel.push_back(token);
            }
        }
        activeDuetPrefetchByGuid_.clear();
        rescanTimersByGuid_.clear();
        activeRescanSerialByGuid_.clear();
        fcpTransportsByNodeID_.clear();
        rescanAttempts_.clear();
        duetPrefetchByGuid_.clear();
        IOLockUnlock(lock_);
    }

    for (auto token : tokensToCancel) {
        timerScheduler_.Cancel(token);
    }

    for (const auto& unit : units) {
        unit->Shutdown();
    }
}

//==============================================================================
// IUnitObserver Interface
//==============================================================================

void AVCDiscovery::OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
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
    auto avcUnit = std::make_shared<AVCUnit>(device, unit, deviceRegistry_, busOps_, busInfo_, timerScheduler_);

    // Publish the unit to the shutdown owner before initializing it. A
    // termination callback can race discovery after our first atomic check;
    // if it wins, Shutdown() will see and stop this FCP producer.
    IOLockLock(lock_);
    if (shuttingDown_.load(std::memory_order_acquire)) {
        IOLockUnlock(lock_);
        avcUnit->Shutdown();
        return;
    }
    units_[guid] = avcUnit;
    IOLockUnlock(lock_);

    // The PHASE 88 is a BeBoB unit matched by stable Config ROM identity.
    // Linux BeBoB starts directly with unit PLUG_INFO and BridgeCo commands;
    // it does not require generic UNIT_INFO or SUBUNIT_INFO first. Keep that
    // wire ordering instead of letting generic AV/C discovery consume or race
    // its FCP route. Cross-validated: firewire/bebob/bebob.c:184-260 and
    // firewire/bebob/bebob_stream.c:908-940.
    if (DeviceProfiles::Audio::BeBoB::IsBeBoBDevice(device->GetVendorID(), device->GetModelID())) {
        ASFW_LOG(AVC,
                 "AVCDiscovery: BeBoB device matched; bypassing generic UNIT_INFO/SUBUNIT_INFO GUID=0x%016llx",
                 guid);
        // BeBoB intentionally bypasses generic AV/C discovery, so it must
        // publish its profile-owned configuration here. Otherwise it never
        // reaches AudioCoordinator/AudioNubPublisher and every start attempt
        // is correctly rejected as not-ready before FCP/CMP.
        // cross-validated: linux-sound-firewire-stack/firewire/bebob/bebob.c:184-260,
        // bebob_stream.c:908-940.
        const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();
        const uint32_t vendorId = device->GetVendorID();
        const uint32_t modelId = device->GetModelID();
        const std::string deviceName{device->GetModelName()};
        ::ASFW::Audio::BeBoB::StartBeBoBPlug0Discovery(
            *avcUnit, guid,
            [weakSelf, guid, vendorId, modelId, deviceName](const ::ASFW::Audio::BeBoB::DeviceModel& inventory) {
                const auto self = weakSelf.lock();
                if (!self || self->shuttingDown_.load(std::memory_order_acquire)) {
                    return;
                }
                self->PublishBeBoBAudioConfig(guid, vendorId, modelId, deviceName, inventory);
            });
        RebuildNodeIDMap();
        return;
    }

    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();

    // Initialize (probe subunits, plugs)
    avcUnit->Initialize([weakSelf, avcUnit, guid](bool success) {
        const auto self = weakSelf.lock();
        if (!self || self->shuttingDown_.load(std::memory_order_acquire)) {
            return;
        }
        if (!success) {
            os_log_error(self->log_,
                         "AVCDiscovery: AVCUnit initialization failed: GUID=%llx",
                         guid);
            return;
        }

        self->HandleInitializedUnit(guid, avcUnit);
    });

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

    // The OXFW971's Music subunit implements no descriptor mechanism (standard
    // descriptor access and the non-standard direct read both refuse on real
    // hardware; Linux snd-oxfw never consults the Music subunit either), so the
    // generic descriptor-driven path below can never publish this device.
    // Publish the hardware-verified profile-owned configuration instead — same
    // pattern as the BeBoB bypass in OnAVCUnitCreated.
    if (IsMackieOnyxIOxford(*device)) {
        PublishMackieOnyxIProfileOwnedConfig(guid, *device);
        return;
    }

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

    auto audioDeviceConfig = BuildAudioDeviceConfig(guid, *device, *musicSubunit);
    if (audioDeviceConfig.channelCount == 0 || audioDeviceConfig.sampleRates.empty() ||
        audioDeviceConfig.currentSampleRate == 0) {
        ASFW_LOG_WARNING(Audio,
                         "AVCDiscovery: Deferring audio nub for GUID=%llx; decoded format lacks %{public}s%{public}s%{public}s",
                         guid,
                         audioDeviceConfig.channelCount == 0 ? "channel count" : "",
                         audioDeviceConfig.channelCount == 0 && audioDeviceConfig.sampleRates.empty() ? " and " : "",
                         audioDeviceConfig.sampleRates.empty() ? "sample rate" : "");
        return;
    }
    if (IsApogeeDuet(*device)) {
        ASFW_LOG(Audio,
                 "AVCDiscovery: Apogee Duet detected (GUID=%llx) - prefetching vendor config before publishing config",
                 guid);
        PrefetchDuetStateAndCreateNub(guid, avcUnit, audioDeviceConfig);
        return;
    }

    PublishReadyAudioConfig(guid, audioDeviceConfig);
}

void AVCDiscovery::PublishBeBoBAudioConfig(uint64_t guid,
                                             uint32_t vendorId,
                                             uint32_t modelId,
                                             const std::string& deviceName,
                                             const ::ASFW::Audio::BeBoB::DeviceModel& inventory) {
    // Register a per-GUID BeBoB profile from discovery data. For Phase88 this
    // is superseded by the static Phase88Profile in the registry; for generic
    // BeBoB devices it provides discovery-derived geometry.
    ASFW::Isoch::Audio::AudioProfileRegistry::RegisterBeBoBProfile(guid, &inventory);

    constexpr uint8_t kPcmChannels = 10;
    constexpr uint8_t kMidiSlots = 1;
    constexpr uint32_t kSampleRateHz = 48000;

    // The live inventory must confirm the profile's duplex AM824 geometry.
    // It is intentionally independent of the BridgeCo formation rate code:
    // that list describes capabilities, while Phase88Protocol explicitly
    // programs FDF/SFC 48 kHz immediately before CMP connection.
    // cross-validated: linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:96-115.
    if (!inventory.SupportsDuplexFormation(kPcmChannels, kMidiSlots)) {
        ASFW_LOG_ERROR(Audio,
                       "[BeBoB] refusing BeBoB nub: inventory lacks duplex %u PCM + %u MIDI-slot formation GUID=0x%016llx",
                       static_cast<unsigned>(kPcmChannels), static_cast<unsigned>(kMidiSlots), guid);
        return;
    }

    ::ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = guid;
    config.vendorId = vendorId;
    config.modelId = modelId;
    config.deviceName = deviceName.empty() ? "PHASE 88 Rack FW" : deviceName;
    config.channelCount = kPcmChannels;
    config.inputChannelCount = kPcmChannels;
    config.outputChannelCount = kPcmChannels;
    config.sampleRates = {kSampleRateHz};
    config.currentSampleRate = kSampleRateHz;
    config.inputPlugName = "PHASE 88 Inputs";
    config.outputPlugName = "PHASE 88 Outputs";
    config.streamMode = ::ASFW::Audio::Model::StreamMode::kBlocking;

    ASFW_LOG(Audio,
             "[BeBoB] publishing BeBoB audio nub GUID=0x%016llx pcm=%u midiSlots=%u dbs=%u rate=%u mode=%{public}s",
             guid, static_cast<unsigned>(kPcmChannels), static_cast<unsigned>(kMidiSlots),
             static_cast<unsigned>(kPcmChannels + kMidiSlots), kSampleRateHz, "blocking");
    PublishReadyAudioConfig(guid, config);
}

void AVCDiscovery::PublishMackieOnyxIProfileOwnedConfig(uint64_t guid,
                                                        const Discovery::FWDevice& device) {
    // Wire geometry captured live from an Onyx 820i (AV/C unit-level EXTENDED
    // STREAM FORMAT INFORMATION, 2026-08-17): capture 8ch MBLA / playback 2ch
    // MBLA, compound AM824, one isoch plug per direction, no MIDI. Only the
    // captured current rate is offered until the AV/C rate transition is wired
    // for this device (M4) — same policy as MackieOnyx820iProfile, which owns
    // the matching isoch geometry.
    constexpr uint32_t kCaptureChannels = 8;   // device -> host (CoreAudio input)
    constexpr uint32_t kPlaybackChannels = 2;  // host -> device (CoreAudio output)
    constexpr uint32_t kSampleRateHz = 44100;   // captured current rate
    constexpr uint32_t kAltSampleRateHz = 48000;  // via AV/C SIGNAL FORMAT (M4)

    ::ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = guid;
    config.vendorId = device.GetVendorID();
    config.modelId = device.GetModelID();
    config.deviceName =
        std::string(::ASFW::Audio::DeviceProtocolFactory::kMackieVendorName) + " " +
        ::ASFW::Audio::DeviceProtocolFactory::kOnyxIOxfwModelName;
    config.channelCount = kCaptureChannels;
    config.inputChannelCount = kCaptureChannels;
    config.outputChannelCount = kPlaybackChannels;
    config.sampleRates = {kSampleRateHz, kAltSampleRateHz};
    config.currentSampleRate = kSampleRateHz;
    config.inputPlugName = "Onyx Capture";
    config.outputPlugName = "Onyx Monitor Return";
    // LOUD vendor-wide rule (Linux snd-oxfw oxfw.c:189-196); also forced in
    // DeviceStreamModeQuirks so every downstream mode resolution agrees.
    config.streamMode = ::ASFW::Audio::Model::StreamMode::kBlocking;

    ASFW_LOG(Audio,
             "[Onyx] publishing profile-owned audio nub GUID=0x%016llx in=%u out=%u rate=%u mode=blocking",
             guid, kCaptureChannels, kPlaybackChannels, kSampleRateHz);
    PublishReadyAudioConfig(guid, config);
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
        return;
    }
    mutableCaps.currentSampleRate = 0.0;
    ASFW_LOG_WARNING(Audio, "AVCDiscovery: Current sample rate unavailable from decoded format");
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

void AVCDiscovery::PublishReadyAudioConfig(uint64_t guid, const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    if (!audioConfigListener_) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: no audio config listener; dropping config for GUID=%llx",
                       guid);
        return;
    }

    audioConfigListener_->OnAVCAudioConfigurationReady(guid, config);
}

void AVCDiscovery::FinishDuetPrefetch(
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config,
    const char* reason) {
    if (!operation || operation->completed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    Scheduling::TimerToken tokenToCancel = Scheduling::kInvalidTimerToken;
    bool shouldPublish = false;
    bool ownsActiveOperation = false;
    ::ASFW::Audio::Model::ASFWAudioDevice finalConfig = config;

    if (lock_) {
        IOLockLock(lock_);
        auto it = activeDuetPrefetchByGuid_.find(operation->route.guid);
        if (it != activeDuetPrefetchByGuid_.end() && it->second == operation) {
            ownsActiveOperation = true;
            tokenToCancel = operation->timeoutToken;
            operation->timeoutToken = Scheduling::kInvalidTimerToken;
            duetPrefetchByGuid_[operation->route.guid] = operation->state;
            activeDuetPrefetchByGuid_.erase(it);
        }
        if (!ownsActiveOperation) {
            tokenToCancel = operation->timeoutToken;
            operation->timeoutToken = Scheduling::kInvalidTimerToken;
        }
        shouldPublish = ownsActiveOperation && !shuttingDown_.load(std::memory_order_acquire) &&
                        operation->state.clockVerified && deviceRegistry_.IsCurrent(operation->route);
        IOLockUnlock(lock_);
    }

    if (tokenToCancel != Scheduling::kInvalidTimerToken) {
        timerScheduler_.Cancel(tokenToCancel);
    }

    ASFW_LOG(Audio,
             "AVCDiscovery: Duet prepublish complete GUID=%llx reason=%{public}s input=%d mixer=%d output=%d display=%d fw=%d hw=%d clockVerified=%d clockStatus=0x%x timedOut=%d",
             operation->route.guid,
             reason ? reason : "unknown",
             operation->state.inputParams.has_value(),
             operation->state.mixerParams.has_value(),
             operation->state.outputParams.has_value(),
             operation->state.displayParams.has_value(),
             operation->state.firmwareId.has_value(),
             operation->state.hardwareId.has_value(),
             operation->state.clockVerified,
             operation->state.clockStatus,
             operation->state.timedOut);

    if (!shouldPublish) {
        if (!operation->state.clockVerified) {
            ASFW_LOG_ERROR(Audio,
                           "AVCDiscovery: Deferring Duet audio nub GUID=%llx; fixed %u Hz prepublish failed status=0x%x reason=%{public}s",
                           operation->route.guid,
                           kDuetFixedSampleRateHz,
                           operation->state.clockStatus,
                           reason ? reason : "unknown");
        }
        return;
    }

    finalConfig.sampleRates = {kDuetFixedSampleRateHz};
    finalConfig.currentSampleRate = kDuetFixedSampleRateHz;

    if (!audioConfigListener_) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: no audio config listener; dropping Duet config for GUID=%llx",
                       operation->route.guid);
        return;
    }
    audioConfigListener_->OnAVCAudioConfigurationReady(operation->route.guid, finalConfig);
}

void AVCDiscovery::PrefetchDuetStateAndCreateNub(
    uint64_t guid,
    const std::shared_ptr<AVCUnit>& avcUnit,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    if (!avcUnit) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: Deferring Duet audio nub GUID=%llx; no AV/C unit for fixed %u Hz prepublish",
                       guid,
                       kDuetFixedSampleRateHz);
        return;
    }

    auto device = avcUnit->GetDevice();
    if (!device) {
        ASFW_LOG_ERROR(Audio,
                       "AVCDiscovery: Deferring Duet audio nub GUID=%llx; no device for fixed %u Hz prepublish",
                       guid,
                       kDuetFixedSampleRateHz);
        return;
    }

    std::shared_ptr<DuetPrefetchOperation> oldOp;
    auto operation = std::make_shared<DuetPrefetchOperation>();
    const auto route = deviceRegistry_.CurrentRoute(guid);
    if (!route.has_value()) {
        ASFW_LOG(Audio, "AVCDiscovery: Duet prefetch refused without live route GUID=%llx", guid);
        return;
    }

    if (lock_) {
        IOLockLock(lock_);
        operation->operationSerial = ++nextDuetPrefetchEpoch_;
        operation->route = *route;
        auto it = activeDuetPrefetchByGuid_.find(guid);
        if (it != activeDuetPrefetchByGuid_.end()) {
            oldOp = it->second;
            activeDuetPrefetchByGuid_.erase(it);
        }
        activeDuetPrefetchByGuid_[guid] = operation;
        IOLockUnlock(lock_);
    }

    if (oldOp) {
        FinishDuetPrefetch(oldOp, config, "superseded");
    }

    auto protocol = std::make_shared<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>(
        busOps_,
        busInfo_,
        *route,
        // Register reads resolve through the registry, not through the CMP
        // client — this path deliberately has no CMP/IRM client (FW-142).
        &deviceRegistry_,
        &avcUnit->GetFCPTransport(),
        nullptr,
        nullptr,
        100U,
        &timerScheduler_);

    const uint64_t timeoutNs = static_cast<uint64_t>(kDuetPrefetchTimeoutMs) * 1000000ULL;
    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();

    Scheduling::TimerToken token = timerScheduler_.ScheduleAfter(
        timeoutNs,
        [weakSelf, operation, config]() {
            const auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            if (self->lock_) {
                IOLockLock(self->lock_);
                operation->state.timedOut = true;
                operation->timeoutToken = Scheduling::kInvalidTimerToken;
                IOLockUnlock(self->lock_);
            }
            self->FinishDuetPrefetch(operation, config, "timeout");
        });

    if (lock_) {
        IOLockLock(lock_);
        operation->timeoutToken = token;
        IOLockUnlock(lock_);
    }

    protocol->GetInputParams([this, guid, protocol, operation, config](
                                 IOReturn status,
                                 ::ASFW::Audio::Oxford::Apogee::InputParams params) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (status == kIOReturnSuccess) {
                operation->state.inputParams = params;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet input prefetch failed GUID=%llx status=0x%x",
                                 guid, status);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchMixer(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchMixer(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    protocol->GetMixerParams([this, guid, protocol, operation, config](
                                 IOReturn mixerStatus,
                                 ::ASFW::Audio::Oxford::Apogee::MixerParams mixerParams) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (mixerStatus == kIOReturnSuccess) {
                operation->state.mixerParams = mixerParams;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet mixer prefetch failed GUID=%llx status=0x%x",
                                 guid, mixerStatus);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchOutput(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchOutput(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    protocol->GetOutputParams([this, guid, protocol, operation, config](
                                  IOReturn outputStatus,
                                  ::ASFW::Audio::Oxford::Apogee::OutputParams outputParams) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (outputStatus == kIOReturnSuccess) {
                operation->state.outputParams = outputParams;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet output prefetch failed GUID=%llx status=0x%x",
                                 guid, outputStatus);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchDisplay(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchDisplay(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    protocol->GetDisplayParams([this, guid, protocol, operation, config](
                                   IOReturn displayStatus,
                                   ::ASFW::Audio::Oxford::Apogee::DisplayParams displayParams) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (displayStatus == kIOReturnSuccess) {
                operation->state.displayParams = displayParams;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet display prefetch failed GUID=%llx status=0x%x",
                                 guid, displayStatus);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchFirmware(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchFirmware(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    protocol->GetFirmwareId([this, guid, protocol, operation, config](
                                IOReturn fwStatus,
                                uint32_t firmwareId) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (fwStatus == kIOReturnSuccess) {
                operation->state.firmwareId = firmwareId;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet firmware-id prefetch failed GUID=%llx status=0x%x",
                                 guid, fwStatus);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchHardware(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchHardware(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    protocol->GetHardwareId([this, guid, protocol, operation, config](
                                IOReturn hwStatus,
                                uint32_t hardwareId) {
        if (!IsDuetPrefetchCurrent(operation)) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
            if (hwStatus == kIOReturnSuccess) {
                operation->state.hardwareId = hardwareId;
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "AVCDiscovery: Duet hardware-id prefetch failed GUID=%llx status=0x%x",
                                 guid, hwStatus);
            }
            IOLockUnlock(lock_);
        }
        ContinueDuetPrefetchStreamFormats(guid, protocol, operation, config);
    });
}

void AVCDiscovery::ContinueDuetPrefetchStreamFormats(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    // FW-139: observational only. This runs the two-tier discovery and logs
    // what the Duet reports, but the advertised rate set stays pinned to
    // kDuetFixedSampleRateHz below. Unpinning it depends on which tier the
    // device actually answers, and that is a hardware question — the reference
    // stacks disagree on whether OXFW devices implement the format list at all.
    // Read the `stream formats:` line from the Oxfw ring to settle it.
    //
    // Safe to run pre-publication: tier 1 is a STATUS query and tier 2 probes
    // with SPECIFIC INQUIRY, so nothing here changes device state.
    auto* transport = protocol->GetFCPTransport();
    if (transport == nullptr) {
        ContinueDuetPrefetchClock(guid, protocol, operation, config);
        return;
    }

    ::ASFW::Audio::Oxford::DetectStreamFormats(
        *transport, /*isOutput=*/false,
        [this, guid, protocol, operation, config](
            IOReturn status, const ::ASFW::Audio::Oxford::StreamFormatSet& formats) {
            if (!IsDuetPrefetchCurrent(operation)) {
                return;
            }
            if (status == kIOReturnSuccess) {
                // The per-entry detail is logged by the Oxford layer; this line
                // exists to tie the result to a GUID and to record whether the
                // pinned 48 kHz is even in the device's own set.
                const auto rates = formats.Rates();
                ASFW_LOG(Oxfw,
                         "Duet stream formats: %zu rate(s), %{public}s, has48k=%d GUID=%llx",
                         rates.size(), formats.assumed ? "assumed" : "reported",
                         formats.SupportsRate(kDuetFixedSampleRateHz) ? 1 : 0, guid);
            } else {
                ASFW_LOG_WARNING(Oxfw, "Duet stream-format discovery failed status=0x%x GUID=%llx",
                                 status, guid);
            }
            ContinueDuetPrefetchClock(guid, protocol, operation, config);
        });
}

void AVCDiscovery::ContinueDuetPrefetchClock(
    uint64_t guid,
    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
    const std::shared_ptr<DuetPrefetchOperation>& operation,
    const ::ASFW::Audio::Model::ASFWAudioDevice& config) {
    // Linux OXFW configures the input formation before output and verifies
    // the selected format before stream construction.  ApplyClockConfig
    // implements that same control-plane sequence and readback; it owns no
    // CMP/IRM resources, so this remains strictly pre-stream.
    protocol->ApplyClockConfig(
        ::ASFW::Audio::AudioClockConfig{.sampleRateHz = kDuetFixedSampleRateHz},
        [this, guid, protocol, operation, config](IOReturn clockStatus,
                                                   const ::ASFW::Audio::ClockApplyResult& result) {
            (void)guid;
            if (!IsDuetPrefetchCurrent(operation)) {
                return;
            }
            if (lock_) {
                IOLockLock(lock_);
                operation->state.clockStatus = clockStatus;
                operation->state.clockVerified =
                    clockStatus == kIOReturnSuccess &&
                    result.appliedClock.sampleRateHz == kDuetFixedSampleRateHz;
                IOLockUnlock(lock_);
            }
            FinishDuetPrefetch(operation, config, operation->state.clockVerified ? "complete" : "clock-failed");
        });
}

void AVCDiscovery::ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    if (!avcUnit) {
        return;
    }
    const auto route = deviceRegistry_.CurrentRoute(guid);
    if (!route.has_value()) {
        return;
    }

    constexpr uint8_t kMaxAutoRescanAttempts = 1;
    constexpr uint32_t kRescanDelayMs = 250;

    uint8_t attempt = 0;
    uint64_t operationSerial = 0;
    Scheduling::TimerToken oldRescanToken = Scheduling::kInvalidTimerToken;
    IOLockLock(lock_);
    auto rescanIt = rescanTimersByGuid_.find(guid);
    if (rescanIt != rescanTimersByGuid_.end()) {
        oldRescanToken = rescanIt->second;
        rescanTimersByGuid_.erase(rescanIt);
    }
    auto& count = rescanAttempts_[guid];
    if (count >= kMaxAutoRescanAttempts) {
        IOLockUnlock(lock_);
        if (oldRescanToken != Scheduling::kInvalidTimerToken) {
            timerScheduler_.Cancel(oldRescanToken);
        }
        ASFW_LOG(Audio,
                 "AVCDiscovery: Auto re-scan limit reached for GUID=%llx (attempts=%u)",
                 guid, count);
        return;
    }
    count++;
    attempt = count;
    operationSerial = ++nextRescanOperationSerial_;
    activeRescanSerialByGuid_[guid] = operationSerial;
    IOLockUnlock(lock_);

    if (oldRescanToken != Scheduling::kInvalidTimerToken) {
        timerScheduler_.Cancel(oldRescanToken);
    }

    auto unit = avcUnit;
    const std::weak_ptr<AVCDiscovery> weakSelf = weak_from_this();
    const auto token = timerScheduler_.ScheduleAfter(
        static_cast<uint64_t>(kRescanDelayMs) * 1000000ULL,
        [weakSelf, route = *route, operationSerial, attempt, unit]() {
            const auto self = weakSelf.lock();
            if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                return;
            }

            if (self->lock_) {
                IOLockLock(self->lock_);
                self->rescanTimersByGuid_.erase(route.guid);
                IOLockUnlock(self->lock_);
            }

            auto work = [weakSelf, route, operationSerial, attempt, unit]() {
                const auto self = weakSelf.lock();
                if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                    return;
                }

                ASFW_LOG(Audio, "AVCDiscovery: Auto re-scan attempt %u for GUID=%llx", attempt, route.guid);
                unit->ReScan([weakSelf, route, operationSerial, unit](bool success) {
                    const auto self = weakSelf.lock();
                    if (!self || !self->IsRescanCurrent(route, operationSerial)) {
                        return;
                    }

                    if (!success) {
                        ASFW_LOG_ERROR(Audio,
                                       "AVCDiscovery: AVCUnit re-scan failed GUID=%llx",
                                       route.guid);
                        return;
                    }

                    self->HandleInitializedUnit(route.guid, unit);
                });
            };

            if (self->rescanQueue_) {
                self->rescanQueue_->DispatchAsync(^{ work(); });
            } else {
                work();
            }
        });

    if (lock_) {
        IOLockLock(lock_);
        rescanTimersByGuid_[guid] = token;
        IOLockUnlock(lock_);
    }
}

void AVCDiscovery::OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
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
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);
    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit resumed: GUID=%llx",
                    guid);
        // Unit is now available again
    }
    IOLockUnlock(lock_);

    if (avcUnit) {
        avcUnit->OnRouteRevalidated();
    }

    // Rebuild node ID map (resumed units back in routing)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t guid = GetUnitGUID(unit);

    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        os_log_info(log_,
                    "AVCDiscovery: AV/C unit terminated: GUID=%llx",
                    guid);
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    duetPrefetchByGuid_.erase(guid);
    IOLockUnlock(lock_);

    // A response-router lease may keep the transport alive after its unit has
    // left discovery. Stop it explicitly so no pending callback survives the
    // unit-removal lifecycle boundary.
    if (avcUnit) {
        avcUnit->Shutdown();
    }

    // Rebuild node ID map (terminated unit removed)
    RebuildNodeIDMap();
}

void AVCDiscovery::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    (void)device;
}

void AVCDiscovery::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    (void)device;
}

void AVCDiscovery::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    (void)device;
}

void AVCDiscovery::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    std::shared_ptr<AVCUnit> avcUnit;
    IOLockLock(lock_);

    const auto it = units_.find(guid);
    if (it != units_.end()) {
        avcUnit = it->second;
        units_.erase(it);
    }
    rescanAttempts_.erase(guid);
    duetPrefetchByGuid_.erase(guid);
    IOLockUnlock(lock_);

    if (avcUnit) {
        avcUnit->Shutdown();
    }

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
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
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
    // Legacy borrowing API. New asynchronous callers must use Acquire...()
    // and retain the returned shared owner across their complete operation.
    const auto transport = AcquireFCPTransportForNodeID(nodeID);
    return transport.get();
}

std::shared_ptr<FCPTransport> AVCDiscovery::AcquireFCPTransportForNodeID(uint16_t nodeID) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    IOLockLock(lock_);

    // Normalize to node number (low 6 bits) to match map keys
    const uint16_t nodeNumber = static_cast<uint16_t>(nodeID & 0x3Fu);

    auto it = fcpTransportsByNodeID_.find(nodeNumber);
    std::shared_ptr<FCPTransport> result = (it != fcpTransportsByNodeID_.end())
                                                ? it->second
                                                : nullptr;

    IOLockUnlock(lock_);

    return result;
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCDiscovery::OnBusReset(uint32_t newGeneration) {
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
    os_log_info(log_,
                "AVCDiscovery: Bus reset (generation %u)",
                newGeneration);

    // ControllerCore invalidates DeviceRegistry routes before this callback.
    // Cancel token-bound prefetches before their old generation can publish.
    std::vector<std::shared_ptr<DuetPrefetchOperation>> cancelledPrefetches;
    std::vector<Scheduling::TimerToken> cancelledRescans;

    // Notify all AVCUnits of bus reset
    IOLockLock(lock_);

    for (auto& [guid, avcUnit] : units_) {
        avcUnit->OnBusReset(newGeneration);
    }

    cancelledPrefetches.reserve(activeDuetPrefetchByGuid_.size());
    for (const auto& [guid, operation] : activeDuetPrefetchByGuid_) {
        (void)guid;
        if (operation) {
            cancelledPrefetches.push_back(operation);
        }
    }
    activeDuetPrefetchByGuid_.clear();
    activeRescanSerialByGuid_.clear();
    for (const auto& [guid, token] : rescanTimersByGuid_) {
        (void)guid;
        if (token != Scheduling::kInvalidTimerToken) {
            cancelledRescans.push_back(token);
        }
    }
    rescanTimersByGuid_.clear();

    IOLockUnlock(lock_);

    for (const auto& operation : cancelledPrefetches) {
        FinishDuetPrefetch(operation, {}, "bus-reset");
    }
    for (const auto token : cancelledRescans) {
        timerScheduler_.Cancel(token);
    }

    // Rebuild node ID map (node IDs changed)
    RebuildNodeIDMap();
}

bool AVCDiscovery::IsDuetPrefetchCurrent(
    const std::shared_ptr<DuetPrefetchOperation>& operation) const noexcept {
    if (!operation || operation->completed.load(std::memory_order_acquire) ||
        shuttingDown_.load(std::memory_order_acquire) || !lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto it = activeDuetPrefetchByGuid_.find(operation->route.guid);
    const bool active = it != activeDuetPrefetchByGuid_.end() && it->second == operation &&
                        it->second->operationSerial == operation->operationSerial;
    IOLockUnlock(lock_);
    return active && deviceRegistry_.IsCurrent(operation->route);
}

bool AVCDiscovery::IsRescanCurrent(const Discovery::DeviceRouteToken& route,
                                   uint64_t operationSerial) const noexcept {
    if (!route || operationSerial == 0 || shuttingDown_.load(std::memory_order_acquire) || !lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto it = activeRescanSerialByGuid_.find(route.guid);
    const bool active = it != activeRescanSerialByGuid_.end() && it->second == operationSerial;
    IOLockUnlock(lock_);
    return active && deviceRegistry_.IsCurrent(route);
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
    return device.GetVendorID() == ::ASFW::Audio::DeviceProtocolFactory::kApogeeVendorId &&
           device.GetModelID() == ::ASFW::Audio::DeviceProtocolFactory::kApogeeDuetModelId;
}

bool AVCDiscovery::IsMackieOnyxIOxford(const Discovery::FWDevice& device) const noexcept {
    return device.GetVendorID() == ::ASFW::Audio::DeviceProtocolFactory::kMackieVendorId &&
           device.GetModelID() == ::ASFW::Audio::DeviceProtocolFactory::kOnyxIOxfwModelId;
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
    if (shuttingDown_.load(std::memory_order_acquire)) {
        return;
    }
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
        
        auto transport = avcUnit->GetFCPTransportShared();
        if (!transport) {
            continue;
        }
        fcpTransportsByNodeID_[nodeNumber] = std::move(transport);

        os_log_debug(log_,
                     "AVCDiscovery: Mapped fullNodeID=0x%04x (node=%u) → FCPTransport (GUID=%llx)",
                     fullNodeID, nodeNumber, guid);
    }

    IOLockUnlock(lock_);
}
