// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapter.hpp - MOTU V1/V2/V3 on the shared audio host.
// Describe reads current clock/optical state and publishes model-derived
// formations. A failed read refuses publication; it never substitutes counts.
// V3 notifications acknowledge explicit clock changes. Unsolicited front-panel
// changes are not yet forwarded through DeviceEventSink.

#pragma once

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

    void SetEventSink(DeviceEventSink* sink) noexcept override {
        // Clock-change acknowledgements are consumed by the protocol. Publishing
        // unsolicited control changes needs a separate host event bridge.
        (void)sink;
    }

    /// The endpoint the MOTU backend published, with the geometry the protocol
    /// currently reports from completed clock/optical reads.
    [[nodiscard]] static Model::ASFWAudioDevice BuildNubConfig(const Discovery::DeviceRecord& record,
                                                               const IDeviceProtocol& protocol);
};

} // namespace ASFW::Audio::Host
