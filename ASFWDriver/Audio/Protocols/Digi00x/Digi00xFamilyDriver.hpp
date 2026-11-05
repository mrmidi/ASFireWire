// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xFamilyDriver.hpp - FamilyDriver stages for the Digidesign Digi 002 /
// 003 register protocol (issue #249).
//
// Session order, cross-validated with Linux sound/firewire/digi00x
// (digi00x-stream.c reserve_duplex :288-333, start_duplex :335-400,
// finish_session :125-143, begin_session :145-186) as a behavioral reference:
//   Configure            finish any old session, program the local rate
//   AssignChannels       write (capture << 16 | playback) to the channel register
//   ArmDeviceRx          nothing: the device receives once channels are set
//   ArmDeviceTxAndEnable the streaming-state countdown
//   (host transmit starts, then host receive: StreamStartShape::TransmitFirst.
//    The device sends nothing until it has received packets.)
//   Confirm              the clock is healthy at the requested rate
//   Stop                 stop write, release channels, settle
//
// ASFW streams 1x rates only (AudioClockConfig.hpp), so this family offers
// 44.1 and 48 kHz: 18 PCM channels plus the MIDI data channel each way.

#pragma once

#include "../Duplex/FamilyDriver.hpp"
#include "../Duplex/FamilyStageWait.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../Session/SessionClock.hpp"
#include "../../../Logging/Logging.hpp"
#include "Digi00xRegisters.hpp"

#include <DriverKit/IOLib.h>

#include <array>
#include <atomic>
#include <optional>
#include <utility>

namespace ASFW::Audio::Digi00x {

/// The rates this family streams today (1x only).
inline constexpr std::array<uint32_t, 2> kStreamedRatesHz{44100, 48000};

[[nodiscard]] constexpr bool IsStreamedRate(uint32_t hz) noexcept {
    return hz == kStreamedRatesHz[0] || hz == kStreamedRatesHz[1];
}

class Digi00xFamilyDriver final : public FamilyDriver {
public:
    explicit Digi00xFamilyDriver(Protocols::Ports::ProtocolRegisterIO& io) noexcept
        : io_(io), stateGeneration_(io.Generation()) {}

    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override { cancel_ = cancel; }

    [[nodiscard]] IOReturn LoadGeometry() override {
        caps_ = MakeCaps(rateHz_ != 0 ? rateHz_ : kStreamedRatesHz[1]);
        return kIOReturnSuccess;
    }
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override { return caps_; }

