// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFamilyAdapter.hpp - The AV/C audio family on the audio device host (E4).
//
// documentation/AUDIO_DEVICE_HOST.md §4.2. Replaces AVCAudioBackend for BeBoB
// (Phase 88, M-Audio special, Onyx), Fireworks, Apogee Duet and generic AV/C
// units.
//
// Describe: AV/C discovery (AVC::DiscoveryCoordinator) reads the endpoint from
// the device and pushes it; the host stores the last push and hands it back as
// DescribeInput::discovered. The adapter returns it unchanged, or refuses with
// NotReady until discovery has delivered one.
//
// JudgeRuntimeFault: the AV/C health verdict is RX cadence, not a register
// probe. A timing loss is debounced for kTimingLossSettleMs; if the host RX
// path re-established replay meanwhile, the gap was host-side and healed.
//
// Device events: AV/C raises none. Discovery's "configuration ready" arrives as
// OfferDiscoveredDescription, not as a DeviceEvent.

#pragma once

#include "FamilyAdapter.hpp"

namespace ASFW::Audio::Host {

class AvcFamilyAdapter final : public FamilyAdapter {
public:
    // Debounce before escalating an RX timing-loss to a restart. AppleFWAudio
    // uses 80 ms x 2 consecutive late RX callbacks; we settle ~256 ms (>= several
    // IO windows) so a host-side StartIO/StopIO gap that the RX epoch reset
    // self-heals is not mistaken for a device outage.
    static constexpr uint32_t kTimingLossSettleMs = 256;
    static constexpr uint32_t kTimingLossPollMs = 32;
    // A device that comes back only partially cannot restart-loop forever: the
    // session stops recovering after repeated failures
    // (SessionScheduler::kMaxFaultRestartFailures).

    [[nodiscard]] const char* Name() const noexcept override { return "AV/C"; }

    void Describe(const DescribeInput& in, DescribeDone done) override;

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override;

    void SetEventSink(DeviceEventSink* sink) noexcept override {
        // AV/C raises no device events: discovery pushes its description through
        // AudioDeviceHost::OfferDiscoveredDescription, and the unit has no
        // notification mailbox. Nothing to store (§4.1 rule 6).
        (void)sink;
    }
};

} // namespace ASFW::Audio::Host
