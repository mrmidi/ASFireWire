// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../Duplex/FamilyStageWait.hpp"
#include "../../../Runtime/RateValidation.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

namespace {

[[nodiscard]] bool HasUsableRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    // CoreAudio visibility is not a wire-topology signal. A Weiss INT202, for
    // example, deliberately has zero HAL input channels while still reporting
    // and using a device->host DICE stream. Validate the physical DICE sections
    // through their slot counts instead.
    return caps.sampleRateHz != 0 &&
        caps.hostOutputPcmChannels != 0 &&
        caps.deviceToHostAm824Slots != 0 &&
        caps.hostToDeviceAm824Slots != 0;
}

void LogRuntimeCaps(const char* source, const AudioStreamRuntimeCaps& caps) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: runtime caps source=%{public}s rate=%u in=%u out=%u d2hSlots=%u h2dSlots=%u usable=%u",
             source,
             caps.sampleRateHz,
             caps.hostInputPcmChannels,
             caps.hostOutputPcmChannels,
             caps.deviceToHostAm824Slots,
             caps.hostToDeviceAm824Slots,
             HasUsableRuntimeCaps(caps) ? 1U : 0U);
}

void LogStreamConfigSummary(const char* label, const StreamConfig& config) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: %{public}s stream summary count=%u pcm=%u midi=%u am824=%u entrySize=%u parsedEntrySize=%u",
             label,
             config.numStreams,
             config.TotalPcmChannels(),
             config.TotalMidiPorts(),
             config.TotalAm824Slots(),
             config.entrySizeBytes,
             config.parsedEntrySizeBytes);
}

} // namespace

bool DICETcatProtocol::MakeDiceClockConfiguration(
    const AudioClockConfig& requested, DiceClockConfiguration& out) noexcept {
    if (!IsSupportedAudioClockConfig(requested) &&
        !(Runtime::kDiceHardwareBatch && DiceRateMode(requested.sampleRateHz))) {
        return false;
    }
    // The DICE adapter owns the register encoding: Linux selects the requested
    // rate by updating GLOBAL_CLOCK_SELECT while preserving the source bits
    // (dice-stream.c:60-85; dice-interface.h:80-95). Encode the requested rate
    // via the standard table; source stays Internal (bring-up policy).
    uint32_t clockSelect = 0;
    if (!DiceClockSelectForRate(requested.sampleRateHz, ClockSource::Internal,
                                clockSelect)) {
        return false;
    }
    out = DiceClockConfiguration{
        .sampleRateHz = requested.sampleRateHz,
        .clockSelect = clockSelect,
    };
    return true;
}

DICETcatProtocol::DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   Discovery::DeviceRegistry& routeRegistry,
                                   const Discovery::DeviceRouteToken& route,
                                   ::ASFW::IRM::IRMClient* irmClient,
                                   DiceWaitClock& waitClock,
                                   DiceNotificationRouter* notifications,
                                   DICETcatRuntimePolicy runtimePolicy)
    : busInfo_(busInfo)
    , irmClient_(irmClient)
    , io_(busOps, busInfo, routeRegistry, route)
    , diceReader_(io_)
    , deviceIo_(io_, diceReader_, waitClock)
    , notificationRouter_(notifications)
    , guid_(route.guid)
    , runtimePolicy_(runtimePolicy) {
    if (notificationRouter_) {
        notificationRouter_->Register(guid_, notifications_);
    }
}

DICETcatProtocol::~DICETcatProtocol() {
    if (rateFormatsLock_) IOLockFree(rateFormatsLock_);
    if (notificationRouter_) {
        notificationRouter_->Unregister(guid_, notifications_);
    }
}

IOReturn DICETcatProtocol::Initialize() {
    if (!rateFormatsLock_) return kIOReturnNoMemory;
    if (!driver_) {
        driver_.emplace(deviceIo_, busInfo_, notifications_,
                        DICEBringupPolicy{
                            .requireSourceLockBeforeStreamEnable =
                                runtimePolicy_.requireSourceLockBeforeStreamEnable,
                            .requireSourceLockAtConfirm =
                                runtimePolicy_.requireSourceLockAtConfirm,
                        });
        driver_->SetTeardownCancelToken(teardownCancel_);
    }

    initialized_ = true;
    ASFW_LOG(DICE, "DICETcatProtocol::Initialize defers generic discovery until runtime");
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::Shutdown() {
    if (driver_) {
        if (driver_->IsPrepared() || driver_->IsRunning()) {
            const IOReturn stopStatus = driver_->Stop();
            if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown duplex stop failed: 0x%x", stopStatus);
            }
        }
    }

    sections_ = {};
    sectionsLoaded_ = false;
    initialized_ = false;
    ResetRuntimeCaps();
    PublishRateFormations({});
    return kIOReturnSuccess;
}

