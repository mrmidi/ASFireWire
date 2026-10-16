// MidiRxDrainTests.cpp
// The device -> host consumer: gap safety and epoch handling.
//
// The interleaving cases land the producer's action in the exact window the
// reviewed code left open (reviews/midi-2026-09-14, finding 1), via the
// afterConvert seam, rather than hoping a thread schedule finds them.

#include <gtest/gtest.h>

#include <vector>

#include "Midi/Transport/MidiRxDrain.hpp"

using namespace ASFW::Midi;
using ASFW::Midi::Ump::UmpWord;

namespace {

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> list) { return list; }

void Write(MidiByteRing& ring, std::initializer_list<uint8_t> bytes) {
    const std::vector<uint8_t> run(bytes);
    ASSERT_TRUE(ring.TryWrite(run, 0u));
}

struct Collected {
    std::vector<UmpWord> words;
    auto Sink() {
        return [this](std::span<const UmpWord> w) {
            words.insert(words.end(), w.begin(), w.end());
        };
    }
};

class MidiRxDrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        block.Arm(1);
        drain.ObserveEpoch(block);
    }
    MidiTransportBlock block;
    MidiRxDrain drain;
};

} // namespace

TEST_F(MidiRxDrainTest, ConvertsQueuedBytesIntoUmp) {
    Write(block.deviceToHost[2], {0x90, 0x3C, 0x40});
    Collected out;
    drain.DrainPort(block, 2, out.Sink());
    EXPECT_EQ(out.words, (std::vector<UmpWord>{0x2090'3C40u}));
    EXPECT_TRUE(block.deviceToHost[2].Empty());
}

TEST_F(MidiRxDrainTest, KeepsAMessageSplitAcrossWakesTogether) {
    // A Note On spans at least three packets on the wire, so it routinely
    // arrives over several wakes.
    Collected out;
    Write(block.deviceToHost[0], {0x90});
    drain.DrainPort(block, 0, out.Sink());
    Write(block.deviceToHost[0], {0x3C});
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_TRUE(out.words.empty());
    Write(block.deviceToHost[0], {0x40});
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_EQ(out.words, (std::vector<UmpWord>{0x2090'3C40u}));
}

TEST_F(MidiRxDrainTest, AGapAlreadyVisibleDiscardsTheBytesBeforeIt) {
    auto& ring = block.deviceToHost[0];
    Write(ring, {0x90, 0x3C});
    ring.MarkDiscontinuity();
    Write(ring, {0x40});

    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_TRUE(out.words.empty())
        << "pre-gap '90 3C' and post-gap '40' must never form a Note On";
    EXPECT_EQ(drain.Counters().gapsHandled, 1u);
}

TEST_F(MidiRxDrainTest, AGapMarkedWhileConvertingNeverPublishesAFabricatedMessage) {
    // The reviewed repro: '90 3C' is queued, the consumer converts it, the
    // producer then marks a gap and appends '40', and the consumer resumes. The
    // old drain sampled the gap count once before its loop and emitted
    // 0x20903C40.
    auto& ring = block.deviceToHost[0];
    Write(ring, {0x90, 0x3C});

    Collected out;
    bool injected = false;
    drain.DrainPort(block, 0, out.Sink(), [&] {
        if (injected) return;
        injected = true;
        ring.MarkDiscontinuity();
        Write(ring, {0x40});
    });
    ASSERT_TRUE(injected);
    EXPECT_TRUE(out.words.empty()) << "a span that converted across a gap must not be published";
    EXPECT_EQ(drain.Counters().spansDroppedForGap, 1u);
    EXPECT_GE(drain.Counters().gapsHandled, 1u);
    EXPECT_TRUE(ring.Empty());

    // The stream recovers: the next whole message after the gap comes through.
    Write(ring, {0x80, 0x3C, 0x40});
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_EQ(out.words, (std::vector<UmpWord>{0x2080'3C40u}));
}

TEST_F(MidiRxDrainTest, AGapDoesNotCostAnotherPortItsMessage) {
    Write(block.deviceToHost[0], {0x90, 0x3C});
    block.deviceToHost[0].MarkDiscontinuity();
    Write(block.deviceToHost[1], {0x90, 0x3C, 0x40});

    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    drain.DrainPort(block, 1, out.Sink());
    EXPECT_EQ(out.words, (std::vector<UmpWord>{0x2090'3C40u}));
}

TEST_F(MidiRxDrainTest, ARingOverflowIsAGap) {
    auto& ring = block.deviceToHost[0];
    Write(ring, {0x90, 0x3C});
    // Fill the ring so the next run is refused and marks the discontinuity.
    std::vector<uint8_t> filler(kMidiRingCapacityBytes - 2, 0xF8);
    ASSERT_TRUE(ring.TryWrite(filler, 0u));
    ASSERT_FALSE(ring.TryWrite(std::vector<uint8_t>{0x40}, 0u));
    ASSERT_EQ(ring.discontinuities.load(), 1u);

    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    for (const UmpWord word : out.words) {
        EXPECT_NE(word, 0x2090'3C40u) << "the dropped '40' must not be replaced by a neighbour";
    }
    EXPECT_EQ(drain.Counters().gapsHandled, 1u);
}

TEST_F(MidiRxDrainTest, ANewEpochDropsTheOldStreamsBacklogAndParserState) {
    auto& ring = block.deviceToHost[0];
    Write(ring, {0x90, 0x3C});
    Collected out;
    drain.DrainPort(block, 0, out.Sink());     // parser now holds '90 3C'
    Write(ring, {0x40});                       // would complete it...

    block.Arm(2);                              // ...but the stream restarted
    ASSERT_TRUE(drain.ObserveEpoch(block));
    EXPECT_EQ(drain.Counters().epochChanges, 2u);  // SetUp's arm + this one

    drain.DrainPort(block, 0, out.Sink());
    EXPECT_TRUE(out.words.empty()) << "the old run's partial message must not be completed";
    EXPECT_TRUE(ring.Empty());

    Write(ring, {0x90, 0x3E, 0x40});
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_EQ(out.words, (std::vector<UmpWord>{0x2090'3E40u}));
}

TEST_F(MidiRxDrainTest, ObserveEpochOnlyWritesTheConsumerCursor) {
    auto& ring = block.deviceToHost[0];
    Write(ring, {0x90, 0x3C});
    const uint32_t writeBefore = ring.writeIndex.load();
    block.Arm(2);
    drain.ObserveEpoch(block);
    EXPECT_EQ(ring.writeIndex.load(), writeBefore);
    EXPECT_EQ(ring.discontinuities.load(), 0u);
}

TEST_F(MidiRxDrainTest, NothingIsDrainedFromAQuiescedBlock) {
    Write(block.deviceToHost[0], {0x90, 0x3C, 0x40});
    block.Quiesce();
    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_TRUE(out.words.empty());
}

TEST_F(MidiRxDrainTest, ASteadyEpochIsNotAChange) {
    EXPECT_FALSE(drain.ObserveEpoch(block));
    EXPECT_FALSE(drain.ObserveEpoch(block));
}

TEST_F(MidiRxDrainTest, RealTimeBytesPassThroughAnOpenMessage) {
    Write(block.deviceToHost[0], {0x90, 0xF8, 0x3C, 0x40});
    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    // Clock first (it arrived first), then the Note On it interrupted.
    ASSERT_EQ(out.words.size(), 2u);
    EXPECT_EQ(out.words[0], Ump::MakeSystem(0, 0xF8, 0, 0));
    EXPECT_EQ(out.words[1], 0x2090'3C40u);
}

TEST_F(MidiRxDrainTest, ALongBacklogIsDrainedInOneCall) {
    auto& ring = block.deviceToHost[0];
    std::vector<uint8_t> run;
    for (int i = 0; i < 300; ++i) {
        run.push_back(0x90);
        run.push_back(0x3C);
        run.push_back(0x40);
    }
    ASSERT_LE(run.size(), kMidiRingCapacityBytes);
    ASSERT_TRUE(ring.TryWrite(run, 0u));
    Collected out;
    drain.DrainPort(block, 0, out.Sink());
    EXPECT_EQ(out.words.size(), 300u);
    EXPECT_TRUE(ring.Empty());
}
