// MidiTxIngressTests.cpp
// The host -> device producer: whole callbacks, no half messages, bounded
// recovery. Covers reviews/midi-2026-09-13 findings 5 and 6.

#include <gtest/gtest.h>

#include <vector>

#include "Midi/Transport/MidiTxIngress.hpp"

using namespace ASFW::Midi;
using ASFW::Midi::Ump::UmpWord;

namespace {

std::vector<uint8_t> Drain(MidiByteRing& ring) {
    std::vector<uint8_t> all;
    uint8_t buffer[256];
    while (true) {
        const uint32_t n = ring.Peek(buffer);
        if (n == 0) break;
        all.insert(all.end(), buffer, buffer + n);
        ring.Consume(n);
    }
    return all;
}

class MidiTxIngressTest : public ::testing::Test {
protected:
    void SetUp() override { block.Arm(1); }
    MidiTransportBlock block;
    MidiTxIngress ingress;
};

} // namespace

TEST_F(MidiTxIngressTest, QueuesAWholeNoteOn) {
    const std::vector<UmpWord> words{0x2092'3C40u};
    EXPECT_EQ(ingress.Submit(block, 3, words, 77), MidiTxStatus::kOk);
    EXPECT_EQ(Drain(block.hostToDevice[3]), (std::vector<uint8_t>{0x92, 0x3C, 0x40}));
}

TEST_F(MidiTxIngressTest, EveryMessageOfALargeCallbackIsQueued) {
    // 256 Note Ons = 768 bytes, which fits an empty ring. The reviewed callback
    // converted 169 of them against a 512-byte scratch and returned success.
    std::vector<UmpWord> words(256, 0x2090'3C40u);
    ASSERT_EQ(ingress.Submit(block, 0, words, 1), MidiTxStatus::kOk);
    const auto bytes = Drain(block.hostToDevice[0]);
    ASSERT_EQ(bytes.size(), 768u);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        EXPECT_EQ(bytes[i], 0x90);
        EXPECT_EQ(bytes[i + 1], 0x3C);
        EXPECT_EQ(bytes[i + 2], 0x40);
    }
}

TEST_F(MidiTxIngressTest, ACallbackLargerThanTheScratchIsStillWhole) {
    // 400 Note Ons = 1200 bytes: more than one chunk and more than one ring.
    // It cannot fit, so it must FAIL, not claim success for a prefix.
    std::vector<UmpWord> words(400, 0x2090'3C40u);
    EXPECT_EQ(ingress.Submit(block, 0, words, 1), MidiTxStatus::kRingFull);
    EXPECT_EQ(ingress.Counters(0).droppedRingFull, 1u);
}

TEST_F(MidiTxIngressTest, ATruncatedPacketIsAnErrorNotASilentSuccess) {
    // A SysEx7 packet is two words; supply one.
    const std::vector<UmpWord> words{0x3003'4110u};
    EXPECT_EQ(ingress.Submit(block, 0, words, 1), MidiTxStatus::kTruncated);
    EXPECT_EQ(ingress.Counters(0).truncatedInputs, 1u);
    EXPECT_TRUE(block.hostToDevice[0].Empty());
}

TEST_F(MidiTxIngressTest, ARejectedSysExStartDoesNotLetItsEndReachTheWireBare) {
    auto& ring = block.hostToDevice[0];
    // Fill the ring so the SysEx start is refused.
    std::vector<uint8_t> filler(kMidiRingCapacityBytes, 0xF8);
    ASSERT_TRUE(ring.TryWrite(filler, 0u));

    const std::vector<UmpWord> start{0x3011'0100u, 0x0000'0000u};   // Start, byte 01
    EXPECT_EQ(ingress.Submit(block, 0, start, 1), MidiTxStatus::kRingFull);
    ASSERT_EQ(Drain(ring).size(), kMidiRingCapacityBytes);          // ring empty again

    const std::vector<UmpWord> end{0x3031'0200u, 0x0000'0000u};     // End, byte 02
    EXPECT_EQ(ingress.Submit(block, 0, end, 2), MidiTxStatus::kOk);
    EXPECT_TRUE(Drain(ring).empty())
        << "the reviewed code emitted '02 F7': payload and EOX with no F0, read by "
           "the device under its previous running status";
}

TEST_F(MidiTxIngressTest, ARejectedEndAfterAnAcceptedStartOwesTheDeviceAnEox) {
    auto& ring = block.hostToDevice[0];
    const std::vector<UmpWord> start{0x3011'0100u, 0x0000'0000u};
    ASSERT_EQ(ingress.Submit(block, 0, start, 1), MidiTxStatus::kOk);
    ASSERT_EQ(Drain(ring), (std::vector<uint8_t>{0xF0, 0x01}));

    std::vector<uint8_t> filler(kMidiRingCapacityBytes, 0xF8);
    ASSERT_TRUE(ring.TryWrite(filler, 0u));
    const std::vector<UmpWord> end{0x3031'0200u, 0x0000'0000u};
    EXPECT_EQ(ingress.Submit(block, 0, end, 2), MidiTxStatus::kRingFull);
    EXPECT_TRUE(ingress.OwesTerminator(0));
    ASSERT_EQ(Drain(ring).size(), kMidiRingCapacityBytes);

    // The next accepted callback closes the device's SysEx first, so the Note
    // On is not swallowed into the old message's payload.
    const std::vector<UmpWord> noteOn{0x2090'3C40u};
    EXPECT_EQ(ingress.Submit(block, 0, noteOn, 3), MidiTxStatus::kOk);
    EXPECT_FALSE(ingress.OwesTerminator(0));
    EXPECT_EQ(Drain(ring), (std::vector<uint8_t>{0xF7, 0x90, 0x3C, 0x40}));
    EXPECT_EQ(ingress.Counters(0).sysExTerminatorsSent, 1u);
}

TEST_F(MidiTxIngressTest, ATerminatorThatCannotBeQueuedYetIsStillOwed) {
    auto& ring = block.hostToDevice[0];
    ASSERT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x3011'0100u, 0}, 1),
              MidiTxStatus::kOk);
    Drain(ring);
    std::vector<uint8_t> filler(kMidiRingCapacityBytes, 0xF8);
    ASSERT_TRUE(ring.TryWrite(filler, 0u));
    ASSERT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x3031'0200u, 0}, 2),
              MidiTxStatus::kRingFull);
    ASSERT_TRUE(ingress.OwesTerminator(0));

    // Ring still full: the debt survives.
    EXPECT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x2090'3C40u}, 3),
              MidiTxStatus::kRingFull);
    EXPECT_TRUE(ingress.OwesTerminator(0));
}

TEST_F(MidiTxIngressTest, NothingIsQueuedWhileNoStreamIsRunning) {
    block.Quiesce();
    EXPECT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x2090'3C40u}, 1),
              MidiTxStatus::kNotActive);
    EXPECT_TRUE(block.hostToDevice[0].Empty());
    EXPECT_EQ(ingress.Counters(0).droppedNotActive, 1u);

    // And a later stream does not inherit it.
    block.Arm(2);
    EXPECT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x2090'3E40u}, 2),
              MidiTxStatus::kOk);
    EXPECT_EQ(Drain(block.hostToDevice[0]), (std::vector<uint8_t>{0x90, 0x3E, 0x40}));
}

