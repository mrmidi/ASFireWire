// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFamilyAdapter.cpp - see AvcFamilyAdapter.hpp.

#include "AvcFamilyAdapter.hpp"

#include "../Protocols/DeviceProtocolChoice.hpp"

namespace ASFW::Audio::Host {

void AvcFamilyAdapter::Describe(const DescribeInput& in, DescribeDone done) {
    // The host only routes AV/C devices here; refuse anything else rather than
    // publish a discovered description for another family's device.
    if (!in.policy || ChooseAudioBackend(in.policy->plan) != AudioBackendKind::Avc) {
        done(DescribeRefusal{kIOReturnUnsupported, "not-an-avc-policy"});
        return;
    }
    // Discovery has not delivered a description yet (or the device was removed
    // and its stored description dropped). Its ready event publishes.
    if (!in.discovered) {
        done(DescribeRefusal{kIOReturnNotReady, "awaiting-avc-discovery"});
        return;
    }
    done(*in.discovered);
}

FaultVerdict AvcFamilyAdapter::JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) {
    (void)guid;
    (void)reason;

    // Debounce off the RX queue: give the [TxAlign] self-heal its transient
    // window. Check cancellation each tick so teardown aborts within one poll.
    // Teardown is not the only exit: the device itself can be unplugged during
    // the settle window (the ordinary case, not an edge one), and without the
    // liveness check the escalation runs against a device whose record, nub and
    // CoreAudio presence are already gone (FW-146).
    for (uint32_t waited = 0; waited < kTimingLossSettleMs; waited += kTimingLossPollMs) {
        if (context.Cancelled() || !context.StillStreaming()) {
            return FaultVerdict::kDeviceLeft;
        }
        context.Sleep(kTimingLossPollMs);
    }
    if (context.Cancelled() || !context.StillStreaming()) {
        return FaultVerdict::kDeviceLeft;
    }

    // AV/C health verdict = RX cadence (no register probe). If replay
    // re-established during the settle window the gap was host-side
    // (StartIO/StopIO churn, a brief RX gap) and already self-healed.
    if (context.ReceiveReplayEstablished()) {
        return FaultVerdict::kSelfHealed;
    }

    // Still stalled: a genuine device outage. A session restart re-establishes
    // CMP/PCR (the wire-observable recovery: bebob break_both_connections +
    // cmp_connection_establish).
    return FaultVerdict::kRestart;
}

} // namespace ASFW::Audio::Host