bool DICETcatProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.deviceRateMask = deviceRateMask_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    outCaps.deviceToHostIsoChannel =
        static_cast<uint8_t>(deviceToHostIsoChannel_.load(std::memory_order_relaxed));
    outCaps.hostToDeviceIsoChannel =
        static_cast<uint8_t>(hostToDeviceIsoChannel_.load(std::memory_order_relaxed));

    // Per-stream geometry: the runtimeCapsValid_ acquire-load above establishes
    // happens-before with the writer's release-store, so the plain arrays are
    // safe to read here.
    outCaps.deviceToHostStreamCount = deviceToHostStreamCount_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceStreamCount = hostToDeviceStreamCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        outCaps.deviceToHostStreams[i] = deviceToHostStreams_[i];
        outCaps.hostToDeviceStreams[i] = hostToDeviceStreams_[i];
    }
    return true;
}

void DICETcatProtocol::EnsureRuntimeStreamGeometry(VoidCallback callback) {
    EnsureRuntimeCapsLoaded(std::move(callback));
}

void DICETcatProtocol::ReadRateObservation(
    std::function<void(IOReturn, RateHardwareObservation)> callback) {
    if (!initialized_ || !sectionsLoaded_) { callback(kIOReturnNotReady, {}); return; }
    diceReader_.ReadCapabilities([this, callback = std::move(callback)](
        IOReturn status, DICECapabilities observed) mutable {
        if (status != kIOReturnSuccess) { callback(status, {}); return; }
        // The clock can move externally between GLOBAL and stream reads.
        // Read it again: a mixed snapshot cannot authorize a HAL projection.
        diceReader_.ReadGlobalState(sections_,
            [this, observed, callback = std::move(callback)](IOReturn status, GlobalState after) mutable {
                if (status != kIOReturnSuccess) { callback(status, {}); return; }
                const bool stable = after.sampleRate == observed.global.sampleRate &&
                    after.clockSelect == observed.global.clockSelect &&
                    NominalRateHz(after.status) == after.sampleRate &&
                    std::ranges::any_of(kDiceRateTable, [&](const auto& rate) {
                        return rate.hz == after.sampleRate && rate.rateIndex ==
                            ((after.clockSelect & ClockSelect::kRateMask) >> ClockSelect::kRateShift);
                    });
                const bool locked = (!runtimePolicy_.requireSourceLockAtConfirm || IsSourceLocked(after.status)) &&
                    ((after.clockSelect & ClockSelect::kSourceMask) != static_cast<uint32_t>(ClockSource::ARX1) ||
                     (IsArx1Locked(after.extStatus) && !HasArx1Slip(after.extStatus)));
                callback(kIOReturnSuccess, {MakeDiceRuntimeCaps(after, observed.txStreams,
                    observed.rxStreams, runtimePolicy_.exposeDeviceToHostToCoreAudio), stable && locked});
            });
    });
}

void DICETcatProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
    if (driver_) {
        driver_->SetTeardownCancelToken(cancel);
    }
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

IOReturn DICETcatProtocol::LoadGeometry() {
    return AwaitStageStatus([&](auto callback) { EnsureRuntimeStreamGeometry(std::move(callback)); },
                            teardownCancel_);
}

bool DICETcatProtocol::DeviceSupportsRate(uint32_t rateHz) const noexcept {
    if (rateHz > 48000) {
        const auto formations = RateFormations();
        if (!formations || std::ranges::find(*formations, rateHz,
            &Runtime::RateFormation::sampleRateHz) == formations->end()) return false;
    }
    // TCAT refuses a rate outside CLOCK_CAPABILITIES before touching the device
    // (MidasFW SetNewSamplingRate). Before the first geometry read the mask is
    // unknown, and the request goes through as it always has.
    const uint32_t mask = deviceRateMask_.load(std::memory_order_relaxed);
    if (mask == 0 || !runtimeCapsValid_.load(std::memory_order_acquire)) {
        return true;
    }
    if (DiceClockCapsSupportRate(mask, rateHz)) {
        return true;
    }
    ASFW_LOG(DICE, "DICETcatProtocol: device does not support %u Hz (rates=0x%02x); refused",
             rateHz, mask);
    return false;
}

