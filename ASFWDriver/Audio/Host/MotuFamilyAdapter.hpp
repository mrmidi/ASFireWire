// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapter.hpp - MOTU V1/V2/V3 on the shared audio host.
// Describe reads current clock/optical state and publishes model-derived
// formations. A failed read refuses publication; it never substitutes counts.
// V3 notifications acknowledge explicit clock changes. An unsolicited V3
// CLK_CHANGED becomes a clock-status device event (E7c); other bits are logged
// only, until their meaning is traced.

#pragma once

#include <atomic>

#include "FamilyAdapter.hpp"

namespace ASFW::Audio::Host {

class MotuFamilyAdapter final : public FamilyAdapter {
public:
    /// DescribeRefusal reason: the optical config read failed and a nub is live.
    static constexpr const char* kReadFailedReason = "optical-config-read-failed";
    [[nodiscard]] const char* Name() const noexcept override { return "MOTU"; }

    void Describe(const DescribeInput& in, DescribeDone done) override;

    [[nodiscard]] bool ActsOn(DuplexRestartReason reason) const noexcept override {
        // MotuAudioBackend never acted on cycle inconsistent (IAudioBackend's
        // default no-op); everything else restarts.
        return reason != DuplexRestartReason::kRecoverAfterCycleInconsistent;
    }

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override {
        // MotuAudioBackend restarted on every timing loss without a health read.
        (void)guid;
        (void)reason;
        (void)context;
        return FaultVerdict::kRestart;
    }

    /// Subscribes to the MOTU notification table while a sink is installed;
    /// null unsubscribes, and no notification is running once it returns. A
    /// clock change the host asked for is claimed by the protocol's wait; an
    /// unsolicited one with the model's named clock-changed bit becomes
    /// kClockStatusChanged, which the host turns into its clock probe (§4.4).
    /// Other bits are only logged (E7c).
    void SetEventSink(DeviceEventSink* sink) noexcept override;

    /// The notification table's observer, public for tests.
    void OnUnsolicitedNotification(uint64_t guid, uint32_t bits, bool clockChanged) noexcept;

    ~MotuFamilyAdapter() noexcept override;

    /// The endpoint the MOTU backend published, with the geometry the protocol
    /// currently reports from completed clock/optical reads.
    [[nodiscard]] static Model::ASFWAudioDevice BuildNubConfig(const Discovery::DeviceRecord& record,
                                                               const IDeviceProtocol& protocol);

private:
    static void NotificationThunk(void* context, uint64_t guid, uint32_t bits, bool clockChanged) noexcept;
    std::atomic<DeviceEventSink*> sink_{nullptr};
};

} // namespace ASFW::Audio::Host
