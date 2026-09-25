// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Wire v4 contract for the audio telemetry snapshot (FW-175).
//
// The golden fixture tests/fixtures/audio_telemetry_v4.bin is shared with the
// Swift decoder tests (ASFWTests/AudioTelemetryWireTests.swift): both sides
// derive every expected value from the field's byte offset, so a layout change
// on either side fails a test rather than silently misreading a field.
//
// Regenerate after an intentional layout change:
//   ASFW_REGENERATE_FIXTURES=1 ctest --test-dir build/tests_build -R AudioTelemetryWire

#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/Runtime/AudioTelemetrySnapshot.hpp"
#include "Audio/Runtime/Seqlock.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace ASFW::Audio::Runtime;

#ifndef ASFW_TEST_FIXTURE_DIR
#error "ASFW_TEST_FIXTURE_DIR must point at tests/fixtures"
#endif

constexpr uint32_t kGoldenEndpoints = 2;
constexpr uint64_t kGoldenCaptureTicks = 0x0123'4567'89AB'CDEFULL;
constexpr uint32_t kGoldenNumer = 125;
constexpr uint32_t kGoldenDenom = 3;
constexpr uint32_t kGoldenFlags = 0x7F;

// u32 regions of the v4 record; everything else is u64.
[[nodiscard]] constexpr bool IsU32Slot(size_t offset) noexcept {
    return (offset >= 184 && offset < 224) || offset == 368 || offset == 372;
}

// Must match expectedValue(offset:endpoint:) in the Swift test.
[[nodiscard]] constexpr uint64_t GoldenU64(uint32_t endpoint, size_t offset) noexcept {
    return (static_cast<uint64_t>(endpoint + 1) << 40) + offset;
}
[[nodiscard]] constexpr uint32_t GoldenU32(uint32_t endpoint, size_t offset) noexcept {
    return ((endpoint + 1) << 16) + static_cast<uint32_t>(offset);
}

AudioTelemetrySnapshot MakeGolden() {
    AudioTelemetrySnapshot snapshot{};
    snapshot.header.captureHostTicks = kGoldenCaptureTicks;
    snapshot.header.hostTimebaseNumer = kGoldenNumer;
    snapshot.header.hostTimebaseDenom = kGoldenDenom;
    snapshot.header.endpointCount = kGoldenEndpoints;
    for (uint32_t e = 0; e < kGoldenEndpoints; ++e) {
        auto* bytes = reinterpret_cast<uint8_t*>(&snapshot.endpoints[e]);
        size_t offset = 0;
        while (offset < kAudioTelemetryEndpointRecordBytes) {
            if (IsU32Slot(offset)) {
                uint32_t value = GoldenU32(e, offset);
                if (offset == offsetof(AudioTelemetryEndpointSnapshot, flags)) {
                    value = kGoldenFlags;
                } else if (offset == offsetof(AudioTelemetryEndpointSnapshot, reserved0)) {
                    value = 0;
                }
                std::memcpy(bytes + offset, &value, sizeof(value));
                offset += sizeof(value);
            } else {
                const uint64_t value = GoldenU64(e, offset);
                std::memcpy(bytes + offset, &value, sizeof(value));
                offset += sizeof(value);
            }
        }
    }
    return snapshot;
}

std::vector<uint8_t> Serialize(AudioTelemetrySnapshot snapshot) {
    std::array<uint8_t, sizeof(AudioTelemetrySnapshot)> buffer{};
    const size_t bytes = SerializeAudioTelemetry(snapshot, buffer);
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes)};
}

TEST(AudioTelemetryWireTests, WorstCaseFitsTheInlineReply) {
    EXPECT_LE(AudioTelemetryWireBytes(kAudioTelemetryMaxEndpoints),
              kAudioTelemetryInlineReplyLimitBytes);
    EXPECT_EQ(AudioTelemetryWireBytes(0), sizeof(AudioTelemetryHeader));
    // Over-count is clamped, never serialised past the array.
    EXPECT_EQ(AudioTelemetryWireBytes(99), AudioTelemetryWireBytes(kAudioTelemetryMaxEndpoints));
}

TEST(AudioTelemetryWireTests, SerializesHeaderAndOnlyPopulatedRecords) {
    const auto wire = Serialize(MakeGolden());
    ASSERT_EQ(wire.size(), 32U + kGoldenEndpoints * 464U);

    AudioTelemetryHeader header{};
    std::memcpy(&header, wire.data(), sizeof(header));
    EXPECT_EQ(header.version, 4U);
    EXPECT_EQ(header.headerBytes, 32U);
    EXPECT_EQ(header.totalBytes, wire.size());
    EXPECT_EQ(header.endpointCount, kGoldenEndpoints);
    EXPECT_EQ(header.endpointRecordBytes, 464U);
    EXPECT_EQ(header.captureHostTicks, kGoldenCaptureTicks);
    EXPECT_EQ(header.hostTimebaseNumer, kGoldenNumer);
    EXPECT_EQ(header.hostTimebaseDenom, kGoldenDenom);

    AudioTelemetryEndpointSnapshot second{};
    std::memcpy(&second, wire.data() + 32 + 464, sizeof(second));
    EXPECT_EQ(second.guid, GoldenU64(1, 0));
    EXPECT_EQ(second.rxCompletedIntervalEndHostTicks, GoldenU64(1, 456));
}