std::optional<AudioStreamRuntimeCaps> DICETcatProtocol::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> DICETcatProtocol::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(clock, diceClock) || !DeviceSupportsRate(clock.sampleRateHz)) {
        return std::unexpected(kIOReturnUnsupported);
    }

    // Remember the live clock so a later per-StartIO PrepareDuplex48k targets it
    // rather than reverting the device to 48 kHz (see selectedClock_).
    if (clock.sampleRateHz != 0) {
        selectedClock_ = clock;
    }

    const auto result = driver_->Prepare(channels, diceClock, /*refreshRuntimeCaps=*/true);
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
        // Device configuration, not a bring-up gate: a device that needs a
        // router program comes up silent without one, but the streams are
        // sound either way, so a failure here is logged and not propagated.
        ApplyStartupProgram();
    }
    return result;
}

// --- Startup router program ---------------------------------------------------

std::expected<uint32_t, IOReturn> DICETcatProtocol::ReadRouterEntryCount(
    const ExtensionSections& ext) {
    // The *current* configuration is what the device is really holding; the
    // router section is only a staging buffer, and reading it on a freshly
    // attached device returns zero even though the device is working.
    const uint32_t base = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.currentConfig);
    if (base == kDICEExtensionOffset) {
        return std::unexpected(kIOReturnUnsupported);
    }
    return deviceIo_.ReadQuad(base);
}

std::expected<void, IOReturn> DICETcatProtocol::WriteRouterProgram(
    const ExtensionSections& ext) {
    const uint32_t routerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.router);
    const uint32_t commandBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.command);
    if (routerBase == kDICEExtensionOffset || commandBase == kDICEExtensionOffset) {
        ASFW_LOG(DICE, "Startup router: device exposes no TCAT router/command section");
        return std::unexpected(kIOReturnUnsupported);
    }

    const uint32_t count = runtimePolicy_.startupRouterEntryCount;
    // Wire image: quadlet 0 is the entry count, then one quadlet per entry.
    std::vector<uint8_t> buffer(static_cast<size_t>(count + 1U) * 4U, 0U);
    FW::WriteBE32(buffer.data(), count);
    for (uint32_t i = 0; i < count; ++i) {
        FW::WriteBE32(buffer.data() + (i + 1U) * 4U, runtimePolicy_.startupRouterEntries[i]);
    }

    if (const auto written = deviceIo_.WriteBlock(routerBase, buffer); !written) {
        ASFW_LOG(DICE, "Startup router: write failed (0x%08x)", written.error());
        return std::unexpected(written.error());
    }

    // Commit for every rate mode, as the vendor driver does. A commit of the
    // low mode alone is accepted and does not take effect.
    const uint32_t opcode = ExtensionCommandOpcode::kExecute |
                            ExtensionCommandOpcode::kRateLow |
                            ExtensionCommandOpcode::kRateMiddle |
                            ExtensionCommandOpcode::kRateHigh |
                            ExtensionCommandOpcode::kLoadRouter;
    if (const auto committed =
            deviceIo_.WriteQuad(commandBase + ExtensionCommandOffset::kOpcode, opcode);
        !committed) {
        ASFW_LOG(DICE, "Startup router: commit failed (0x%08x)", committed.error());
        return std::unexpected(committed.error());
    }

    ASFW_LOG(DICE, "Startup router: programmed %u entries (opcode=0x%08x)", count, opcode);
    return {};
}

void DICETcatProtocol::WriteStartupMixerCells(const ExtensionSections& ext) {
    if (runtimePolicy_.startupMixerCells == nullptr ||
        runtimePolicy_.startupMixerCellCount == 0) {
        return;
    }
    const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.mixer);
    if (mixerBase == kDICEExtensionOffset) {
        return;
    }
    // Coefficients start one quadlet past the section base.
    uint32_t written = 0;
    for (uint32_t i = 0; i < runtimePolicy_.startupMixerCellCount; ++i) {
        const auto& cell = runtimePolicy_.startupMixerCells[i];
        if (deviceIo_.WriteQuad(mixerBase + 4U + cell.index * 4U, cell.gain)) {
            ++written;
        } else {
            // One bad cell should not abort the rest.
            ASFW_LOG(DICE, "Startup mixer: cell %u failed", cell.index);
        }
    }
    ASFW_LOG(DICE, "Startup mixer: wrote %u of %u coefficients", written,
             runtimePolicy_.startupMixerCellCount);
}

