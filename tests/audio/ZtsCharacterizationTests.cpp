// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ZtsCharacterizationTests.cpp - the zero timestamps the RX path publishes, recorded.
//
// Epic 4 (documentation/HARDWARE_TIMELINE_OWNERSHIP.md) moves the RX anchor
// onto HardwareSampleTimeline. Before that, this drives the real
// DirectAudioReceiveConsumer with a synthetic AMDTP stream and records every
// anchor it hands to HostClockAnchor, into tests/golden/zts/<case>.txt. A clean
// stream must keep its anchors byte-identical through the change; loss and
// restart may move only in the lines the stage declares.
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "WireTrace.hpp"

#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Common/TimingUtils.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer;

// Host ticks equal nanoseconds, so the goldens do not depend on the machine.
struct TimebaseGuard final {
    mach_timebase_info_data_t old{ASFW::Timing::gHostTimebaseInfo};
    TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = {1, 1}; }
    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = old; }
};

class FixedBinding final : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
public:
    explicit FixedBinding(ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot) noexcept
        : snapshot_(snapshot) {}
    bool CopyDirectAudioBinding(ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out = snapshot_;
        return true;
    }
    // A new StartIO publishes a new binding generation.
    void Rebind() noexcept { ++snapshot_.generation; }

private:
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot_{};
};

constexpr uint32_t kChannels = 2;
constexpr uint32_t kFramesPerDataPacket = 8;  // blocking, SYT interval 8 (1x)
constexpr uint64_t kTicksPerCycle = 3072;
constexpr uint64_t kCyclesPerBatch = 8;
constexpr uint64_t kHostBaseNs = 1'000'000'000ULL;

uint32_t CycleTimerAt(uint64_t cycle) {
    const uint64_t seconds = (cycle / 8000) % 128;
    return static_cast<uint32_t>((seconds << ASFW::Timing::kCycleTimerSecondsShift) |
                                 ((cycle % 8000) << ASFW::Timing::kCycleTimerCyclesShift));
}

void WriteBE32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

// What the stream does to one cycle's packet.
enum class Fault { None, Drop };

struct StreamSpec {
    uint32_t rateHz{48000};
    bool withSyt{true};
    uint64_t cycles{0};
    // Cycles whose packet never arrives.
    std::vector<uint64_t> dropped{};
    // Quiesce and re-activate the consumer just before this cycle (0: never).
    uint64_t restartAt{0};
    // Begin a Receive epoch on the device timeline at each start, as StartIO
    // does since Epic 4 T3; the RX path then publishes through the timeline.
    bool timelineEpoch{false};
};

// Blocking AMDTP: a DATA packet of 8 frames whenever 8 frames have accrued by
// the end of the cycle, otherwise NO-DATA. SYT is the exact presentation time
// of the packet's first frame, two cycles after reception.
class StreamGenerator {
public:
    explicit StreamGenerator(const StreamSpec& spec) : spec_(spec) {}

    // Fills `out` with the cycle's packet and says whether it carries data.
    std::vector<uint8_t> PacketFor(uint64_t cycle) {
        const uint64_t accrued = (cycle + 1) * spec_.rateHz / 8000;
        const bool data = accrued - emittedFrames_ >= kFramesPerDataPacket;
        const uint32_t payloadBytes = data ? kFramesPerDataPacket * kChannels * 4 : 0;
        std::vector<uint8_t> packet(8 + 8 + payloadBytes, 0);
        // OHCI receive prefix: the 16-bit receive timestamp, little-endian.
        const uint32_t ts = CycleTimerAt(cycle) >> ASFW::Timing::kCycleTimerCyclesShift;
        const uint16_t stamp = static_cast<uint16_t>(((ts >> 13) & 0x7) << 13 | (ts & 0x1FFF));
        packet[0] = static_cast<uint8_t>(stamp & 0xFF);
        packet[1] = static_cast<uint8_t>(stamp >> 8);
        WriteBE32(packet.data() + 8, 0x02000000u | (kChannels << 16) | (dbc_ & 0xFF));
        // IEC 61883-6 FDF sample-frequency code: 32 kHz 0, 44.1 kHz 1, 48 kHz 2.
        const uint8_t fdf = spec_.rateHz == 32000 ? 0x00 : spec_.rateHz == 44100 ? 0x01 : 0x02;
        if (!data) {
            WriteBE32(packet.data() + 12, 0x90FFFFFFu);
            return packet;
        }
        uint32_t syt = 0xFFFF;
        if (spec_.withSyt) {
            // Presentation of this packet's first frame, exact in bus ticks.
            const uint64_t presentation =
                (2 + 1) * kTicksPerCycle + emittedFrames_ * 24'576'000ULL / spec_.rateHz;
            syt = static_cast<uint32_t>((((presentation / kTicksPerCycle) & 0xF) << 12) |
                                        (presentation % kTicksPerCycle));
        }
        WriteBE32(packet.data() + 12, 0x90000000u | (uint32_t{fdf} << 16) | syt);
        for (uint32_t i = 0; i < kFramesPerDataPacket * kChannels; ++i) {
            WriteBE32(packet.data() + 16 + i * 4, 0x40000000u);
        }
        emittedFrames_ += kFramesPerDataPacket;
        dbc_ += kFramesPerDataPacket;
        return packet;
    }

private:
    const StreamSpec& spec_;
    uint64_t emittedFrames_{0};
    uint32_t dbc_{0};
};

