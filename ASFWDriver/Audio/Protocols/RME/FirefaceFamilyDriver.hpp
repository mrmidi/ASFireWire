// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "../Duplex/FamilyDriver.hpp"
#include "../Duplex/FamilyStageWait.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../Session/SessionClock.hpp"
#include "../../../Logging/Logging.hpp"
#include <array>

namespace ASFW::Audio::RME {

enum class FirefaceModel : uint8_t { kFF400, kFF800 };

struct ClockStatus {
    uint32_t configuredSource{0};
    uint32_t activeSource{0};
    uint32_t q0{0};
    uint32_t q1{0};
    bool configured48k{false};
    bool internalActive{false};
    bool externalLocked48k{false};
};

// Register constants and clock maps are transcribed as behavior from FFADO
// fireface_def.h:84-90, fireface_flash.cpp:59-82 and ALSA former/ff400.rs:399-455,
// ff800.rs:113-174. All register values are little-endian quadlets.
namespace Register {
inline constexpr uint64_t kStatus = 0x801c0000ULL;
// Same address, write side: one quadlet per playback data channel. 0 lets the
// device fetch PCM from that channel, 1 mutes it (FFADO RME_FF_CHANNEL_MUTE_MASK,
// fireface_def.h:86; Linux FORMER_REG_FETCH_PCM_FRAMES, ff-protocol-former.c:12).
inline constexpr uint64_t kFetchMask = kStatus;
inline constexpr uint64_t kFF400Init = 0x80100500ULL;
inline constexpr uint64_t kFF400Start = 0x8010050cULL;
inline constexpr uint64_t kFF400Stop = 0x80100504ULL;
inline constexpr uint64_t kFF400FlashCommand = 0x80100520ULL;
inline constexpr uint64_t kFF400FlashStatus = 0x80100524ULL;
inline constexpr uint64_t kFF400Revision = 0x80100290ULL;
inline constexpr uint64_t kFF800Init = 0x00020000001cULL;
inline constexpr uint64_t kFF800Start = 0x000200000028ULL;
inline constexpr uint64_t kFF800Stop = 0x000200000034ULL;
inline constexpr uint64_t kFF800Revision = 0x000200000100ULL;
inline constexpr uint32_t kConfiguredSourceMask = 0x1c01;
// Bit 0 of the configured source wins over the saved external selection in
// bits 12:10; the device reports both together (Linux parse_clock_bits,
// ff-protocol-former.c:53-55).
inline constexpr uint32_t kConfiguredInternalFlag = 0x0001;
}

[[nodiscard]] constexpr uint32_t FirmwareMinimum(FirefaceModel model) noexcept {
    return model == FirefaceModel::kFF800 ? 0x24dU : 0x146U;
}

[[nodiscard]] constexpr std::array<uint32_t, 3> InitWords(
    FirefaceModel model, uint8_t playbackChannel, bool s800) noexcept {
    if (model == FirefaceModel::kFF800) {
        return {48000U, (28U << 11U) + playbackChannel, 28U | (s800 ? 0x800U : 0U)};
    }
    return {48000U, (18U << 11U) + playbackChannel, 18U};
}

[[nodiscard]] constexpr uint32_t StartWord(FirefaceModel model, uint8_t captureChannel,
                                            bool s800) noexcept {
    return model == FirefaceModel::kFF800
        ? 0x80000000U | 28U | (s800 ? 0x800U : 0U)
        : 0x80000000U | 18U | (static_cast<uint32_t>(captureChannel) << 5U);
}
[[nodiscard]] constexpr bool IsValidAssignedChannel(FirefaceModel model, uint8_t channel) noexcept {
    return channel < (model == FirefaceModel::kFF400 ? 8U : 64U);
}

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
        if (IOReturn kr = WriteWords(Register::kFF400FlashCommand, std::array<uint32_t, 1>{0xf});
            kr != kIOReturnSuccess) return std::unexpected(kr);
        const uint64_t deadline = Session::UptimeMilliseconds() + 50U;
        while (Session::UptimeMilliseconds() < deadline) {
            if (Cancelled()) return std::unexpected(kIOReturnAborted);
            IOSleep(2);
            const uint64_t now = Session::UptimeMilliseconds();
            if (now >= deadline) break;
            const uint32_t remaining = static_cast<uint32_t>(deadline - now);
            auto busy = Read(Register::kFF400FlashStatus, remaining);
            if (!busy) return std::unexpected(busy.error());
            if (*busy == 0) return Read(Register::kFF400Revision);
        }
        return std::unexpected(kIOReturnTimeout);
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
};

} // namespace ASFW::Audio::RME