void DICETcatProtocol::ApplyStartupProgram() {
    if (runtimePolicy_.startupRouterEntries == nullptr ||
        runtimePolicy_.startupRouterEntryCount == 0) {
        return;
    }

    const auto ext = deviceIo_.ReadExtensionSections();
    if (!ext) {
        ASFW_LOG(DICE, "Startup program: extension sections unreadable (0x%08x)", ext.error());
        return;
    }

    // Do not latch on the first success. A burst of bus resets around bring-up
    // can leave the device holding nothing while every write still reports
    // success, and a latch then guarantees nobody ever puts the program back:
    // the device stays silent until someone reprograms it by hand. Read the
    // count the device is really holding and reprogram whenever it disagrees.
    const uint32_t want = runtimePolicy_.startupRouterEntryCount;
    const auto live = ReadRouterEntryCount(*ext);
    if (startupProgramApplied_ && live && *live == want) {
        return;
    }
    if (startupProgramApplied_) {
        ASFW_LOG(DICE, "Startup router lost: device holds %u of %u entries; reprogramming",
                 live ? *live : 0U, want);
        startupProgramApplied_ = false;
    }

    if (!WriteRouterProgram(*ext)) {
        return;
    }
    WriteStartupMixerCells(*ext);
    startupProgramApplied_ = true;
}

std::expected<AudioDuplexChannels, IOReturn> DICETcatProtocol::AssignChannels(const AudioDuplexChannels& channels) {
    // The IRM chose these channels; the device must be told the same ones the
    // host DMA will use. ArmDeviceRx refuses to run unprepared, so a refusal
    // here cannot leave the two sides on different channels.
    if (!driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    if (const IOReturn status = driver_->AssignChannels(channels); status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(DICE, "AssignChannels: refused d2h=%u h2d=%u kr=0x%x",
                       channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel, status);
        return std::unexpected(status);
    }
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> DICETcatProtocol::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_, timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> DICETcatProtocol::ArmDeviceRx() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    return driver_->ProgramRx();
}

std::expected<DuplexStageResult, IOReturn> DICETcatProtocol::ArmDeviceTxAndEnable() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    return driver_->ProgramTxAndEnable();
}

std::expected<DuplexConfirmResult, IOReturn> DICETcatProtocol::Confirm() {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }
    const auto result = driver_->Confirm();
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
    }
    return result;
}

std::expected<DuplexClockApplyResult, IOReturn> DICETcatProtocol::ApplyClockIdle(
    const AudioClockConfig& clock) {
    if (!initialized_ || !driver_) {
        return std::unexpected(kIOReturnNotReady);
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(clock, diceClock) || !DeviceSupportsRate(clock.sampleRateHz)) {
        return std::unexpected(kIOReturnUnsupported);
    }

    // Remember an idle rate change so the next StartIO's PrepareDuplex48k keeps
    // the device at this rate instead of rewriting CLOCK_SELECT back to 48 kHz
    // (see selectedClock_).
    if (clock.sampleRateHz != 0) {
        selectedClock_ = clock;
    }

    const auto result = driver_->ApplyClock(diceClock);
    if (result) {
        CacheRuntimeCaps(result->runtimeCaps);
    }
    return result;
}

IOReturn DICETcatProtocol::DisconnectPlayback() {
    // DICE has no per-direction connection to drop: its stop is one sequence
    // (GLOBAL_ENABLE off, every stream's ISOCHRONOUS cleared). The staged-stop
    // recipe that calls this is not DICE's.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::DisconnectCapture() {
    // As DisconnectPlayback.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::BreakConnections() {
    // No CMP connections to break; Stop undoes everything the bring-up armed.
    return kIOReturnUnsupported;
}

IOReturn DICETcatProtocol::Stop() {
    return StopDuplex();
}

