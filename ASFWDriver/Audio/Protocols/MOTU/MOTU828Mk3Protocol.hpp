// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MOTU 828 Mk3 (protocol V3) device protocol and its FamilyDriver.
//
// The stages stay callback chains over async register IO; the FamilyDriver
// steps start a chain and wait for it (FamilyStageWait.hpp), so the wire
// traffic is the same as under the duplex coordinator it replaced.

#pragma once

#include "MOTU828Mk3Geometry.hpp"
#include "MOTU828Mk3RegisterIO.hpp"
#include "../Duplex/FamilyDriver.hpp"
#include "../IDeviceProtocol.hpp"
#include "../../../Protocols/Ports/FireWireBusPort.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::MOTU {

class MOTU828Mk3Protocol final : public IDeviceProtocol,
                                 public FamilyDriver {
public:
    MOTU828Mk3Protocol(Protocols::Ports::FireWireBusOps& busOps,
                       Protocols::Ports::FireWireBusInfo& busInfo,
                       Discovery::DeviceRegistry& routeRegistry,
                       const Discovery::DeviceRouteToken& route,
                       IRM::IRMClient* irmClient,
                       Scheduling::ITimerScheduler* timerScheduler) noexcept;
    ~MOTU828Mk3Protocol() noexcept override;

    MOTU828Mk3Protocol(const MOTU828Mk3Protocol&) = delete;
    MOTU828Mk3Protocol& operator=(const MOTU828Mk3Protocol&) = delete;

    // IDeviceProtocol
    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "MOTU 828 Mk3 (V3)"; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    FamilyDriver* AsFamilyDriver() noexcept override { return this; }
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) override;

    void EnsureRuntimeStreamGeometry(VoidCallback callback) override;

    // ---- Stage chains (callback form; the FamilyDriver steps below wait on them) ----
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback);
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept;
    void ProgramRx(StageCallback callback);
    void ProgramTxAndEnableDuplex(StageCallback callback);
    void ConfirmDuplexStart(ConfirmCallback callback);
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback);
    void ReadDuplexHealth(HealthCallback callback);
    void DisconnectPlayback(VoidCallback callback);
    void DisconnectCapture(VoidCallback callback);
    void BreakBothConnections(VoidCallback callback);

    // ---- FamilyDriver ----
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;
    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    [[nodiscard]] std::expected<AudioDuplexChannels, IOReturn> AssignChannels(
        const AudioDuplexChannels& channels) override;
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override;
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override;
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override;
    [[nodiscard]] IOReturn DisconnectPlayback() override;
    [[nodiscard]] IOReturn DisconnectCapture() override;
    [[nodiscard]] IOReturn BreakConnections() override;
    [[nodiscard]] IOReturn Stop() override;

private:
    struct SettleState;

    static constexpr uint64_t kDeactivateSettleNs = 20ULL * 1000ULL * 1000ULL;

    [[nodiscard]] bool TeardownRequested() const noexcept;
    void CancelSettle();
    void PublishGeometry(const StreamGeometry& geometry) noexcept;
    void PublishChannels(const AudioDuplexChannels& channels) noexcept;
    [[nodiscard]] AudioStreamRuntimeCaps RuntimeCapsSnapshot() const noexcept;
    [[nodiscard]] DuplexStageResult StageResult(DuplexRestartPhase phase) const noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    const uint64_t guid_;
    MOTU828Mk3RegisterIO registers_;
    IRM::IRMClient* irmClient_{nullptr};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    const std::atomic<bool>* teardownCancel_{nullptr};

    FW::Generation preparedGeneration_{0};
    AudioDuplexChannels duplexChannels_{};
    AudioClockConfig appliedClock_{.sampleRateHz = 0};
    std::optional<IsocControlWords> pendingIsocWords_{};
    std::shared_ptr<SettleState> activeSettle_{};

    // Runtime caps are reconstructed exclusively from atomics so synchronous
    // readers never race asynchronous register callbacks.
    std::atomic<uint32_t> capsSequence_{0};
    std::atomic<bool> runtimeCapsValid_{false};
    std::atomic<uint32_t> sampleRateHz_{0};
    std::atomic<uint32_t> hostInputPcm_{0};
    std::atomic<uint32_t> hostOutputPcm_{0};
    std::atomic<uint32_t> deviceToHostSlots_{0};
    std::atomic<uint32_t> hostToDeviceSlots_{0};
    std::atomic<uint8_t> deviceToHostChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
    std::atomic<uint8_t> hostToDeviceChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
};

} // namespace ASFW::Audio::MOTU
