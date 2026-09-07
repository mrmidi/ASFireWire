// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kAudioTuningWireVersion = 3;

// One snapshot answers both questions the panel asks: what is the driver
// running, and what is CoreAudio being told. Keeping them in one struct means
// the two can never be read a configuration change apart and displayed as if
// they belonged together.
struct AudioTuningSnapshotWire final {
    uint32_t version{kAudioTuningWireVersion};
    uint32_t _reserved0{0};
    uint64_t endpointId{0};

    // --- geometry actually in force -----------------------------------------
    uint32_t txDispatchSlackPackets{0};
    uint32_t txOwnershipGuardPackets{0};
    uint32_t preparedTargetPackets{0};
    uint32_t preparedLeadFrames{0};

    // --- declared to CoreAudio ----------------------------------------------
    // Resolved values, i.e. what SetOutputLatency and friends were actually
    // given, not the operator's override. Zero override means these are the
    // device profile's own numbers.
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};

    // --- HAL geometry --------------------------------------------------------
    uint32_t frameRingFrames{0};
    uint32_t clientIoBudgetFrames{0};
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t sampleRateHz{0};

    // --- context the panel shows but cannot change ---------------------------
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};
    uint32_t txPacketsPerGroup{0};
    uint32_t txHardwareRingPackets{0};
    uint32_t txSharedSlotPackets{0};
    // Blocking SYT interval: frames one DATA packet carries at the current
    // rate (8 / 16 / 32). Zero when no V3 rate is running. This replaced a
    // `framesPerPacketAverage` field that was computed as sampleRateHz / 8000
    // and therefore truncated to 5 at 44.1 kHz -- the panel needs the exact
    // cadence quantity, and derives the 6-frames-per-cycle average itself from
    // sampleRateHz and isochCyclesPerSecond, both of which are exact.
    uint32_t framesPerDataPacket{0};

    // --- state ---------------------------------------------------------------
    uint32_t streaming{0};          // 1 while IO is running
    uint32_t pendingGroups{0};      // non-zero: a candidate is parked, not yet applied
    uint32_t appliedSequence{0};    // increments on every successful apply
    uint32_t lastError{0};          // IOReturn of the last asynchronous outcome
    uint32_t lastWarnings{0};       // TuningWarning mask of the last accepted apply
    uint32_t requestId{0};
    uint32_t requestStatus{0};      // TuningRequestStatus
    uint32_t supportedGroups{0};
    // 1 once the audio graph has published its geometry. A snapshot with
    // ready == 0 is still a valid snapshot: the state fields above are the only
    // record of a request that was aborted by the disconnect which cleared it,
    // so it is reported rather than refused.
    uint32_t ready{0};

    // --- read-only geometry (AudioGeometryReport) ----------------------------
    // Published so the panel renders the driver's numbers instead of deriving
    // its own. Everything here is compile-time structure or a function of
    // sampleRateHz; rate-dependent fields are zero when no V3 rate is running.
    uint32_t txDispatchSlackFloorPackets{0};
    uint32_t txDispatchSlackDefaultPackets{0};
    uint32_t isochCyclesPerSecond{0};
    uint32_t microsecondsPerIsochCycle{0};
    uint32_t rxPacketsPerGroup{0};
    uint32_t rxHardwareRingPackets{0};
    // Exact. Interrupts per second is 8000/6 and does not survive an integer,
    // so the interval is published and the reciprocal is the panel's to take.
    uint32_t txInterruptIntervalMicroseconds{0};
    uint32_t rxInterruptIntervalMicroseconds{0};
    uint32_t framesPerCompletionGroupTx{0};
    uint32_t framesPerCompletionGroupRx{0};
    uint32_t minFramesPerRxInterrupt{0};
    uint32_t maxFramesPerRxInterrupt{0};
    uint32_t cadenceBlockPackets{0};
    uint32_t cadenceBlockFrames{0};
    uint32_t txContentFreezePackets{0};
    uint32_t txRepointGuardPackets{0};
    uint32_t schedulingJitterFrames{0};
    uint32_t frameAlignmentFrames{0};
    uint32_t pcmPublicationCacheFrames{0};
    uint32_t maxBlockingFramesPerDataPacket{0};
    uint32_t txSafetyOffsetPolicyFrames{0};
    uint32_t rxSafetyOffsetPolicyFrames{0};
    uint32_t reportedLatencyPolicyFrames{0};
    uint32_t completionBatchFrames{0};
    // Frames in one TX hardware-ring traversal: the step size of the observed
    // RTL lattice and of the committed-margin minimum.
    uint32_t txRingLapFrames{0};
};
static_assert(sizeof(AudioTuningSnapshotWire) == 224,
              "AudioTuningSnapshotWire must stay 224 bytes");
static_assert(offsetof(AudioTuningSnapshotWire, requestId) == 108,
              "state block must stay put: the app mirrors these offsets");
static_assert(offsetof(AudioTuningSnapshotWire, txDispatchSlackFloorPackets) ==
                  124,
              "geometry block must start immediately after `ready`");

// An apply request. `groups` is the TuningGroup mask; a zero mask is a no-op
// rather than an error, so a panel that submits with nothing selected does not
// restart the streams.
struct AudioTuningRequestWire final {
    uint32_t version{kAudioTuningWireVersion};
    uint32_t groups{0};
    uint64_t endpointId{0};

    uint32_t txDispatchSlackPackets{0};
    uint32_t txOwnershipGuardPackets{0};
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};
    uint32_t frameRingFrames{0};
    uint32_t clientIoBudgetFrames{0};
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t _reserved{0};
};
static_assert(sizeof(AudioTuningRequestWire) == 56,
              "AudioTuningRequestWire must stay 56 bytes");

} // namespace ASFW::UserClient::Wire