void DICETcatProtocol::ReadDuplexHealth(HealthCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus, {});
            return;
        }

        diceReader_.ReadGlobalState(
            sections_,
            [this, callback = std::move(callback)](IOReturn status, GlobalState global) mutable {
                if (status != kIOReturnSuccess) {
                    callback(status, {});
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                (void)GetRuntimeAudioStreamCaps(caps);
                const uint32_t clockSource =
                    global.clockSelect & ClockSelect::kSourceMask;
                const bool clockReferenceHealthy =
                    clockSource != static_cast<uint32_t>(ClockSource::ARX1) ||
                    (IsArx1Locked(global.extStatus) && !HasArx1Slip(global.extStatus));

                callback(status,
                         DuplexHealthResult{
                             .generation = busInfo_.GetGeneration(),
                             .appliedClock =
                                 AudioClockConfig{
                                     .sampleRateHz = global.sampleRate,
                                 },
                             .runtimeCaps = caps,
                             .sourceLocked = IsSourceLocked(global.status),
                             .clockReferenceHealthy = clockReferenceHealthy,
                             .nominalRateHz = NominalRateHz(global.status),
                             .notification = global.notification,
                             .status = global.status,
                             .extStatus = global.extStatus,
                         });
            });
    });
}


IOReturn DICETcatProtocol::StopDuplex() {
    if (!driver_) {
        return kIOReturnSuccess;
    }
    return driver_->Stop();
}

void DICETcatProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                            std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    (void)avcUnit; // DICE is register-based; no AV/C.
    io_.UpdateRoute(route);
}

void DICETcatProtocol::EnsureSectionsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (sectionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    diceReader_.ReadGeneralSections([this, callback = std::move(callback)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DICETcatProtocol: failed to read general sections: 0x%x", status);
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
        ASFW_LOG(DICE,
                 "DICETcatProtocol: loaded sections global=%u/%u tx=%u/%u rx=%u/%u ext=%u/%u",
                 sections_.global.offset,
                 sections_.global.size,
                 sections_.txStreamFormat.offset,
                 sections_.txStreamFormat.size,
                 sections_.rxStreamFormat.offset,
                 sections_.rxStreamFormat.size,
                 sections_.extSync.offset,
                 sections_.extSync.size);
        callback(kIOReturnSuccess);
    });
}

