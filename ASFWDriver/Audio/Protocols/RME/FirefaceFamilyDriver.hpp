// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "../Duplex/FamilyDriver.hpp"
#include "../Duplex/FamilyStageWait.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../Session/SessionClock.hpp"
#include "../../../Logging/Logging.hpp"
#include "FirefaceRegisters.hpp"
#include "FirefaceSettings.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace ASFW::Audio::RME {

// What the last Configure found for the configuration upload (dry run).
struct SettingsDryRun {
    std::optional<FlashSettings> flash;
    std::optional<FlashDecodeError> decodeError;
    std::optional<ConfigWords> config;
    std::optional<StatusComparison> status;
};

struct ClockStatus {
    uint32_t configuredSource{0};
    uint32_t activeSource{0};
    uint32_t q0{0};
    uint32_t q1{0};
    bool configured48k{false};
    bool internalActive{false};
    bool externalLocked48k{false};
};

class FirefaceFamilyDriver final : public FamilyDriver {
public:
    FirefaceFamilyDriver(Protocols::Ports::ProtocolRegisterIO& io, FirefaceModel model,
                         bool s800) noexcept : io_(io), model_(model), s800_(s800),
                         stateGeneration_(io.Generation()) {}

    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override { cancel_ = cancel; }
    [[nodiscard]] IOReturn LoadGeometry() override { caps_ = MakeCaps(); return kIOReturnSuccess; }
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override { return caps_; }
    [[nodiscard]] ResourcePolicy GetResourcePolicy() const noexcept override {
        ResourcePolicy p{};
        if (model_ == FirefaceModel::kFF800) p.capture.reserveHostResources = false;
        else {
            p.playback.allowedIsoChannels = 0xff;
            p.capture.allowedIsoChannels = 0xff;
        }
        return p;
    }
    [[nodiscard]] StopPolicy GetStopPolicy() const noexcept override { return {.stopHostContextsBeforeDevice = true}; }
    /// The settings block read by the last Configure, for diagnostics.
    [[nodiscard]] const std::optional<FlashSettings>& LastFlashSettings() const noexcept { return lastDryRun_.flash; }
    [[nodiscard]] const SettingsDryRun& LastSettingsDryRun() const noexcept { return lastDryRun_; }
    [[nodiscard]] std::optional<uint32_t> PostEnableDelayMs() const noexcept override { return 5U; }
    void SetLinkSpeed(bool s800) noexcept { s800_ = s800; }

    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override {
        SyncGeneration();
        if (clock.sampleRateHz != 48000U) return std::unexpected(kIOReturnUnsupported);
        const IOReturn routeStatus = CheckRoute();
        if (routeStatus != kIOReturnSuccess) return std::unexpected(routeStatus);
        channels_ = channels;
        auto revision = ReadFirmwareRevision();
        if (!revision) return std::unexpected(revision.error());
        if (*revision == 0 || *revision < FirmwareMinimum(model_)) return std::unexpected(kIOReturnUnsupported);
        LogSettingsDryRun();
        auto status = ReadClockStatus();
        if (!status) return std::unexpected(status.error());
        if (IsExternalConfigured(status->configuredSource) && !status->externalLocked48k) {
            LogClockRefusal("Configure", *status);
            return std::unexpected(kIOReturnNotReady);
        }

        configured_ = true;
        caps_ = MakeCaps();
        return DuplexPrepareResult{.generation = io_.Generation(), .channels = channels_,
            .appliedClock = {.sampleRateHz = 48000}, .runtimeCaps = caps_};
    }