    // Both channels come from the IRM and are written to the device: the
    // default policy (reserve both directions, any channel) is the right one.
    [[nodiscard]] ResourcePolicy GetResourcePolicy() const noexcept override { return {}; }
    // Linux stops the host streams before finishing the session.
    [[nodiscard]] StopPolicy GetStopPolicy() const noexcept override {
        return {.stopHostContextsBeforeDevice = true};
    }

    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override {
        SyncGeneration();
        if (!IsStreamedRate(clock.sampleRateHz)) return std::unexpected(kIOReturnUnsupported);
        if (const IOReturn kr = CheckRoute(); kr != kIOReturnSuccess) return std::unexpected(kr);
        channels_ = channels;
        // A previous host may have left the device streaming; Linux finishes
        // the session before every new reservation.
        if (const IOReturn kr = FinishSession("configure"); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        if (const IOReturn kr = ProgramRate(clock.sampleRateHz); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        configured_ = true;
        caps_ = MakeCaps(rateHz_);
        return DuplexPrepareResult{.generation = io_.Generation(), .channels = channels_,
            .appliedClock = {.sampleRateHz = rateHz_}, .runtimeCaps = caps_};
    }

    [[nodiscard]] std::expected<AudioDuplexChannels, IOReturn> AssignChannels(
        const AudioDuplexChannels& requested) override {
        SyncGeneration();
        if (const IOReturn kr = CheckRoute(); kr != kIOReturnSuccess) return std::unexpected(kr);
        if (!configured_ || channelsWritten_) return std::unexpected(kIOReturnNotReady);
        channels_ = requested;
        const uint32_t word = ChannelWord(channels_.deviceToHostIsoChannel,
                                          channels_.hostToDeviceIsoChannel);
        channelsWritten_ = true;  // A failed write still needs the release on Stop.
        if (const IOReturn kr = Write(Register::kIsochChannels, word, "assign isoch channels");
            kr != kIOReturnSuccess)
            return std::unexpected(kr);
        caps_ = MakeCaps(rateHz_);
        ASFW_LOG(Audio, "[Digi00x] channels: device transmits on %u, host transmits on %u",
                 channels_.deviceToHostIsoChannel, channels_.hostToDeviceIsoChannel);
        return channels_;
    }

    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override {
        SyncGeneration();
        const uint32_t budget = timeoutMs == 0 ? kRegisterTimeoutMs : timeoutMs;
        auto health = ReadClockHealth(budget);
        if (!health) return std::unexpected(health.error());
        if (!lastHealthy_ || *lastHealthy_ != health->locked) {  // edges only: health is polled
            ASFW_LOG(Audio, "[Digi00x] clock %{public}s: source=%u local=%u external=%u detected=%d",
                     health->locked ? "ok" : "lost", health->sourceRaw, health->localRateHz,
                     health->externalRateHz, health->externalDetected ? 1 : 0);
            lastHealthy_ = health->locked;
        }
        return DuplexHealthResult{.generation = io_.Generation(),
            .appliedClock = {.sampleRateHz = rateHz_}, .runtimeCaps = caps_,
            .sourceLocked = health->locked, .clockReferenceHealthy = health->locked,
            .nominalRateHz = health->localRateHz, .status = health->sourceRaw,
            .extStatus = health->externalRateRaw};
    }

    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override {
        SyncGeneration();
        if (!channelsWritten_) return std::unexpected(kIOReturnNotReady);
        return Stage(DuplexRestartPhase::kDeviceRxProgrammed);
    }

    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override {
        SyncGeneration();
        if (!channelsWritten_ || enabled_) return std::unexpected(kIOReturnNotReady);
        auto state = Read(Register::kStreamingState);
        if (!state) return std::unexpected(state.error());
        const auto steps = StartStepsFor(*state);
        if (!steps) {
            ASFW_LOG(Audio, "[Digi00x] start refused: unexpected streaming state %u", *state);
            return std::unexpected(kIOReturnError);
        }
        enabled_ = true;  // Any step written needs the stop on rollback.
        for (const uint32_t value : steps->Values()) {
            if (const IOReturn kr = Write(Register::kStreamingSet, value, "start step");
                kr != kIOReturnSuccess)
                return std::unexpected(kr);
            if (const IOReturn kr = Sleep(kStartStepDelayMs); kr != kIOReturnSuccess)
                return std::unexpected(kr);
        }
        ASFW_LOG(Audio, "[Digi00x] streaming enabled from state %u (%u steps)", *state,
                 static_cast<uint32_t>(steps->Values().size()));
        return Stage(DuplexRestartPhase::kDeviceTxArmed);
    }

    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override {
        SyncGeneration();
        auto h = ReadHealth(0);
        if (!h) return std::unexpected(h.error());
        if (!h->sourceLocked) return std::unexpected(kIOReturnNotReady);
        ASFW_LOG(Audio, "[Digi00x] guid=%016llx gen=%u streaming confirmed at %u Hz",
                 io_.Route().guid, io_.Generation().value, rateHz_);
        return DuplexConfirmResult{.generation = io_.Generation(), .channels = channels_,
            .appliedClock = {.sampleRateHz = rateHz_}, .runtimeCaps = caps_,
            .status = h->status, .extStatus = h->extStatus};
    }

    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override {
        SyncGeneration();
        if (!IsStreamedRate(clock.sampleRateHz)) return std::unexpected(kIOReturnUnsupported);
        if (enabled_) return std::unexpected(kIOReturnBusy);
        if (const IOReturn kr = ProgramRate(clock.sampleRateHz); kr != kIOReturnSuccess)
            return std::unexpected(kr);
        caps_ = MakeCaps(rateHz_);
        return DuplexClockApplyResult{.generation = io_.Generation(),
            .appliedClock = {.sampleRateHz = rateHz_}, .runtimeCaps = caps_};
    }

    // The device has no per-direction connections to break.
    [[nodiscard]] IOReturn DisconnectPlayback() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn DisconnectCapture() override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn BreakConnections() override { return kIOReturnSuccess; }

    [[nodiscard]] IOReturn Stop() override {
        SyncGeneration();
        if (!configured_ && !channelsWritten_ && !enabled_) return kIOReturnSuccess;
        const IOReturn kr = FinishSession("stop");
        if (kr != kIOReturnSuccess) return kr;
        configured_ = false;
        lastHealthy_.reset();
        return kIOReturnSuccess;
    }

private:
    static constexpr uint32_t kRegisterTimeoutMs = 1500;

    struct ClockHealth {
        bool locked{false};
        uint32_t sourceRaw{0};
        uint32_t localRateHz{0};
        uint32_t externalRateRaw{0};
        uint32_t externalRateHz{0};
        bool externalDetected{false};
    };

    [[nodiscard]] static uint32_t RemainingMs(uint64_t deadline) noexcept {
        const uint64_t now = Session::UptimeMilliseconds();
        return now < deadline ? static_cast<uint32_t>(deadline - now) : 0U;
    }

    [[nodiscard]] static AudioStreamRuntimeCaps CapsFor(uint32_t hz, uint8_t capture,
                                                        uint8_t playback) noexcept {
        const uint32_t pcm = PcmChannelsForRate(hz).value_or(0);
        const uint32_t slots = DataBlockQuadletsForRate(hz).value_or(0);
        return AudioStreamRuntimeCaps{.hostInputPcmChannels = pcm, .hostOutputPcmChannels = pcm,
            .deviceToHostAm824Slots = slots, .hostToDeviceAm824Slots = slots,
            .sampleRateHz = hz, .deviceToHostIsoChannel = capture,
            .hostToDeviceIsoChannel = playback,
            .deviceToHostStreamCount = 1, .hostToDeviceStreamCount = 1};
    }
    [[nodiscard]] AudioStreamRuntimeCaps MakeCaps(uint32_t hz) const noexcept {
        return CapsFor(hz, channels_.deviceToHostIsoChannel, channels_.hostToDeviceIsoChannel);
    }

    void SyncGeneration() noexcept {
        const FW::Generation current = io_.Generation();
        if (current.value != stateGeneration_.value) {
            // A bus reset ends the device's session. Never send stop traffic to
            // the old route; start again from Configure on the new one.
            configured_ = false;
            channelsWritten_ = false;
            enabled_ = false;
            stateGeneration_ = current;
        }
    }
    [[nodiscard]] IOReturn CheckRoute() const noexcept {
        return io_.IsRouteCurrent() ? kIOReturnSuccess : kIOReturnOffline;
    }
    [[nodiscard]] bool Cancelled() const noexcept {
        return cancel_ && cancel_->load(std::memory_order_acquire);
    }
    [[nodiscard]] IOReturn Sleep(uint32_t ms) const {
        if (Cancelled()) return kIOReturnAborted;
        IOSleep(ms);
        return Cancelled() ? kIOReturnAborted : kIOReturnSuccess;
    }

    // Stop streaming and release both channels, then let the device settle
    // (digi00x-stream.c:125-143). Both writes are always attempted.
    [[nodiscard]] IOReturn FinishSession(const char* why) {
        const IOReturn stopped = Write(Register::kStreamingSet, kStreamingStop, "stop streaming");
        const IOReturn released = Write(Register::kIsochChannels, kReleaseChannels, "release channels");
        enabled_ = false;
        channelsWritten_ = false;
        if (stopped != kIOReturnSuccess || released != kIOReturnSuccess) {
            ASFW_LOG(Audio, "[Digi00x] finish session (%{public}s) failed: stop=0x%08x release=0x%08x",
                     why, stopped, released);
            return stopped != kIOReturnSuccess ? stopped : released;
        }
        return Sleep(kStopSettleMs);
    }

    [[nodiscard]] IOReturn ProgramRate(uint32_t hz) {
        const auto code = RateCodeForHz(hz);
        if (!code) return kIOReturnUnsupported;
        auto current = Read(Register::kLocalRate);
        if (!current) return current.error();
        if (DecodeRateHz(*current) != hz) {
            if (const IOReturn kr = Write(Register::kLocalRate, static_cast<uint32_t>(*code), "set rate");
                kr != kIOReturnSuccess)
                return kr;
            ASFW_LOG(Audio, "[Digi00x] local rate %u -> %u Hz", DecodeRateHz(*current).value_or(0), hz);
        }
        rateHz_ = hz;
        return kIOReturnSuccess;
    }

    // Locked means the device runs at the requested rate: on its internal
    // clock that is the local rate; on an external source the external rate
    // must also decode (an out-of-range code is desync) and match.
    [[nodiscard]] std::expected<ClockHealth, IOReturn> ReadClockHealth(uint32_t timeoutMs) {
        const uint64_t deadline = Session::UptimeMilliseconds() + timeoutMs;
        auto source = Read(Register::kClockSource, RemainingMs(deadline));
        if (!source) return std::unexpected(source.error());
        auto local = Read(Register::kLocalRate, RemainingMs(deadline));
        if (!local) return std::unexpected(local.error());
        ClockHealth h{.sourceRaw = *source, .localRateHz = DecodeRateHz(*local).value_or(0)};
        const auto clock = DecodeClockSource(*source);
        if (!clock) return h;
        if (*clock == ClockSource::kInternal) {
            h.locked = h.localRateHz == rateHz_ && rateHz_ != 0;
            return h;
        }
        auto detected = Read(Register::kDetectExternal, RemainingMs(deadline));
        if (!detected) return std::unexpected(detected.error());
        auto external = Read(Register::kExternalRate, RemainingMs(deadline));
        if (!external) return std::unexpected(external.error());
        h.externalDetected = DecodeExternalDetected(*detected);
        h.externalRateRaw = *external;
        h.externalRateHz = DecodeRateHz(*external).value_or(0);
        h.locked = h.externalDetected && h.externalRateHz == rateHz_ && rateHz_ != 0;
        return h;
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
        if (got.status != kIOReturnSuccess) return std::unexpected(got.status);
        if (!io_.IsRouteCurrent()) return std::unexpected(kIOReturnOffline);
        return got.value;
    }

    [[nodiscard]] static Async::FWAddress Address(uint64_t address) noexcept {
        return Async::FWAddress{Async::FWAddress::AddressParts{
            .addressHi = static_cast<uint16_t>(address >> 32U),
            .addressLo = static_cast<uint32_t>(address)}};
    }

    [[nodiscard]] std::expected<uint32_t, IOReturn> Read(uint64_t address,
                                                         uint32_t timeoutMs = kRegisterTimeoutMs) {
        return AwaitRegister<uint32_t>([&](auto cb) {
            const auto handle = io_.ReadQuadBE(Address(address),
                [cb = std::move(cb)](Async::AsyncStatus s, uint32_t v) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s), v);
                });
            (void)handle;
        }, timeoutMs);
    }