void DICETcatProtocol::EnsureRuntimeCapsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (runtimeCapsValid_.load(std::memory_order_acquire) && RateFormations()) {
        callback(kIOReturnSuccess);
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus);
            return;
        }

        struct RuntimeCapsState {
            GlobalState global;
            StreamConfig tx;
            StreamConfig rx;
        };

        auto state = std::make_shared<RuntimeCapsState>();
        diceReader_.ReadGlobalState(
            sections_,
            [this, state, callback = std::move(callback)](IOReturn globalStatus, GlobalState global) mutable {
                if (globalStatus != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read global state: 0x%x", globalStatus);
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                ASFW_LOG(DICE,
                         "DICETcatProtocol: global state rate=%u clockSelect=0x%08x status=0x%08x extStatus=0x%08x notification=0x%08x",
                         global.sampleRate,
                         global.clockSelect,
                         global.status,
                         global.extStatus,
                         global.notification);
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            ASFW_LOG(DICE, "DICETcatProtocol: failed to read TX stream config: 0x%x", txStatus);
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        LogStreamConfigSummary("TX", state->tx);
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read RX stream config: 0x%x", rxStatus);
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                LogStreamConfigSummary("RX", state->rx);
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                AudioStreamRuntimeCaps caps{};
                                (void)GetRuntimeAudioStreamCaps(caps);
                                LogRuntimeCaps("standard-dice", caps);
                                if (!HasUsableRuntimeCaps(caps)) {
                                    ASFW_LOG(DICE,
                                             "DICETcatProtocol: standard DICE discovery produced zero or partial caps; audio publication should fail closed");
                                }
                                diceReader_.ReadRateFormats(caps.deviceRateMask,
                                    [this, caps, callback = std::move(callback)](IOReturn status,
                                                                                DiceRateFormats formats) mutable {
                                        // Linux's non-EAP fallback retains only the observed
                                        // rate mode. No clock probing or guessed scaling.
                                        if (status == kIOReturnAborted || status == kIOReturnNoDevice ||
                                            status == kIOReturnOffline) {
                                            callback(status); return;
                                        }
                                        if (status != kIOReturnSuccess) formats = {};
                                        const auto mode = DiceRateMode(caps.sampleRateHz);
                                        const auto observed = DiceObservedFormat(caps);
                                        if (mode && observed) formats[*mode] = *observed;
                                        auto formations = DiceFormations(caps.deviceRateMask, formats,
                                            runtimePolicy_.exposeDeviceToHostToCoreAudio);
                                        PublishRateFormations(
                                            std::make_shared<const std::vector<Runtime::RateFormation>>(
                                                std::move(formations)));
                                        callback(kIOReturnSuccess);
                                    });
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    const auto caps = MakeDiceRuntimeCaps(global, tx, rx,
        runtimePolicy_.exposeDeviceToHostToCoreAudio);

    // Per-channel device labels from the DICE TX/RX name sections, flattened
    // across streams in channel order. Written BEFORE CacheRuntimeCaps(caps)'s
    // release-store so GetChannelLabels readers see a consistent snapshot.
    // Host input == device TX, host output == device RX (AudioTypes.hpp).
    auto fillLabels = [](const StreamConfig& sc,
                         std::atomic<uint32_t>& outCount,
                         char (&outLabels)[kMaxChannelLabels][64]) noexcept {
        uint32_t idx = 0;
        const uint32_t streams = (sc.numStreams < kMaxAudioStreamsPerDirection)
                                     ? sc.numStreams
                                     : kMaxAudioStreamsPerDirection;
        for (uint32_t s = 0; s < streams && idx < kMaxChannelLabels; ++s) {
            for (const auto& name : SplitDiceLabels(sc.streams[s].labels)) {
                if (idx >= kMaxChannelLabels) {
                    break;
                }
                strlcpy(outLabels[idx], name.c_str(), sizeof(outLabels[idx]));
                ++idx;
            }
        }
        for (uint32_t z = idx; z < kMaxChannelLabels; ++z) {
            outLabels[z][0] = '\0';
        }
        outCount.store(idx, std::memory_order_relaxed);
    };
    fillLabels(tx, inputChannelLabelCount_, inputChannelLabels_);
    fillLabels(rx, outputChannelLabelCount_, outputChannelLabels_);

    CacheRuntimeCaps(caps);
}

bool DICETcatProtocol::GetChannelLabels(std::vector<std::string>& inNames,
                                        std::vector<std::string>& outNames) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }
    const uint32_t inCount = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                 ? inputChannelLabelCount_.load(std::memory_order_relaxed)
                                 : 0;
    const uint32_t outCount = outputChannelLabelCount_.load(std::memory_order_relaxed);
    inNames.clear();
    outNames.clear();
    for (uint32_t i = 0; i < inCount && i < kMaxChannelLabels; ++i) {
        inNames.emplace_back(inputChannelLabels_[i]);
    }
    for (uint32_t i = 0; i < outCount && i < kMaxChannelLabels; ++i) {
        outNames.emplace_back(outputChannelLabels_[i]);
    }
    return inCount > 0 || outCount > 0;
}

void DICETcatProtocol::CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    const uint32_t exposedInputChannels = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                              ? caps.hostInputPcmChannels
                                              : 0;
    hostInputPcmChannels_.store(exposedInputChannels, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(caps.deviceToHostAm824Slots, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(caps.hostOutputPcmChannels, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(caps.hostToDeviceAm824Slots, std::memory_order_relaxed);
    runtimeSampleRateHz_.store(caps.sampleRateHz, std::memory_order_relaxed);
    deviceRateMask_.store(caps.deviceRateMask, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(caps.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(caps.hostToDeviceIsoChannel, std::memory_order_relaxed);

    // Per-stream geometry: write the plain arrays + counts BEFORE the
    // release-store of runtimeCapsValid_ so readers that pass the acquire-load
    // observe a consistent snapshot.
    deviceToHostStreamCount_.store(caps.deviceToHostStreamCount, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(caps.hostToDeviceStreamCount, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = caps.deviceToHostStreams[i];
        hostToDeviceStreams_[i] = caps.hostToDeviceStreams[i];
    }

    runtimeCapsValid_.store(true, std::memory_order_release);
    LogRuntimeCaps("cache", caps);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    deviceRateMask_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    deviceToHostStreamCount_.store(0, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = AudioStreamWireInfo{};
        hostToDeviceStreams_[i] = AudioStreamWireInfo{};
    }
    inputChannelLabelCount_.store(0, std::memory_order_relaxed);
    outputChannelLabelCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxChannelLabels; ++i) {
        inputChannelLabels_[i][0] = '\0';
        outputChannelLabels_[i][0] = '\0';
    }
}

} // namespace ASFW::Audio::DICE::TCAT