// Runs the stream through the consumer and records every published anchor.
ASFW::Testing::WireTrace RecordAnchors(const StreamSpec& spec) {
    TimebaseGuard timebase{};
    std::vector<float> input(8192 * kChannels, 0.0f);
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    FixedBinding binding({
        .generation = 1,
        .inputBase = input.data(),
        .inputBytes = static_cast<uint32_t>(input.size() * sizeof(float)),
        .inputFrames = 8192,
        .inputChannels = kChannels,
        .control = &control,
        .sampleRateHz = spec.rateHz,
        .valid = true,
    });
    DirectAudioReceiveConsumer consumer(&binding, {.am824Slots = kChannels,
                                                   .streamChannels = kChannels});
    auto beginEpoch = [&] {
        if (spec.timelineEpoch) {
            (void)control.hardwareTimeline.BeginEpoch(
                ASFW::Audio::Runtime::HardwareTimelineSource::Receive,
                ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO, spec.rateHz, 0);
        }
    };
    beginEpoch();
    consumer.OnReceiveActivated();

    ASFW::Testing::WireTrace trace;
    char line[160];
    std::snprintf(line, sizeof(line), "stream rate=%u syt=%u cycles=%llu", spec.rateHz,
                  spec.withSyt ? 1U : 0U, static_cast<unsigned long long>(spec.cycles));
    trace.Add(line);

    StreamGenerator generator(spec);
    uint64_t lastGeneration = 0;
    for (uint64_t batchStart = 0; batchStart < spec.cycles; batchStart += kCyclesPerBatch) {
        const uint64_t batchEnd = std::min(batchStart + kCyclesPerBatch, spec.cycles);
        if (spec.restartAt != 0 && spec.restartAt >= batchStart && spec.restartAt < batchEnd) {
            // As StopIO -> StartIO: quiesce, reset the control block, publish a
            // new binding generation, re-activate.
            consumer.OnReceiveQuiesced();
            control.ResetForStart();
            beginEpoch();
            binding.Rebind();
            consumer.OnReceiveActivated();
            lastGeneration = 0;
            std::snprintf(line, sizeof(line), "restart at cycle %llu",
                          static_cast<unsigned long long>(spec.restartAt));
            trace.Add(line);
        }
        // The drain happens at the start of the cycle after the batch.
        const ASFW::Isoch::IsochReceiveBatch batch{
            .drainCycleTimer = CycleTimerAt(batchEnd),
            .drainHostTicks = kHostBaseNs + batchEnd * 125'000ULL,
        };
        consumer.BeginReceiveBatch(batch);
        for (uint64_t cycle = batchStart; cycle < batchEnd; ++cycle) {
            std::vector<uint8_t> packet = generator.PacketFor(cycle);
            bool dropped = false;
            for (const uint64_t d : spec.dropped) {
                dropped = dropped || d == cycle;
            }
            if (dropped) {
                std::snprintf(line, sizeof(line), "drop cycle %llu",
                              static_cast<unsigned long long>(cycle));
                trace.Add(line);
                continue;
            }
            const ASFW::Isoch::IsochReceivePacket isochPacket{
                .descriptorIndex = static_cast<uint32_t>(cycle % 64),
                .payload = packet,
            };
            consumer.ConsumePacket(batch, isochPacket);
            ASFW::Audio::Runtime::HostClockAnchorSample anchor{};
            if (control.hostClockAnchor.TryReadLatest(lastGeneration, anchor) &&
                anchor.generation != lastGeneration) {
                lastGeneration = anchor.generation;
                // Through the timeline, every anchor carries the live epoch.
                if (spec.timelineEpoch) {
                    EXPECT_EQ(anchor.timelineEpoch, control.hardwareTimeline.Epoch())
                        << "cycle " << cycle;
                    EXPECT_NE(anchor.timelineEpoch, 0U);
                }
                std::snprintf(line, sizeof(line),
                              "anchor cycle=%llu frame=%llu hostNs=%llu nsPerSampleQ8=%llu",
                              static_cast<unsigned long long>(cycle),
                              static_cast<unsigned long long>(anchor.sampleFrame),
                              static_cast<unsigned long long>(anchor.hostTicks - kHostBaseNs),
                              static_cast<unsigned long long>(anchor.hostNanosPerSampleQ8));
                trace.Add(line);
            }
        }
    }
    if (spec.timelineEpoch && spec.withSyt) {
        // The timeline really carried the clock (not the grid-rule fallback),
        // and an established loss began a new epoch.
        EXPECT_GT(control.hardwareTimeline.observations_.load(), 0U);
        EXPECT_EQ(control.hardwareTimeline.Source(),
                  ASFW::Audio::Runtime::HardwareTimelineSource::Receive);
        if (!spec.dropped.empty()) {
            EXPECT_EQ(control.hardwareTimeline.DiscontinuityReason(),
                      ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::PresentationLoss);
        }
    }
    std::snprintf(line, sizeof(line), "replayResets=%llu",
                  static_cast<unsigned long long>(control.rxReplayEpochResets.load()));
    trace.Add(line);
    return trace;
}

