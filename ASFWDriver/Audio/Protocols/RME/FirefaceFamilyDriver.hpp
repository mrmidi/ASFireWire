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
#include "FirefaceClockStatus.hpp"
#include "FF800TransmitStart.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace ASFW::Audio::RME {

// What Configure read from the flash, what the flash encodes to, and what was
// last written to the configuration register.
struct SettingsDryRun {
    std::optional<FlashSettings> flash;
    std::optional<FlashDecodeError> decodeError;
    std::optional<FirefaceSettings> settings;
    std::optional<ConfigWords> config;      // the flash as saved
    std::optional<StatusComparison> status;
    std::optional<ConfigWords> written;     // the flash with the clock forced to master
};

class FirefaceFamilyDriver final : public FamilyDriver {
public:
    FirefaceFamilyDriver(Protocols::Ports::ProtocolRegisterIO& io, FirefaceModel model,
                         bool s800, Scheduling::ITimerScheduler* scheduler = nullptr) noexcept
        : io_(io), model_(model), s800_(s800), scheduler_(scheduler),
                         stateGeneration_(io.Generation()) {}

    ~FirefaceFamilyDriver() override {
        CancelPendingStart(true);
        if (startLock_) IOLockFree(startLock_);
    }

    void CancelPendingStart(bool close = false) noexcept {
        std::shared_ptr<FF800TransmitStart> operation;
        if (startLock_) {
            IOLockLock(startLock_);
            startsClosed_ = startsClosed_ || close;
            operation = pendingStart_;
            IOLockUnlock(startLock_);
        }
        if (operation) operation->Cancel();
    }

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
    // FF800's callback operation owns the 5 ms wait; do not sleep again in the session.
    [[nodiscard]] std::optional<uint32_t> PostEnableDelayMs() const noexcept override {
        return model_ == FirefaceModel::kFF800 ? 0U : 5U;
    }
    void SetLinkSpeed(bool s800) noexcept { s800_ = s800; }

    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override {
        SyncGeneration();
        if (clock.sampleRateHz != 48000U) return std::unexpected(kIOReturnUnsupported);
        if (model_ == FirefaceModel::kFF800 && (!scheduler_ || !startLock_))
            return std::unexpected(kIOReturnNotReady);
        const IOReturn routeStatus = CheckRoute();
        if (routeStatus != kIOReturnSuccess) return std::unexpected(routeStatus);
        channels_ = channels;
        auto revision = ReadFirmwareRevision();
        if (!revision) return std::unexpected(revision.error());
        if (*revision == 0 || *revision < FirmwareMinimum(model_)) return std::unexpected(kIOReturnUnsupported);
        ASFW_LOG(Audio, "[RME] firmware %{public}s: revision 0x%04x (minimum 0x%04x)",
                 model_ == FirefaceModel::kFF800 ? "FF800" : "FF400", *revision, FirmwareMinimum(model_));
        // Cache flash settings across start retries.
        if (!lastDryRun_.flash) LoadFlashSettings();
        if (const IOReturn kr = WriteConfig(); kr != kIOReturnSuccess) return std::unexpected(kr);
        auto status = ReadClockStatus();
        if (!status) return std::unexpected(status.error());
        LogClockStatus("configure", *status);
        if (lastDryRun_.written) {
            const auto mirror = CompareWithStatus(model_, *lastDryRun_.written, status->q1);
            ASFW_LOG(Audio, "[RME] config readback: status mirror expected=%08x reported=%08x -> %{public}s",
                     mirror.expected, mirror.reported, mirror.Matches() ? "match" : "MISMATCH");
        }
        if (IsExternalConfigured(status->configuredSource) && !status->externalLocked48k)
            return std::unexpected(kIOReturnNotReady);

        if (model_ == FirefaceModel::kFF800) {
            // RME 3.41 hwStart 0x3f05: fetching precedes host reservation/init.
            // Mark the uncertain write for rollback even if its completion fails.
            fetchOpened_ = true;
            const auto fetched = RunFF800Stage(FF800TransmitStart::Create(io_, *scheduler_, {}, cancel_,
                FF800TransmitStart::Phase::kFetch));
            if (!fetched) return std::unexpected(fetched.error());
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
        if (model_ == FirefaceModel::kFF800) {
            if (!scheduler_ || !startLock_) return std::unexpected(kIOReturnNotReady);
            if (Cancelled()) return std::unexpected(kIOReturnAborted);
            const auto words = InitWords(model_, channels_.hostToDeviceIsoChannel, s800_);
            initialized_ = true; // A partial asynchronous init still needs rollback.
            const auto result = RunFF800Stage(FF800TransmitStart::Create(io_, *scheduler_, words, cancel_));
            if (!result) return std::unexpected(result.error());
            channels_.deviceToHostIsoChannel = *result;
            channels_.captureIsoChannels[0] = *result;
            caps_.deviceToHostIsoChannel = *result;
            ASFW_LOG(Audio, "[RME] FF800 assigned (device not yet enabled): device transmits on channel %u, host transmits on channel %u",
                     *result, channels_.hostToDeviceIsoChannel);
        } else {
            if (const IOReturn kr = InitializeStreamFormat(); kr != kIOReturnSuccess)
                return std::unexpected(kr);
            if (const IOReturn kr = VerifyInitializedClock(); kr != kIOReturnSuccess)
                return std::unexpected(kr);
        }
        return channels_;
    }

    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override {
        SyncGeneration();
        auto s = ReadClockStatus(timeoutMs == 0 ? kRegisterTimeoutMs : timeoutMs);
        if (!s) return std::unexpected(s.error());
        const bool healthy = ClockReady(*s);
        if (!lastHealthy_ || *lastHealthy_ != healthy) {  // edges only: health is polled
            LogClockStatus(healthy ? "health-ok" : "health-lost", *s);
            lastHealthy_ = healthy;
        }
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
        if (model_ == FirefaceModel::kFF800) {
            const auto enabled = RunFF800Stage(FF800TransmitStart::Create(io_, *scheduler_,
                {word[0], 0, 0}, cancel_, FF800TransmitStart::Phase::kEnable));
            if (!enabled) return std::unexpected(enabled.error());
        } else if (const IOReturn kr = WriteWords(StartAddress(), word, "enable device transmit"); kr != kIOReturnSuccess) {
            return std::unexpected(kr);
        }
        enabled_ = true;
        return Stage(kIOReturnSuccess, DuplexRestartPhase::kDeviceTxArmed);
    }
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override {
        SyncGeneration();
        auto h = ReadHealth(0);
        if (!h || !h->sourceLocked) return std::unexpected(h ? kIOReturnNotReady : h.error());
        // FF800 fetching was opened before init, as RME 3.41 hwStart 0x3f05.
        // FF400 keeps its established post-start fetch order (FFADO/Linux).
        if (model_ == FirefaceModel::kFF400) {
            if (const IOReturn kr = WriteFetchMask(true); kr != kIOReturnSuccess)
                return std::unexpected(kr);
        }
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u streaming confirmed: host RX/TX running, "
                 "PCM fetching enabled, clock ready at 48000 Hz",
                 io_.Route().guid, io_.Generation().value);
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
            LogClockStatus("apply-clock", *s);
            return std::unexpected(kIOReturnNotReady);
        }
        return DuplexClockApplyResult{.generation = io_.Generation(),
            .appliedClock = {.sampleRateHz = 48000}, .runtimeCaps = caps_};
    }
    [[nodiscard]] IOReturn DisconnectPlayback() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn DisconnectCapture() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn BreakConnections() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn Stop() override {
        CancelPendingStart();
        SyncGeneration();
        if (!initialized_) {
            configured_ = false;
            if (!fetchOpened_) return kIOReturnSuccess;
            // Reservation/configuration failed after fetch opened but before init.
            const IOReturn kr = WriteFetchMask(false);
            if (kr == kIOReturnSuccess) fetchOpened_ = false;
            return kr;
        }
        const std::array<uint32_t, 3> ff800{};
        const std::array<uint32_t, 4> ff400{0, 0, 0, 1};
        const IOReturn kr = model_ == FirefaceModel::kFF800
            ? WriteWords(Register::kFF800Stop, ff800, "stop device stream") : WriteWords(Register::kFF400Stop, ff400, "stop device stream");
        if (kr != kIOReturnSuccess) { ASFW_LOG(Audio, "[RME] stop write failed kr=0x%08x", kr); return kr; }
        ASFW_LOG(Audio, "[RME] stopped");
        enabled_ = false; initialized_ = false; configured_ = false; lastHealthy_.reset();
        // Stop first, then mute every channel (Linux finish_session,
        // ff-stream.c:33-37; FFADO fireface_hw.cpp:949).
        const IOReturn muted = WriteFetchMask(false);
        if (muted == kIOReturnSuccess) fetchOpened_ = false;
        return muted;
    }