TEST(AudioTelemetryWireTests, RefusesAnUndersizedBuffer) {
    auto snapshot = MakeGolden();
    std::array<uint8_t, 100> small{};
    EXPECT_EQ(SerializeAudioTelemetry(snapshot, small), 0U);
}

TEST(AudioTelemetryWireTests, MatchesTheGoldenFixture) {
    const auto wire = Serialize(MakeGolden());
    const std::string path = std::string(ASFW_TEST_FIXTURE_DIR) + "/audio_telemetry_v4.bin";

    if (const char* regen = std::getenv("ASFW_REGENERATE_FIXTURES"); regen && *regen == '1') {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(wire.data()),
                  static_cast<std::streamsize>(wire.size()));
        ASSERT_TRUE(out.good()) << "could not write " << path;
    }

    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "missing fixture " << path
                           << " (set ASFW_REGENERATE_FIXTURES=1 to create it)";
    const std::vector<uint8_t> golden{std::istreambuf_iterator<char>(in), {}};
    EXPECT_EQ(golden, wire) << "wire layout changed: update the Swift decoder, then regenerate";
}

// "No field without a source": drive every ATCB source and require each
// control-sourced field to arrive in the snapshot.
TEST(AudioTelemetryWireTests, EveryControlSourcedFieldIsPopulated) {
    AudioTransportControlBlock control{};
    constexpr auto r = std::memory_order_relaxed;
    control.generation.store(3, r);
    control.txLastPreparationLatencyTicks.store(11, r);
    control.txMaxPreparationLatencyTicks.store(12, r);
    control.txPreparationLatencySamples.store(13, r);
    control.txPreparationAtMost750Us.store(14, r);
    control.txPreparationAtLeast1500Us.store(15, r);
    control.rxReplayEntries.store(16, r);
    control.rxReplayEpochResets.store(17, r);
    control.rxPacketsSeen.store(18, r);
    control.rxDataPackets.store(19, r);
    control.rxNoDataPackets.store(20, r);
    control.rxShortPackets.store(21, r);
    control.rxInvalidCipHeaders.store(22, r);
    control.rxZeroDataBlockSize.store(23, r);
    control.rxGeometryMismatch.store(24, r);
    control.txCurrentCommittedMarginPackets.store(25, r);
    control.txMinimumCommittedMarginPackets.store(26, r);
    control.captureRingOverruns.store(27, r);
    control.captureRingStarvations.store(28, r);

    // TX completed interval, published through the seqlock.
    SeqlockWriteBegin(control.txCompletedIntervalSequence);
    control.txCompletedIntervalMarginMinPackets.store(30, r);
    control.txCompletedIntervalMarginMaxPackets.store(31, r);
    control.txCompletedIntervalPreparationLatencyMaxTicks.store(32, r);
    control.txCompletedIntervalDurationTicks.store(33, r);
    control.txCompletedIntervalEndHostTicks.store(34, r);
    for (auto& bin : control.txCompletedIntervalPreparationLatencyHistogram) bin.store(35, r);
    for (auto& bin : control.txCompletedIntervalCommittedMarginHistogram) bin.store(36, r);
    SeqlockWriteEnd(control.txCompletedIntervalSequence);

    // RX interval via the real producer API; each occupancy bin gets a sample.
    auto& rx = control.rxCaptureBufferTelemetry;
    ASSERT_FALSE(rx.CompleteIntervalIfDue(1000, 500));  // opens the interval
    for (uint64_t available : {10ULL, 300ULL, 500ULL, 700ULL, 990ULL}) {
        rx.Observe(available + 1, 1, 1000);
    }
    rx.RecordOverrun(5);
    rx.RecordStarvation(6);
    rx.RecordReaderBeginRead();
    ASSERT_TRUE(rx.CompleteIntervalIfDue(2000, 500));

    AudioTelemetryEndpointSnapshot s{};
    CopyAudioTelemetrySnapshot(control, s);

    for (const uint64_t value : {
             s.controlGeneration, s.completedIntervalSequence, s.lastPreparationLatencyTicks,
             s.completedIntervalMaxLatencyTicks, s.maxPreparationLatencyTicks,
             s.preparationWakeCount, s.preparationAtMost750Us, s.preparationAtLeast1500Us,
             s.rxReplayEntries, s.rxReplayEpochResets, s.rxCurrentAvailableFrames,
             s.rxCompletedIntervalSequence, s.rxCompletedIntervalMinimumAvailableFrames,
             s.rxCompletedIntervalMaximumAvailableFrames,
             s.rxCompletedIntervalMinimumFreeHeadroomFrames, s.rxCompletedIntervalOverrunEvents,
             s.rxCompletedIntervalOverwrittenFrames, s.rxCompletedIntervalStarvationEvents,
             s.rxCompletedIntervalStarvedFrames, s.rxCaptureOverrunEvents,
             s.rxCaptureStarvationEvents, s.rxTotalOverwrittenFrames, s.rxTotalStarvedFrames,
             s.rxPacketsSeen, s.rxDataPackets, s.rxNoDataPackets, s.rxShortPackets,
             s.rxInvalidCipHeaders, s.rxZeroDataBlockSize, s.rxGeometryMismatch,
             s.txCompletedIntervalDurationTicks, s.txCompletedIntervalEndHostTicks,
             s.rxCompletedIntervalDurationTicks, s.rxCompletedIntervalEndHostTicks}) {
        EXPECT_NE(value, 0U);
    }
    for (const uint32_t value : {s.currentCommittedMarginPackets,
                                 s.completedIntervalMarginMinPackets,
                                 s.completedIntervalMarginMaxPackets,
                                 s.minimumCommittedMarginPackets}) {
        EXPECT_NE(value, 0U);
    }
    for (const uint64_t bin : s.completedLatencyHistogram) EXPECT_NE(bin, 0U);
    for (const uint64_t bin : s.completedMarginHistogram) EXPECT_NE(bin, 0U);
    for (const uint64_t bin : s.rxCompletedOccupancyHistogram) EXPECT_NE(bin, 0U);

    EXPECT_EQ(s.txCompletedIntervalDurationTicks, 33U);
    EXPECT_EQ(s.rxCompletedIntervalDurationTicks, 1000U);
    EXPECT_EQ(s.rxCompletedIntervalEndHostTicks, 2000U);
    for (const uint32_t flag : {kAudioTelemetryHasCompletedInterval,
                                kAudioTelemetryHasCompletedRxInterval,
                                kAudioTelemetryRxCaptureReaderActive,
                                kAudioTelemetryTxIntervalDurationKnown,
                                kAudioTelemetryRxIntervalDurationKnown}) {
        EXPECT_NE(s.flags & flag, 0U) << "flag " << flag;
    }
}