    [[nodiscard]] std::expected<AudioDuplexChannels, IOReturn> AssignChannels(
        const AudioDuplexChannels& requested) override {
        SyncGeneration();
        const IOReturn routeStatus = CheckRoute();
        if (routeStatus != kIOReturnSuccess) return std::unexpected(routeStatus);
        if (!configured_ || initialized_) return std::unexpected(kIOReturnNotReady);
        channels_ = requested;
        if (model_ == FirefaceModel::kFF400 &&
            (!IsValidAssignedChannel(model_, channels_.hostToDeviceIsoChannel) ||
             !IsValidAssignedChannel(model_, channels_.deviceToHostIsoChannel)))
            return std::unexpected(kIOReturnBadArgument);
        // The fetch mask is opened in Confirm(), after both host contexts run.
        const auto words = InitWords(model_, channels_.hostToDeviceIsoChannel, s800_);
        // Cancelled before init reaches the bus: there is no device state to reset.
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        initialized_ = true; // An uncertain init completion must be reset on rollback.
        if (const IOReturn kr = WriteWords(InitAddress(), words); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        auto afterInit = ReadClockStatus();
        if (!afterInit) return std::unexpected(afterInit.error());
        if (!ClockReady(*afterInit)) {
            LogClockRefusal("AssignChannels", *afterInit);
            return std::unexpected(kIOReturnNotReady);
        }
        if (model_ == FirefaceModel::kFF800) {
            // Device selects its transmit channel; the host binds it without an IRM claim.
            const uint64_t deadline = Session::UptimeMilliseconds() + 500U;
            while (Session::UptimeMilliseconds() < deadline) {
                if (Cancelled()) return std::unexpected(kIOReturnAborted);
                const uint64_t now = Session::UptimeMilliseconds();
                const uint32_t remaining = static_cast<uint32_t>(deadline - now);
                auto q = Read(Register::kStatus + 8, remaining);
                if (!q) return std::unexpected(q.error());
                if (*q != 0xffffffffU) {
                    const uint8_t channel = static_cast<uint8_t>(*q & 0x3fU);
                    if (!IsValidAssignedChannel(model_, channel)) return std::unexpected(kIOReturnBadArgument);
                    channels_.deviceToHostIsoChannel = channel;
                    channels_.captureIsoChannels[0] = channel;
                    caps_.deviceToHostIsoChannel = channel;
                    return channels_;
                }
                IOSleep(5);
            }
            return std::unexpected(kIOReturnTimeout);
        }
        if (requested.hostToDeviceIsoChannel > 7 || requested.deviceToHostIsoChannel > 7)
            return std::unexpected(kIOReturnBadArgument);
        return channels_;
    }

    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t) override {
        SyncGeneration();
        auto s = ReadClockStatus();
        if (!s) return std::unexpected(s.error());
        const bool healthy = ClockReady(*s);
        return DuplexHealthResult{.generation = io_.Generation(),
            .appliedClock = {.sampleRateHz = 48000}, .runtimeCaps = caps_,
            .sourceLocked = healthy, .clockReferenceHealthy = healthy,
            .nominalRateHz = s->configured48k ? 48000U : 0U,
            .status = s->q0, .extStatus = s->q1};
    }
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override {
        SyncGeneration();
        return Stage(kIOReturnSuccess, DuplexRestartPhase::kDeviceRxProgrammed);
    }
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override {
        SyncGeneration();
        if (!initialized_ || enabled_) return std::unexpected(kIOReturnNotReady);
        const std::array<uint32_t, 1> word{StartWord(model_, channels_.deviceToHostIsoChannel, s800_)};
        const IOReturn kr = WriteWords(StartAddress(), word);
        if (kr != kIOReturnSuccess) return std::unexpected(kr);
        enabled_ = true;
        return Stage(kIOReturnSuccess, DuplexRestartPhase::kDeviceTxArmed);
    }
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override {
        SyncGeneration();
        auto h = ReadHealth(0);
        if (!h || !h->sourceLocked) return std::unexpected(h ? kIOReturnNotReady : h.error());
        // Both host contexts run now. Let the device fetch PCM, as Linux does once
        // its domain is ready (ff-stream.c:206-217) and FFADO right after start
        // (fireface_hw.cpp:913). RME 3.41 instead writes this mask once before
        // init (hwStart 0x3f05); a user's FF800 played only with this order.
        if (const IOReturn kr = WriteFetchMask(true); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        return DuplexConfirmResult{.generation = io_.Generation(), .channels = channels_,
            .appliedClock = {.sampleRateHz = 48000}, .runtimeCaps = caps_, .status = h->status,
            .extStatus = h->extStatus};
    }
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override {
        SyncGeneration();
        if (clock.sampleRateHz != 48000U) return std::unexpected(kIOReturnUnsupported);
        auto s = ReadClockStatus();
        if (!s) return std::unexpected(s.error());
        if (!ClockReady(*s)) {
            LogClockRefusal("ApplyClockIdle", *s);
            return std::unexpected(kIOReturnNotReady);
        }
        return DuplexClockApplyResult{.generation = io_.Generation(),
            .appliedClock = {.sampleRateHz = 48000}, .runtimeCaps = caps_};
    }
    [[nodiscard]] IOReturn DisconnectPlayback() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn DisconnectCapture() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn BreakConnections() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn Stop() override {
        SyncGeneration();
        if (!initialized_) { configured_ = false; return kIOReturnSuccess; }
        const std::array<uint32_t, 3> ff800{};
        const std::array<uint32_t, 4> ff400{0, 0, 0, 1};
        const IOReturn kr = model_ == FirefaceModel::kFF800
            ? WriteWords(Register::kFF800Stop, ff800) : WriteWords(Register::kFF400Stop, ff400);
        if (kr != kIOReturnSuccess) return kr;
        enabled_ = false; initialized_ = false; configured_ = false;
        // Stop first, then mute every channel (Linux finish_session,
        // ff-stream.c:33-37; FFADO fireface_hw.cpp:949).
        return WriteFetchMask(false);
    }

private:
    void SyncGeneration() noexcept {
        const FW::Generation current = io_.Generation();
        if (current.value != stateGeneration_.value) {
            // A reset clears device streaming state. Never send stop traffic to
            // the old route; discard local state and re-fetch/init on the new one.
            configured_ = false;
            initialized_ = false;
            enabled_ = false;
            stateGeneration_ = current;
        }
    }
    [[nodiscard]] IOReturn CheckRoute() const noexcept {
        return io_.IsRouteCurrent() ? kIOReturnSuccess : kIOReturnOffline;
    }
    [[nodiscard]] bool Cancelled() const noexcept { return cancel_ && cancel_->load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t InitAddress() const noexcept { return model_ == FirefaceModel::kFF800 ? Register::kFF800Init : Register::kFF400Init; }
    [[nodiscard]] uint64_t StartAddress() const noexcept { return model_ == FirefaceModel::kFF800 ? Register::kFF800Start : Register::kFF400Start; }
    [[nodiscard]] bool IsInternalConfigured(uint32_t src) const noexcept {
        return (src & Register::kConfiguredInternalFlag) != 0U;
    }
    [[nodiscard]] bool IsExternalConfigured(uint32_t src) const noexcept { return !IsInternalConfigured(src); }
    [[nodiscard]] bool ClockReady(const ClockStatus& s) const noexcept {
        return s.configured48k &&
            (IsInternalConfigured(s.configuredSource) ? s.internalActive : s.externalLocked48k);
    }
    void LogClockRefusal(const char* stage, const ClockStatus& s) const noexcept {
        ASFW_LOG(Audio, "[RME] clock not ready at %{public}s: status0=0x%08x status1=0x%08x "
                 "configured=0x%04x active=0x%08x 48k=%d internalActive=%d externalLocked48k=%d",
                 stage, s.q0, s.q1, s.configuredSource, s.activeSource, s.configured48k ? 1 : 0,
                 s.internalActive ? 1 : 0, s.externalLocked48k ? 1 : 0);
    }
    // One quadlet per playback data channel: 0 fetches PCM, 1 mutes
    // (Linux former_switch_fetching_mode, ff-protocol-former.c:87-119).
    [[nodiscard]] IOReturn WriteFetchMask(bool fetch) {
        std::array<uint32_t, 28> mask{};
        mask.fill(fetch ? 0U : 1U);
        const uint32_t count = model_ == FirefaceModel::kFF800 ? 28U : 18U;
        return WriteWords(Register::kFetchMask, std::span<const uint32_t>(mask.data(), count));
    }
    [[nodiscard]] AudioStreamRuntimeCaps MakeCaps() const noexcept {
        const uint32_t count = model_ == FirefaceModel::kFF800 ? 28U : 18U;
        return AudioStreamRuntimeCaps{.hostInputPcmChannels = count, .hostOutputPcmChannels = count,
            .deviceToHostAm824Slots = count, .hostToDeviceAm824Slots = count,
            .sampleRateHz = 48000, .deviceToHostIsoChannel = channels_.deviceToHostIsoChannel,
            .hostToDeviceIsoChannel = channels_.hostToDeviceIsoChannel,
            .deviceToHostStreamCount = 1, .hostToDeviceStreamCount = 1};
    }
    [[nodiscard]] std::expected<uint32_t, IOReturn> Read(uint64_t address, uint32_t timeoutMs = 1500) {
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        const auto got = AwaitStage<uint32_t>([&](auto cb) {
            const auto handle = io_.ReadQuadLE(Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(address >> 32), .addressLo = static_cast<uint32_t>(address)}},
                [cb = std::move(cb)](Async::AsyncStatus s, uint32_t v) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s), v);
                });
            (void)handle;
        }, cancel_, timeoutMs, 1);
        if (!got) return std::unexpected(got.error());
        if (!io_.IsRouteCurrent()) return std::unexpected(kIOReturnOffline);
        return *got;
    }
    [[nodiscard]] IOReturn WriteWords(uint64_t address, std::span<const uint32_t> words) {
        if (Cancelled()) return kIOReturnAborted;
        const IOReturn status = AwaitStageStatus([&](auto cb) {
            const auto handle = io_.WriteBlockLEQuadlets(Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(address >> 32), .addressLo = static_cast<uint32_t>(address)}}, words,
                [cb = std::move(cb)](Async::AsyncStatus s) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s));
                });
            (void)handle;
        }, cancel_, 1500);
        if (status == kIOReturnSuccess && !io_.IsRouteCurrent()) return kIOReturnOffline;
        return status;
    }
    [[nodiscard]] std::expected<uint32_t, IOReturn> ReadFirmwareRevision() {
        if (model_ == FirefaceModel::kFF800) return Read(Register::kFF800Revision);
        if (IOReturn kr = RunFF400FlashCommand(FF400FlashCommand::kGetRevision); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        return Read(Register::kFF400Revision);
    }
    // Send a flash command and wait until the register reads 0 again: 25 polls
    // 2 ms apart, as RME 3.41 Wait (0x6c0a) and FFADO wait_while_busy do.
    [[nodiscard]] IOReturn RunFF400FlashCommand(FF400FlashCommand command) {
        if (IOReturn kr = WriteWords(Register::kFF400FlashCommand,
                std::array<uint32_t, 1>{static_cast<uint32_t>(command)}); kr != kIOReturnSuccess)
            return kr;
        const uint64_t deadline = Session::UptimeMilliseconds() + 50U;
        while (Session::UptimeMilliseconds() < deadline) {
            if (Cancelled()) return kIOReturnAborted;
            IOSleep(2);
            const uint64_t now = Session::UptimeMilliseconds();
            if (now >= deadline) break;
            auto busy = Read(Register::kFF400FlashStatus, static_cast<uint32_t>(deadline - now));
            if (!busy) return busy.error();
            if (*busy == 0) return kIOReturnSuccess;
        }
        return kIOReturnTimeout;
    }
    struct BlockWords { std::array<uint32_t, 64> words{}; };
    // Little-endian quadlets, like every Fireface register (FFADO
    // ByteSwapFromDevice32, rme_avdevice.cpp:1160-1174).
    [[nodiscard]] std::expected<BlockWords, IOReturn> ReadWords(uint64_t address, uint32_t count) {
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        if (count == 0 || count > BlockWords{}.words.size()) return std::unexpected(kIOReturnBadArgument);
        const auto got = AwaitStage<BlockWords>([&](auto cb) {
            const auto handle = io_.ReadBlock(Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(address >> 32), .addressLo = static_cast<uint32_t>(address)}},
                count * 4U,
                [cb = std::move(cb), count](Async::AsyncStatus s, std::span<const uint8_t> bytes) mutable {
                    BlockWords out{};
                    if (s == Async::AsyncStatus::kSuccess) {
                        for (uint32_t i = 0; i < count && (i * 4U + 3U) < bytes.size(); ++i) {
                            out.words[i] = static_cast<uint32_t>(bytes[i * 4U]) |
                                (static_cast<uint32_t>(bytes[i * 4U + 1U]) << 8U) |
                                (static_cast<uint32_t>(bytes[i * 4U + 2U]) << 16U) |
                                (static_cast<uint32_t>(bytes[i * 4U + 3U]) << 24U);
                        }
                    }
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s), out);
                });
            (void)handle;
        }, cancel_, 1500, 1);
        if (!got) return std::unexpected(got.error());
        if (!io_.IsRouteCurrent()) return std::unexpected(kIOReturnOffline);
        return *got;
    }
    [[nodiscard]] std::expected<FlashSettings, IOReturn> ReadFlashSettings() {
        FlashSettings settings{};
        if (model_ == FirefaceModel::kFF800) {
            auto block = ReadWords(Register::kFF800FlashSettings, kFlashSettingsQuadlets);
            if (!block) return std::unexpected(block.error());
            std::copy_n(block->words.begin(), kFlashSettingsQuadlets, settings.begin());
            return settings;
        }
        for (uint32_t first = 0; first < kFlashSettingsQuadlets; first += Register::kFF400FlashQuadletsPerRead) {
            const uint32_t count = std::min(Register::kFF400FlashQuadletsPerRead, kFlashSettingsQuadlets - first);
            const std::array<uint32_t, 2> range{Register::kFF400FlashSettings + first * 4U, count * 4U};
            if (IOReturn kr = WriteWords(Register::kFF400FlashBlock, range); kr != kIOReturnSuccess)
                return std::unexpected(kr);
            if (IOReturn kr = RunFF400FlashCommand(FF400FlashCommand::kRead); kr != kIOReturnSuccess)
                return std::unexpected(kr);
            auto block = ReadWords(Register::kFF400FlashReadBuffer, count);
            if (!block) return std::unexpected(block.error());
            std::copy_n(block->words.begin(), count, settings.begin() + first);
        }
        return settings;
    }
    // Dry run of the configuration upload: read what the card stored, compute
    // the 3 quadlets that would restore it, compare the fields the status
    // quadlet mirrors, and log it all. Writes nothing; a failure never stops a
    // start. Log lines start "[RME] settings dry-run".
    void LogSettingsDryRun() {
        const bool ff800 = model_ == FirefaceModel::kFF800;
        const char* model = ff800 ? "FF800" : "FF400";
        lastDryRun_ = {};
        auto flash = ReadFlashSettings();
        if (!flash) {
            ASFW_LOG(Audio, "[RME] settings dry-run %{public}s: flash read failed kr=0x%08x",
                     model, flash.error());
        } else {
            lastDryRun_.flash = *flash;
            const auto& q = *flash;
            for (uint32_t i = 0; i < kFlashSettingsQuadlets; i += 8) {
                uint32_t w[8]{};
                for (uint32_t j = 0; j < 8 && i + j < kFlashSettingsQuadlets; ++j) w[j] = q[i + j];
                ASFW_LOG(Audio, "[RME] settings dry-run %{public}s flash q%02u: %08x %08x %08x %08x %08x %08x %08x %08x",
                         model, i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        std::optional<uint32_t> status1;
        if (auto status = ReadWords(Register::kStatus, 4)) {
            const auto& w = status->words;
            status1 = w[1];
            ASFW_LOG(Audio, "[RME] settings dry-run %{public}s status: %08x %08x %08x %08x",
                     model, w[0], w[1], w[2], w[3]);
        }
        if (!lastDryRun_.flash) return;

        const auto settings = DecodeFlashSettings(model_, *lastDryRun_.flash);
        if (!settings) {
            lastDryRun_.decodeError = settings.error();
            ASFW_LOG(Audio, "[RME] settings dry-run %{public}s: flash q%02u = 0x%08x is unset or unknown; "
                     "no configuration computed", model, settings.error().quadlet, settings.error().value);
            return;
        }
        const auto& s = *settings;
        ASFW_LOG(Audio, "[RME] settings dry-run %{public}s: clock=%{public}s sync=%{public}s "
                 "spdif-in=%{public}s spdif-out=%{public}s in=%{public}s out=%{public}s "
                 "word-clock-1x=%d channels=%{public}s rate=%u",
                 model, s.clockMaster ? "master" : "autosync", Name(s.syncReference),
                 s.spdifInputOptical ? "optical" : "coaxial", s.spdifOutputOptical ? "optical" : "coaxial",
                 Name(s.inputLevel), Name(s.outputLevel), s.wordClockSingleSpeed ? 1 : 0,
                 Name(s.channelLimit), s.sampleRateHz);
        if (ff800) {
            ASFW_LOG(Audio, "[RME] settings dry-run FF800: phantom7-10=%d%d%d%d input1=%{public}s "
                     "input7=%{public}s input8=%{public}s speaker-emulation=%d drive=%d limiter=%d",
                     s.phantom[0], s.phantom[1], s.phantom[2], s.phantom[3], Name(s.ff800Input1),
                     Name(s.ff800Input7), Name(s.ff800Input8), s.ff800SpeakerEmulation ? 1 : 0,
                     s.ff800Drive ? 1 : 0, s.ff800Limiter ? 1 : 0);
        } else {
            ASFW_LOG(Audio, "[RME] settings dry-run FF400: phantom1-2=%d%d pad3-4=%d%d instrument3-4=%d%d "
                     "phones=%{public}s", s.phantom[0], s.phantom[1], s.ff400Pad[0], s.ff400Pad[1],
                     s.ff400Instrument[0], s.ff400Instrument[1], Name(s.ff400Phones));
        }
        const ConfigWords config = EncodeConfig(model_, s);
        lastDryRun_.config = config;
        ASFW_LOG(Audio, "[RME] settings dry-run %{public}s: would write 0x%llx <- %08x %08x %08x (not written)",
                 model, ff800 ? Register::kFF800Config : Register::kFF400Config, config[0], config[1], config[2]);
        if (status1) {
            const auto comparison = CompareWithStatus(model_, config, *status1);
            lastDryRun_.status = comparison;
            ASFW_LOG(Audio, "[RME] settings dry-run %{public}s: status mirror expected=%08x reported=%08x "
                     "mask=%08x -> %{public}s", model, comparison.expected, comparison.reported,
                     comparison.mask, comparison.Matches() ? "match" : "MISMATCH");
        }
    }

    [[nodiscard]] std::expected<ClockStatus, IOReturn> ReadClockStatus() {
        auto q0 = Read(Register::kStatus); if (!q0) return std::unexpected(q0.error());
        auto q1 = Read(Register::kStatus + 4); if (!q1) return std::unexpected(q1.error());
        ClockStatus s{.configuredSource = *q1 & Register::kConfiguredSourceMask,
                      .activeSource = *q0 & 0x01c00000U, .q0 = *q0, .q1 = *q1};
        s.configured48k = (*q1 & 0x1eU) == 0x06U;
        if (IsInternalConfigured(s.configuredSource)) {
            s.internalActive = s.activeSource == 0x01c00000U;
            return s;
        }
        uint32_t expectedActive = 0xffffffffU;
        if (s.configuredSource == 0x1000U) expectedActive = 0x01000000U;
        else if (s.configuredSource == 0x0c00U) expectedActive = 0x00c00000U;
        else if (s.configuredSource == 0U) expectedActive = 0U;
        else if (model_ == FirefaceModel::kFF400 && s.configuredSource == 0x1400U) {
            // FF400 LTC status is in its separate 0x801f0000 page; until that
            // page is decoded, it cannot pass the external-locked-48k gate.
            return s;
        }
        else if (model_ == FirefaceModel::kFF800 && s.configuredSource == 0x0400U) expectedActive = 0x00400000U;
        else if (model_ == FirefaceModel::kFF800 && s.configuredSource == 0x1800U) expectedActive = 0x01800000U;
        if (s.activeSource != expectedActive) return s;
        if (model_ == FirefaceModel::kFF400) {
            const uint32_t active = s.activeSource;
            bool locked = false, synced = false, rate48 = false;
            if (active == 0x01000000U) { locked = (*q0 & 0x20000000U); synced = (*q0 & 0x40000000U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
            else if (active == 0x00c00000U) { locked = (*q0 & 0x00040000U); synced = (*q0 & 0x00100000U); rate48 = (*q0 & 0x0003c000U) == 0x0000c000U; }
            else if (active == 0) { locked = (*q0 & 0x1000U); synced = (*q0 & 0x400U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
        s.externalLocked48k = locked && synced && rate48;
        } else {
            const uint32_t active = s.activeSource;
            bool locked = false, synced = false, rate48 = false;
            if (active == 0x01000000U) { locked = (*q0 & 0x20000000U); synced = (*q0 & 0x40000000U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
            else if (active == 0x00c00000U) { locked = (*q0 & 0x00040000U); synced = (*q0 & 0x00100000U); rate48 = (*q0 & 0x0003c000U) == 0x0000c000U; }
            else if (active == 0) { locked = (*q0 & 0x1000U); synced = (*q0 & 0x400U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
            else if (active == 0x00400000U) { locked = (*q0 & 0x2000U); synced = (*q0 & 0x800U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
            else if (active == 0x01800000U) { locked = (*q1 & 0x00400000U); synced = (*q1 & 0x00800000U); rate48 = (*q0 & 0x1e000000U) == 0x06000000U; }
            s.externalLocked48k = locked && synced && rate48;
        }
        return s;
    }
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> Stage(IOReturn status, DuplexRestartPhase phase) const {
        if (status != kIOReturnSuccess) return std::unexpected(status);
        return DuplexStageResult{.generation = io_.Generation(), .channels = channels_,
            .phase = phase, .runtimeCaps = caps_};
    }

    Protocols::Ports::ProtocolRegisterIO& io_;
    FirefaceModel model_;
    bool s800_{false};
    const std::atomic<bool>* cancel_{nullptr};
    bool initialized_{false};
    bool configured_{false};
    bool enabled_{false};
    AudioDuplexChannels channels_{};
    AudioStreamRuntimeCaps caps_{};
    FW::Generation stateGeneration_{0};
    SettingsDryRun lastDryRun_{};
};

} // namespace ASFW::Audio::RME
