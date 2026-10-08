// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapter.hpp - MOTU protocol-v2 register devices on the audio device
// host (E3).
//
// documentation/AUDIO_DEVICE_HOST.md §4.2. Replaces MotuAudioBackend.
//
// Describe: the optical config (0x0c04) is read asynchronously first
// (IDeviceProtocol::EnsureRuntimeStreamGeometry, as DICE loads its caps), and the
// published PCM channel counts follow from it: 14 fixed chunks, plus 8 per
// direction whose optical port is in ADAT mode (Linux motu-pcm.c:143 and
// motu-protocol-v2.c:246-262 read the same register at PCM open). The model's
// fixed 14 x 14 is published only when that read fails and no nub exists yet;
// against a live nub a failed read is a refusal, because a fixed fallback there
// would latch "geometry changed" on a transient bus error.
//
// JudgeRuntimeFault: MotuAudioBackend restarted on every timing loss without a
// health read, so every fault restarts.
//
// Device events: MOTU raises none today. The notification address
// (MotuV2Protocol::RegisterAsyncMessageAddress) is never registered, so the
// device has no way to tell us anything; E7c wires it.

#pragma once

#include "FamilyAdapter.hpp"

namespace ASFW::Audio::Host {

class MotuFamilyAdapter final : public FamilyAdapter {
public:
    /// DescribeRefusal reason: the optical config read failed and a nub is live.
    static constexpr const char* kReadFailedReason = "optical-config-read-failed";
    /// DescribedWithNote note: the optical config read failed; the model's fixed
    /// geometry was published instead.
    static constexpr const char* kFixedGeometryNote = "optical-read-failed-fixed-geometry";

    [[nodiscard]] const char* Name() const noexcept override { return "MOTU"; }

    void Describe(const DescribeInput& in, DescribeDone done) override;

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override {
        // MotuAudioBackend restarted on every timing loss without a health read.
        (void)guid;
        (void)reason;
        (void)context;
        return FaultVerdict::kRestart;
    }

    void SetEventSink(DeviceEventSink* sink) noexcept override {
        // MOTU raises no device events: no notification address is registered
        // (MotuV2Protocol::RegisterAsyncMessageAddress has no caller), so the
        // device never posts one. Nothing to store (§4.1 rule 6).
        (void)sink;
    }

    /// The endpoint the MOTU backend published, with the geometry the protocol
    /// currently reports (read from the optical config, or the fixed table).
    [[nodiscard]] static Model::ASFWAudioDevice BuildNubConfig(const Discovery::DeviceRecord& record,
                                                               const IDeviceProtocol& protocol);
};

} // namespace ASFW::Audio::Host