// A copy that never stabilises (writer mid-publish) is discarded, not torn.
TEST(AudioTelemetryWireTests, UnstableIntervalIsDiscardedNotTorn) {
    AudioTransportControlBlock control{};
    control.txCompletedIntervalMarginMinPackets.store(42, std::memory_order_relaxed);
    control.txCompletedIntervalDurationTicks.store(99, std::memory_order_relaxed);
    control.txCompletedIntervalSequence.store(3, std::memory_order_relaxed);  // odd: writing
    control.rxCaptureBufferTelemetry.completedIntervalSequence.store(5, std::memory_order_relaxed);
    control.rxCaptureBufferTelemetry.completedIntervalDurationTicks.store(
        77, std::memory_order_relaxed);

    AudioTelemetryEndpointSnapshot s{};
    CopyAudioTelemetrySnapshot(control, s);

    const AudioTelemetryEndpointSnapshot defaults{};
    EXPECT_EQ(s.completedIntervalSequence, 0U);
    EXPECT_EQ(s.completedIntervalMarginMinPackets, defaults.completedIntervalMarginMinPackets);
    EXPECT_EQ(s.txCompletedIntervalDurationTicks, 0U);
    EXPECT_EQ(s.rxCompletedIntervalSequence, 0U);
    EXPECT_EQ(s.rxCompletedIntervalDurationTicks, 0U);
    EXPECT_EQ(s.flags & (kAudioTelemetryHasCompletedInterval |
                         kAudioTelemetryHasCompletedRxInterval |
                         kAudioTelemetryTxIntervalDurationKnown |
                         kAudioTelemetryRxIntervalDurationKnown),
              0U);
}

TEST(AudioTelemetryWireTests, SeqlockReadRejectsAConcurrentWrite) {
    std::atomic<uint64_t> sequence{2};
    int copies = 0;
    // The "writer" publishes during every copy, so no copy is ever stable.
    const auto result = SeqlockTryRead(sequence, [&] {
        ++copies;
        SeqlockWriteBegin(sequence);
        SeqlockWriteEnd(sequence);
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(copies, 4);

    const auto stable = SeqlockTryRead(sequence, [] {});
    ASSERT_TRUE(stable.has_value());
    EXPECT_EQ(*stable, sequence.load());
}

TEST(AudioTelemetryWireTests, RxFallbackCloseOnlyWhenDue) {
    AudioTransportControlBlock control{};
    auto& rx = control.rxCaptureBufferTelemetry;
    EXPECT_FALSE(rx.CompleteIntervalIfDue(100, 1000));   // opens at 100
    EXPECT_FALSE(rx.CompleteIntervalIfDue(900, 1000));   // not due
    EXPECT_TRUE(rx.CompleteIntervalIfDue(1100, 1000));   // due: 1000 ticks
    EXPECT_EQ(rx.completedIntervalDurationTicks.load(), 1000U);
    // The heartbeat closing it resets the fallback's clock.
    EXPECT_TRUE(rx.CompleteInterval(1500));
    EXPECT_EQ(rx.completedIntervalDurationTicks.load(), 400U);
    EXPECT_FALSE(rx.CompleteIntervalIfDue(2400, 1000));
    EXPECT_TRUE(rx.CompleteIntervalIfDue(2500, 1000));
}

}  // namespace