// The recorded anchors must come out of both paths: the previous grid rule and
// the hardware timeline (a Receive epoch begun at StartIO). Epic 4 T4 moves the
// RX clock onto the timeline without moving a single anchor.
void ExpectBothPathsMatch(StreamSpec spec, const char* golden) {
    ASFW::Testing::ExpectMatchesGolden(RecordAnchors(spec), golden);
    spec.timelineEpoch = true;
    ASFW::Testing::ExpectMatchesGolden(RecordAnchors(spec), golden);
}

// About five ZTS periods (12288 frames at 1x) of a clean 48 kHz stream.
TEST(ZtsCharacterization, Clean48k) {
    ExpectBothPathsMatch({.rateHz = 48000, .cycles = 11'000}, "zts/clean-48k.txt");
}

// 32 kHz had no timeline epoch before Epic 4 T5; both paths must agree there too.
TEST(ZtsCharacterization, Clean32k) {
    ExpectBothPathsMatch({.rateHz = 32000, .cycles = 16'000}, "zts/clean-32k.txt");
}

TEST(ZtsCharacterization, Clean44k1) {
    ExpectBothPathsMatch({.rateHz = 44100, .cycles = 12'000}, "zts/clean-44k1.txt");
}

// One DATA packet lost after the second anchor. Today the frame count carries
// on across the gap, so later anchors are short by the lost frames.
TEST(ZtsCharacterization, LostPacket48k) {
    ExpectBothPathsMatch({.rateHz = 48000, .cycles = 11'000, .dropped = {4'501}}, "zts/lost-packet-48k.txt");
}

// A run of lost cycles (a longer gap).
TEST(ZtsCharacterization, CycleGap48k) {
    ExpectBothPathsMatch({.rateHz = 48000, .cycles = 11'000,
             .dropped = {4'501, 4'502, 4'503, 4'504, 4'505, 4'506, 4'507, 4'508}}, "zts/cycle-gap-48k.txt");
}

// No SYT on the wire (as RME/MOTU): without a device timing observer the
// cadence gate never opens, so today nothing is published.
TEST(ZtsCharacterization, NoSyt48k) {
    ExpectBothPathsMatch({.rateHz = 48000, .withSyt = false, .cycles = 6'000}, "zts/no-syt-48k.txt");
}

// The consumer quiesced and re-activated mid-stream (a transport restart).
TEST(ZtsCharacterization, Restart48k) {
    ExpectBothPathsMatch({.rateHz = 48000, .cycles = 11'000, .restartAt = 4'800}, "zts/restart-48k.txt");
}

} // namespace