    [[nodiscard]] IOReturn Write(uint64_t address, uint32_t value, const char* operation) {
        if (Cancelled()) return kIOReturnAborted;
        const IOReturn status = AwaitStageStatus([&](auto cb) {
            const auto handle = io_.WriteQuadBE(Address(address), value,
                [cb = std::move(cb)](Async::AsyncStatus s) mutable {
                    cb(Protocols::Ports::MapAsyncStatusToIOReturn(s));
                });
            (void)handle;
        }, cancel_, kRegisterTimeoutMs);
        const IOReturn result =
            status == kIOReturnSuccess && !io_.IsRouteCurrent() ? kIOReturnOffline : status;
        if (result != kIOReturnSuccess) {
            ASFW_LOG(Audio, "[Digi00x] %{public}s: write 0x%llx <- 0x%08x failed kr=0x%08x",
                     operation, address, value, result);
        }
        return result;
    }

    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> Stage(DuplexRestartPhase phase) const {
        return DuplexStageResult{.generation = io_.Generation(), .channels = channels_,
            .phase = phase, .runtimeCaps = caps_};
    }

    Protocols::Ports::ProtocolRegisterIO& io_;
    const std::atomic<bool>* cancel_{nullptr};
    bool configured_{false};
    bool channelsWritten_{false};
    bool enabled_{false};
    uint32_t rateHz_{0};
    AudioDuplexChannels channels_{};
    AudioStreamRuntimeCaps caps_{};
    FW::Generation stateGeneration_{0};
    std::optional<bool> lastHealthy_;
};

} // namespace ASFW::Audio::Digi00x