private:
    static constexpr uint32_t kRegisterTimeoutMs = 1500;

    [[nodiscard]] static uint32_t RemainingMs(uint64_t deadline) noexcept {
        const uint64_t now = Session::UptimeMilliseconds();
        return now < deadline ? static_cast<uint32_t>(deadline - now) : 0U;
    }

    // Family/session API remains synchronous on its own queue. All bus callbacks,
    // channel polls and device-settle timing run on the driver timer queue.
    [[nodiscard]] std::expected<uint8_t, IOReturn> RunFF800Stage(std::shared_ptr<FF800TransmitStart> operation) {
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        IOLockLock(startLock_);
        if (startsClosed_) {
            IOLockUnlock(startLock_);
            return std::unexpected(kIOReturnAborted);
        }
        pendingStart_ = operation;
        IOLockUnlock(startLock_);
        const auto result = AwaitStage<uint8_t>([operation](auto completion) {
            operation->Begin(std::move(completion));
        }, cancel_, kFamilyStageTimeoutMs, 1U);
        operation->Cancel();
        IOLockLock(startLock_);
        pendingStart_.reset();
        IOLockUnlock(startLock_);
        if (!result) return std::unexpected(result.error());
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        if (const auto kr = CheckRoute(); kr != kIOReturnSuccess) return std::unexpected(kr);
        return result;
    }

    void SyncGeneration() noexcept {
        const FW::Generation current = io_.Generation();
        if (current.value != stateGeneration_.value) {
            // A reset clears device streaming state. Never send stop traffic to
            // the old route; discard local state and re-fetch/init on the new one.
            configured_ = false;
            initialized_ = false;
            enabled_ = false;
            fetchOpened_ = false;
            stateGeneration_ = current;
        }
    }
    [[nodiscard]] IOReturn CheckRoute() const noexcept {
        return io_.IsRouteCurrent() ? kIOReturnSuccess : kIOReturnOffline;
    }
    [[nodiscard]] bool Cancelled() const noexcept { return cancel_ && cancel_->load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t InitAddress() const noexcept { return model_ == FirefaceModel::kFF800 ? Register::kFF800Init : Register::kFF400Init; }

    [[nodiscard]] IOReturn InitializeStreamFormat() {
        const auto words = InitWords(model_, channels_.hostToDeviceIsoChannel, s800_);
        if (Cancelled()) return kIOReturnAborted;
        initialized_ = true; // Rollback must stop even a partially completed init.
        if (const IOReturn kr = WriteWords(InitAddress(), words, "initialize stream (complete command)"); kr != kIOReturnSuccess) return kr;
        ASFW_LOG(Audio, "[RME] %{public}s init words: rate=%u format=0x%08x txsize=0x%08x hostChannel=%u",
                 model_ == FirefaceModel::kFF800 ? "FF800" : "FF400", words[0], words[1], words[2],
                 channels_.hostToDeviceIsoChannel);
        return kIOReturnSuccess;
    }

    [[nodiscard]] IOReturn VerifyInitializedClock() {
        const auto status = ReadClockStatus();
        if (!status) return status.error();
        LogClockStatus("after-init", *status);
        return ClockReady(*status) ? kIOReturnSuccess : kIOReturnNotReady;
    }

    [[nodiscard]] uint64_t ConfigAddress() const noexcept { return model_ == FirefaceModel::kFF800 ? Register::kFF800Config : Register::kFF400Config; }
    [[nodiscard]] uint64_t StartAddress() const noexcept { return model_ == FirefaceModel::kFF800 ? Register::kFF800Start : Register::kFF400Start; }
    [[nodiscard]] bool IsInternalConfigured(uint32_t src) const noexcept {
        return (src & Register::kConfiguredInternalFlag) != 0U;
    }
    [[nodiscard]] bool IsExternalConfigured(uint32_t src) const noexcept { return !IsInternalConfigured(src); }
    // A master needs nothing from status0: its sync-reference field only names
    // what a slave follows. Linux parse_clock_bits (ff-protocol-former.c:54) and
    // FFADO SR1_CLOCK_MODE_MASTER test status1 bit 0 alone; RME 3.41 hwGetRate
    // (0x7c05) never reads 0x01c00000 as internal either.
    [[nodiscard]] bool ClockReady(const ClockStatus& s) const noexcept {
        return IsClockReady(s);
    }
    // One cold-path line per start stage, decoded for remote reports; the raw
    // words stay in the line so a wrong label cannot hide anything.
    void LogClockStatus(const char* stage, const ClockStatus& s) const noexcept {
        const DecodedStatus d = DecodeStatus(s.q0, s.q1);
        ASFW_LOG(Audio, "[RME] clock %{public}s: ready=%d mode=%{public}s rate=%u "
                 "syncRef=%{public}s syncRate=%u wclk=%{public}s spdif=%{public}s "
                 "adat1=%{public}s adat2=%{public}s status0=0x%08x status1=0x%08x",
                 stage, ClockReady(s) ? 1 : 0, Name(d.configured), d.configuredRateHz,
                 Name(d.syncReference), d.syncRateHz, Name(d.wordClock), Name(d.spdif),
                 Name(d.adat1), Name(d.adat2), s.q0, s.q1);
    }
    // One quadlet per playback data channel: 0 fetches PCM, 1 mutes
    // (Linux former_switch_fetching_mode, ff-protocol-former.c:87-119).
    [[nodiscard]] IOReturn WriteFetchMask(bool fetch) {
        std::array<uint32_t, 28> mask{};
        mask.fill(fetch ? 0U : 1U);
        const uint32_t count = model_ == FirefaceModel::kFF800 ? 28U : 18U;
        return WriteWords(Register::kFetchMask, std::span<const uint32_t>(mask.data(), count),
                          fetch ? "enable PCM fetching" : "mute PCM fetching");
    }
    [[nodiscard]] AudioStreamRuntimeCaps MakeCaps() const noexcept {
        const uint32_t count = model_ == FirefaceModel::kFF800 ? 28U : 18U;
        return AudioStreamRuntimeCaps{.hostInputPcmChannels = count, .hostOutputPcmChannels = count,
            .deviceToHostAm824Slots = count, .hostToDeviceAm824Slots = count,
            .sampleRateHz = 48000, .deviceToHostIsoChannel = channels_.deviceToHostIsoChannel,
            .hostToDeviceIsoChannel = channels_.hostToDeviceIsoChannel,
            .deviceToHostStreamCount = 1, .hostToDeviceStreamCount = 1};
    }
    template <typename T, typename Start>
    [[nodiscard]] std::expected<T, IOReturn> AwaitRegister(Start&& start, uint32_t timeoutMs) {
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        if (timeoutMs == 0) return std::unexpected(kIOReturnTimeout);
        const uint64_t deadline = Session::UptimeMilliseconds() + timeoutMs;
        const auto got = WaitForAsyncResult<T>(std::forward<Start>(start), timeoutMs,
            kIOReturnTimeout, [this, deadline] {
                return Cancelled() || RemainingMs(deadline) == 0;
            }, 1);
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        if (RemainingMs(deadline) == 0) return std::unexpected(kIOReturnTimeout);
        if (got.status != kIOReturnSuccess) return std::unexpected(got.status);
        if (!io_.IsRouteCurrent()) return std::unexpected(kIOReturnOffline);
        return got.value;
    }
    [[nodiscard]] std::expected<uint32_t, IOReturn> Read(uint64_t address, uint32_t timeoutMs = kRegisterTimeoutMs) {
        return AwaitRegister<uint32_t>([&](auto cb) {
            const auto handle = io_.ReadQuadLE(Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(address >> 32), .addressLo = static_cast<uint32_t>(address)}},
                [cb = std::move(cb)](Async::AsyncStatus s, uint32_t v) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s), v);
                });
            (void)handle;
        }, timeoutMs);
    }
    [[nodiscard]] IOReturn WriteWords(uint64_t address, std::span<const uint32_t> words,
                                     const char* operation = "write register") {
        if (Cancelled()) return kIOReturnAborted;
        TraceRmeBytes(io_, operation, address, words);
        const uint64_t began = Session::UptimeMilliseconds();
        const IOReturn status = AwaitStageStatus([&](auto cb) {
            const auto handle = io_.WriteBlockLEQuadlets(Async::FWAddress{Async::FWAddress::AddressParts{
                .addressHi = static_cast<uint16_t>(address >> 32), .addressLo = static_cast<uint32_t>(address)}}, words,
                [cb = std::move(cb)](Async::AsyncStatus s) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s));
                });
            (void)handle;
        }, cancel_, kRegisterTimeoutMs);
        const IOReturn result = status == kIOReturnSuccess && !io_.IsRouteCurrent() ? kIOReturnOffline : status;
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=%{public}s write complete kr=0x%08x (%{public}s) elapsedMs=%llu",
                 io_.Route().guid, io_.Generation().value, operation, result,
                 Logging::IOReturnName(result), Session::UptimeMilliseconds() - began);
        return result;
    }
    [[nodiscard]] std::expected<uint32_t, IOReturn> ReadFirmwareRevision() {
        if (model_ == FirefaceModel::kFF800) return Read(Register::kFF800Revision);
        if (IOReturn kr = RunFF400FlashCommand(FF400FlashCommand::kGetRevision); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        return Read(Register::kFF400Revision);
    }
    // Send a flash command and wait until the register reads 0 again. The wait
    // counts polls, like RME 3.41 Wait (0x6c0a) and FFADO wait_while_busy: a
    // slow bus or an oversleeping IOSleep makes it last longer, never give up
    // early before the last read.
    [[nodiscard]] IOReturn RunFF400FlashCommand(FF400FlashCommand command) {
        if (IOReturn kr = WriteWords(Register::kFF400FlashCommand,
                std::array<uint32_t, 1>{static_cast<uint32_t>(command)}); kr != kIOReturnSuccess)
            return kr;
        for (uint32_t poll = 0; poll < Register::kFF400FlashPolls; ++poll) {
            if (Cancelled()) return kIOReturnAborted;
            IOSleep(Register::kFF400FlashPollIntervalMs);
            auto busy = Read(Register::kFF400FlashStatus);
            if (!busy) return busy.error();
            if (*busy == 0) return kIOReturnSuccess;
        }
        return kIOReturnTimeout;
    }
    struct BlockWords { std::array<uint32_t, 64> words{}; };
    // Little-endian quadlets, like every Fireface register (FFADO
    // ByteSwapFromDevice32, rme_avdevice.cpp:1160-1174).
    [[nodiscard]] std::expected<BlockWords, IOReturn> ReadWords(
        uint64_t address, uint32_t count, uint32_t timeoutMs = kRegisterTimeoutMs) {
        if (count == 0 || count > BlockWords{}.words.size()) return std::unexpected(kIOReturnBadArgument);
        return AwaitRegister<BlockWords>([&](auto cb) {
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
        }, timeoutMs);
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
    // start. Log lines start "[RME] settings".
    void LoadFlashSettings() {
        const bool ff800 = model_ == FirefaceModel::kFF800;
        const char* model = ff800 ? "FF800" : "FF400";
        lastDryRun_ = {};
        auto flash = ReadFlashSettings();
        if (!flash) {
            ASFW_LOG(Audio, "[RME] settings %{public}s: flash read failed kr=0x%08x",
                     model, flash.error());
        } else {
            lastDryRun_.flash = *flash;
            const auto& q = *flash;
            for (uint32_t i = 0; i < kFlashSettingsQuadlets; i += 8) {
                uint32_t w[8]{};
                for (uint32_t j = 0; j < 8 && i + j < kFlashSettingsQuadlets; ++j) w[j] = q[i + j];
                ASFW_LOG(Audio, "[RME] settings %{public}s flash q%02u: %08x %08x %08x %08x %08x %08x %08x %08x",
                         model, i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        std::optional<uint32_t> status1;
        if (auto status = ReadWords(Register::kStatus, 4)) {
            const auto& w = status->words;
            status1 = w[1];
            ASFW_LOG(Audio, "[RME] settings %{public}s status: %08x %08x %08x %08x",
                     model, w[0], w[1], w[2], w[3]);
        }
        if (!lastDryRun_.flash) return;

        const auto settings = DecodeFlashSettings(model_, *lastDryRun_.flash);
        if (!settings) {
            lastDryRun_.decodeError = settings.error();
            ASFW_LOG(Audio, "[RME] settings %{public}s: flash q%02u = 0x%08x is unset or unknown; "
                     "no configuration computed", model, settings.error().quadlet, settings.error().value);
            return;
        }
        const auto& s = *settings;
        lastDryRun_.settings = s;
        ASFW_LOG(Audio, "[RME] settings %{public}s: clock=%{public}s sync=%{public}s "
                 "spdif-in=%{public}s spdif-out=%{public}s in=%{public}s out=%{public}s "
                 "word-clock-1x=%d channels=%{public}s rate=%u",
                 model, s.clockMaster ? "master" : "autosync", Name(s.syncReference),
                 s.spdifInputOptical ? "optical" : "coaxial", s.spdifOutputOptical ? "optical" : "coaxial",
                 Name(s.inputLevel), Name(s.outputLevel), s.wordClockSingleSpeed ? 1 : 0,
                 Name(s.channelLimit), s.sampleRateHz);
        if (ff800) {
            ASFW_LOG(Audio, "[RME] settings FF800: phantom7-10=%d%d%d%d input1=%{public}s "
                     "input7=%{public}s input8=%{public}s speaker-emulation=%d drive=%d limiter=%d",
                     s.phantom[0], s.phantom[1], s.phantom[2], s.phantom[3], Name(s.ff800Input1),
                     Name(s.ff800Input7), Name(s.ff800Input8), s.ff800SpeakerEmulation ? 1 : 0,
                     s.ff800Drive ? 1 : 0, s.ff800Limiter ? 1 : 0);
        } else {
            ASFW_LOG(Audio, "[RME] settings FF400: phantom1-2=%d%d pad3-4=%d%d instrument3-4=%d%d "
                     "phones=%{public}s", s.phantom[0], s.phantom[1], s.ff400Pad[0], s.ff400Pad[1],
                     s.ff400Instrument[0], s.ff400Instrument[1], Name(s.ff400Phones));
        }
        const ConfigWords config = EncodeConfig(model_, s);
        lastDryRun_.config = config;
        ASFW_LOG(Audio, "[RME] settings %{public}s: flash encodes %08x %08x %08x",
                 model, config[0], config[1], config[2]);
        if (status1) {
            const auto comparison = CompareWithStatus(model_, config, *status1);
            lastDryRun_.status = comparison;
            ASFW_LOG(Audio, "[RME] settings %{public}s: status mirror expected=%08x reported=%08x "
                     "mask=%08x -> %{public}s", model, comparison.expected, comparison.reported,
                     comparison.mask, comparison.Matches() ? "match" : "MISMATCH");
        }
    }

    // Uploads the flash settings with the clock forced to master, before init.
    // FFADO init_hardware writes the flash-derived configuration before any
    // streaming (fireface_hw.cpp:83-105); RME 3.41 Fireface_InitHardware writes
    // it too. Forcing master is ASFW policy for now, not reference behaviour.
    // Written on every start: idempotent, and it survives a device power cycle.
    // Best effort: without decodable flash settings nothing is written, and a
    // failed write never blocks the start; teardown and a stale route still do.
    [[nodiscard]] IOReturn WriteConfig() {
        const char* model = model_ == FirefaceModel::kFF800 ? "FF800" : "FF400";
        lastDryRun_.written.reset();
        if (!lastDryRun_.settings) {
            ASFW_LOG(Audio, "[RME] config %{public}s: not written, no decodable flash settings", model);
            return kIOReturnSuccess;
        }
        FirefaceSettings s = *lastDryRun_.settings;
        const bool forced = !s.clockMaster;
        s.clockMaster = true;
        const ConfigWords words = EncodeConfig(model_, s);
        const IOReturn kr = WriteWords(ConfigAddress(), words, "configure device clock and routing");
        ASFW_LOG(Audio, "[RME] config %{public}s: wrote 0x%llx <- %08x %08x %08x clock=master%{public}s kr=0x%08x",
                 model, ConfigAddress(), words[0], words[1], words[2],
                 forced ? " (forced; flash says autosync)" : "", kr);
        if (kr == kIOReturnSuccess) lastDryRun_.written = words;
        if (kr == kIOReturnAborted || kr == kIOReturnOffline) return kr;
        return kIOReturnSuccess;
    }

    [[nodiscard]] std::expected<ClockStatus, IOReturn> ReadClockStatus(uint32_t timeoutMs = kRegisterTimeoutMs) {
        const uint64_t deadline = Session::UptimeMilliseconds() + timeoutMs;
        auto q0 = Read(Register::kStatus, RemainingMs(deadline));
        if (!q0) return std::unexpected(q0.error());
        auto q1 = Read(Register::kStatus + 4, RemainingMs(deadline));
        if (!q1) return std::unexpected(q1.error());
        return DecodeClockStatus(model_, *q0, *q1);
    }
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> Stage(IOReturn status, DuplexRestartPhase phase) const {
        if (status != kIOReturnSuccess) return std::unexpected(status);
        return DuplexStageResult{.generation = io_.Generation(), .channels = channels_,
            .phase = phase, .runtimeCaps = caps_};
    }

    Protocols::Ports::ProtocolRegisterIO& io_;
    FirefaceModel model_;
    bool s800_{false};
    Scheduling::ITimerScheduler* scheduler_{nullptr};
    IOLock* startLock_{IOLockAlloc()};
    std::shared_ptr<FF800TransmitStart> pendingStart_;
    bool startsClosed_{false}; // Protected by startLock_; protocol shutdown is terminal.
    const std::atomic<bool>* cancel_{nullptr};
    bool initialized_{false};
    bool fetchOpened_{false};
    bool configured_{false};
    bool enabled_{false};
    AudioDuplexChannels channels_{};
    AudioStreamRuntimeCaps caps_{};
    FW::Generation stateGeneration_{0};
    SettingsDryRun lastDryRun_{};
    std::optional<bool> lastHealthy_;  // last logged health, for edge-only logging
};

} // namespace ASFW::Audio::RME
