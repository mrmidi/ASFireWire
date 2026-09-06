// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kAudioTuningWireVersion = 2;

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
    // Frames a data packet carries at the current rate, so the panel converts
    // packets to frames with the driver's own number rather than assuming 6.
    uint32_t framesPerPacketAverage{0};

    // --- state ---------------------------------------------------------------
    uint32_t streaming{0};          // 1 while IO is running
    uint32_t pendingGroups{0};      // non-zero: a candidate is parked, not yet applied
    uint32_t appliedSequence{0};    // increments on every successful apply
    uint32_t lastError{0};          // IOReturn of the last asynchronous outcome
    uint32_t lastWarnings{0};       // TuningWarning mask of the last accepted apply
    uint32_t requestId{0};
    uint32_t requestStatus{0};      // TuningRequestStatus
    uint32_t supportedGroups{0};
    uint32_t ready{0};
    uint32_t _reserved1{0};
};
static_assert(sizeof(AudioTuningSnapshotWire) == 128,
              "AudioTuningSnapshotWire must stay 128 bytes");

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