TEST_F(MidiTxIngressTest, AStreamRestartResetsOnlyThatPortsConverter) {
    ASSERT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x3011'0100u, 0}, 1),
              MidiTxStatus::kOk);
    ASSERT_EQ(ingress.Submit(block, 1, std::vector<UmpWord>{0x3011'0100u, 0}, 1),
              MidiTxStatus::kOk);
    ASSERT_TRUE(ingress.Converter(0).InSysEx());
    ASSERT_TRUE(ingress.Converter(1).InSysEx());

    block.Arm(2);
    // Port 0 observes the new epoch; port 1 has not been called yet.
    ASSERT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x2090'3C40u}, 2),
              MidiTxStatus::kOk);
    EXPECT_FALSE(ingress.Converter(0).InSysEx());
    EXPECT_TRUE(ingress.Converter(1).InSysEx())
        << "port 0's thread must not have touched port 1's converter";
    EXPECT_EQ(ingress.Counters(0).epochResets, 2u);
    EXPECT_EQ(ingress.Counters(1).epochResets, 1u);
}

TEST_F(MidiTxIngressTest, BadPortIsRefused) {
    EXPECT_EQ(ingress.Submit(block, kMidiPortsPerDirection,
                             std::vector<UmpWord>{0x2090'3C40u}, 1),
              MidiTxStatus::kBadPort);
}

TEST_F(MidiTxIngressTest, EveryByteIsDatedWithTheEnqueueTime) {
    ASSERT_EQ(ingress.Submit(block, 0, std::vector<UmpWord>{0x2090'3C40u}, 12345),
              MidiTxStatus::kOk);
    uint8_t bytes[3];
    uint64_t stamps[3];
    ASSERT_EQ(block.hostToDevice[0].Peek(bytes, stamps), 3u);
    EXPECT_EQ(stamps[0], 12345u);
    EXPECT_EQ(stamps[2], 12345u);
}
