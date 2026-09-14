// MidiTransportBlockTests.cpp
// ASFW - WP-4 byte seam tests
//
// The rings live in memory mapped by more than one service, on more than one
// queue, one of which is a real-time thread. Everything asserted here is about
// what happens when a caller misbehaves or a stream restarts underneath.

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "Midi/Transport/MidiTransportBlock.hpp"
#include "Midi/Transport/MidiTxReservation.hpp"

using namespace ASFW::Midi;

namespace {

std::vector<uint8_t> Drain(MidiByteRing& ring) {
    std::vector<uint8_t> out(kMidiRingCapacityBytes);
    const uint32_t n = ring.Peek(out);
    out.resize(n);
    ring.Consume(n);
    return out;
}

} // namespace

//==============================================================================
// Ring basics
//==============================================================================

TEST(MidiByteRing, RoundTripsAShortRun) {
    MidiByteRing ring;
    const std::vector<uint8_t> in{0x90, 0x3C, 0x40};
    EXPECT_TRUE((ring.TryWrite(in, 0u)));
    EXPECT_EQ(ring.Available(), 3u);
    EXPECT_EQ(Drain(ring), in);
    EXPECT_TRUE(ring.Empty());
}

TEST(MidiByteRing, PeekDoesNotConsume) {
    MidiByteRing ring;
    ASSERT_TRUE((ring.TryWrite(std::vector<uint8_t>{1, 2, 3}, 0u)));
    uint8_t scratch[3]{};
    EXPECT_EQ(ring.Peek(scratch), 3u);
    EXPECT_EQ(ring.Available(), 3u) << "peek must leave the bytes queued";
    EXPECT_EQ(ring.Peek(scratch), 3u) << "and must be repeatable";
}

TEST(MidiByteRing, WrapsCorrectlyAroundTheCapacityBoundary) {
    MidiByteRing ring;
    // Push the cursors most of the way round, then straddle the wrap.
    std::vector<uint8_t> filler(kMidiRingCapacityBytes - 2, 0x7F);
    ASSERT_TRUE((ring.TryWrite(filler, 0u)));
    ASSERT_EQ(Drain(ring).size(), filler.size());

    const std::vector<uint8_t> straddling{0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE((ring.TryWrite(straddling, 0u)));
    EXPECT_EQ(Drain(ring), straddling);
}

TEST(MidiByteRing, FillsToExactlyCapacity) {
    MidiByteRing ring;
    std::vector<uint8_t> full(kMidiRingCapacityBytes);
    std::iota(full.begin(), full.end(), uint8_t{0});
    EXPECT_TRUE((ring.TryWrite(full, 0u)));
    EXPECT_EQ(ring.Available(), kMidiRingCapacityBytes);
    EXPECT_EQ(ring.FreeSpace(), 0u);
    EXPECT_EQ(Drain(ring), full);
}

//==============================================================================
// Overflow
//==============================================================================

TEST(MidiByteRing, RejectsAWholeRunRatherThanTruncatingIt) {
    MidiByteRing ring;
    std::vector<uint8_t> nearlyFull(kMidiRingCapacityBytes - 2, 0x01);
    ASSERT_TRUE((ring.TryWrite(nearlyFull, 0u)));

    // Three bytes into two bytes of space: a truncated MIDI message would
    // leave the device desynchronised until the next status byte.
    const std::vector<uint8_t> message{0x90, 0x3C, 0x40};
    EXPECT_FALSE((ring.TryWrite(message, 0u)));
    EXPECT_EQ(ring.Available(), nearlyFull.size()) << "nothing partial written";
    EXPECT_EQ(ring.droppedBytes.load(), 3u);
    EXPECT_EQ(ring.discontinuities.load(), 1u);
}

TEST(MidiByteRing, ARunLargerThanTheRingCanNeverBeAccepted) {
    MidiByteRing ring;
    const std::vector<uint8_t> huge(kMidiRingCapacityBytes + 1, 0x00);
    EXPECT_FALSE((ring.TryWrite(huge, 0u)));
    EXPECT_TRUE(ring.Empty());
    EXPECT_EQ(ring.droppedBytes.load(), huge.size());
}

TEST(MidiByteRing, RecoversAfterTheConsumerCatchesUp) {
    MidiByteRing ring;
    std::vector<uint8_t> full(kMidiRingCapacityBytes, 0x01);
    ASSERT_TRUE((ring.TryWrite(full, 0u)));
    ASSERT_FALSE((ring.TryWrite(std::vector<uint8_t>{0xF8}, 0u)));

    ASSERT_EQ(Drain(ring).size(), full.size());
    EXPECT_TRUE((ring.TryWrite(std::vector<uint8_t>{0xF8}, 0u)));
}

TEST(MidiByteRing, ConsumingMoreThanAvailableIsClamped) {
    MidiByteRing ring;
    ASSERT_TRUE((ring.TryWrite(std::vector<uint8_t>{1, 2}, 0u)));
    ring.Consume(100);
    EXPECT_TRUE(ring.Empty());
    EXPECT_EQ(ring.Available(), 0u) << "the read index must not pass the write index";
    EXPECT_EQ(ring.FreeSpace(), kMidiRingCapacityBytes);
}

TEST(MidiByteRing, ResetConsumerDropsBacklogAndMarksTheGap) {
    MidiByteRing ring;
    ASSERT_TRUE((ring.TryWrite(std::vector<uint8_t>{0x90, 0x3C}, 0u)));
    ring.ResetConsumer();
    EXPECT_TRUE(ring.Empty());
    EXPECT_EQ(ring.discontinuities.load(), 1u)
        << "a dropped partial message must be reported, not silently lost";
}

//==============================================================================
// Block lifecycle
//==============================================================================

TEST(MidiTransportBlockTest, IsUnusableBeforeItIsArmed) {
    MidiTransportBlock block;
    EXPECT_FALSE(block.Usable(0));
    EXPECT_FALSE(block.Usable(7));
}

TEST(MidiTransportBlockTest, ArmMakesItUsableForThatEpochOnly) {
    MidiTransportBlock block;
    block.Arm(7);
    EXPECT_TRUE(block.Usable(7));
    EXPECT_FALSE(block.Usable(6)) << "a stale consumer must not be able to read";
    EXPECT_FALSE(block.Usable(8));
}

TEST(MidiTransportBlockTest, ArmClearsThePreviousStreamsBytes) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90, 0x3C}, 0u)));
    ASSERT_TRUE((block.deviceToHost[3].TryWrite(std::vector<uint8_t>{0xF8}, 0u)));

    block.Arm(2);
    EXPECT_TRUE(block.hostToDevice[0].Empty());
    EXPECT_TRUE(block.deviceToHost[3].Empty());
    EXPECT_TRUE(block.Usable(2));
}

TEST(MidiTransportBlockTest, QuiesceStopsUseWithoutDestroyingTheMapping) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE(block.Usable(1));
    block.Quiesce();
    EXPECT_FALSE(block.Usable(1));
    // The rings are still addressable; only their use is refused. A mapping
    // must stay valid until every user's queue is quiescent.
    EXPECT_TRUE(block.hostToDevice[0].Empty());
}

//==============================================================================
// Reservation
//==============================================================================

TEST(MidiTxReservation, SelectsOneBytePerPortWithoutConsuming) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90, 0x3C}, 0u)));
    ASSERT_TRUE((block.hostToDevice[5].TryWrite(std::vector<uint8_t>{0xF8}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));

    uint8_t byte = 0;
    ASSERT_TRUE(reservation.Get(0, byte));
    EXPECT_EQ(byte, 0x90);
    ASSERT_TRUE(reservation.Get(5, byte));
    EXPECT_EQ(byte, 0xF8);
    EXPECT_FALSE(reservation.Get(1, byte));

    EXPECT_EQ(block.hostToDevice[0].Available(), 2u)
        << "reserving must not consume: the packet may still be cancelled";
}

TEST(MidiTxReservation, CommitRetiresExactlyTheSelectedBytes) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90, 0x3C, 0x40}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    EXPECT_TRUE(scope.Commit(block, reservation));

    EXPECT_EQ(block.hostToDevice[0].Available(), 2u) << "one byte per packet";
    EXPECT_EQ(scope.Counters().bytesCommitted, 1u);
    EXPECT_FALSE(scope.HasOutstanding());
}

TEST(MidiTxReservation, CancelReturnsTheBytesForALaterPacket) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[2].TryWrite(std::vector<uint8_t>{0xB0}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    scope.Cancel(reservation);

    EXPECT_EQ(block.hostToDevice[2].Available(), 1u)
        << "a cancelled fill must not spend the byte";
    EXPECT_EQ(scope.Counters().bytesReturned, 1u);

    // And the next packet can take it.
    MidiPacketReservation next;
    ASSERT_TRUE(scope.Begin(block, 1, 101, next));
    uint8_t byte = 0;
    ASSERT_TRUE(next.Get(2, byte));
    EXPECT_EQ(byte, 0xB0);
}

TEST(MidiTxReservation, CommittingACancelledReservationRetiresNothing) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    scope.Cancel(reservation);

    EXPECT_FALSE(scope.Commit(block, reservation))
        << "a later commit must not retire bytes a cancel already returned";
    EXPECT_EQ(block.hostToDevice[0].Available(), 1u);
}

TEST(MidiTxReservation, ASecondBeginWhileOneIsOutstandingIsRefused) {
    MidiTransportBlock block;
    block.Arm(1);
    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation first, second;
    ASSERT_TRUE(scope.Begin(block, 1, 100, first));
    EXPECT_FALSE(scope.Begin(block, 1, 101, second))
        << "replacing a live reservation would strand its bytes";
}

TEST(MidiTxReservation, CommitForADifferentPacketIsRefused) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));

    MidiPacketReservation forged = reservation;
    forged.packetIndex = 999;
    EXPECT_FALSE(scope.Commit(block, forged));
    EXPECT_EQ(scope.Counters().mismatchedCommits, 1u);
    EXPECT_EQ(block.hostToDevice[0].Available(), 1u);
}

TEST(MidiTxReservation, CommitAcrossAnEpochChangeRetiresNothing) {
    // Packet indices are reused on restart, so a reservation that survives an
    // epoch change must not be retired by a packet that merely shares its index.
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));

    block.Arm(2);   // stream restarted
    EXPECT_FALSE(scope.Commit(block, reservation));
    EXPECT_EQ(scope.Counters().staleEpochCommits, 1u);
    EXPECT_FALSE(scope.HasOutstanding());
}

TEST(MidiTxReservation, BeginAgainstTheWrongEpochSelectsNothing) {
    MidiTransportBlock block;
    block.Arm(5);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    EXPECT_FALSE(scope.Begin(block, 4, 100, reservation));
    EXPECT_FALSE(reservation.active);
    EXPECT_FALSE(reservation.Any());
}

TEST(MidiTxReservation, BeginOnAQuiescedBlockSelectsNothing) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));
    block.Quiesce();

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    EXPECT_FALSE(scope.Begin(block, 1, 100, reservation));
    EXPECT_FALSE(scope.HasOutstanding());
}

TEST(MidiTxReservation, ResetDropsTheOutstandingReservation) {
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    scope.Reset();

    EXPECT_FALSE(scope.HasOutstanding());
    EXPECT_FALSE(scope.Commit(block, reservation));
    EXPECT_EQ(block.hostToDevice[0].Available(), 1u);
}

TEST(MidiTxReservation, EmptyRingsProduceAnEmptyButValidReservation) {
    MidiTransportBlock block;
    block.Arm(1);
    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    EXPECT_FALSE(reservation.Any());
    EXPECT_TRUE(scope.Commit(block, reservation)) << "an idle packet still closes cleanly";
    EXPECT_EQ(scope.Counters().bytesCommitted, 0u);
}

TEST(MidiTxReservation, EachPortRetiresIndependently) {
    MidiTransportBlock block;
    block.Arm(1);
    for (uint32_t port = 0; port < kMidiPortsPerDirection; ++port) {
        ASSERT_TRUE(block.hostToDevice[port].TryWrite(
            std::vector<uint8_t>{static_cast<uint8_t>(0x10 + port),
                                 static_cast<uint8_t>(0x20 + port)}, 0u));
    }

    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 1, reservation));
    ASSERT_TRUE(scope.Commit(block, reservation));

    for (uint32_t port = 0; port < kMidiPortsPerDirection; ++port) {
        EXPECT_EQ(block.hostToDevice[port].Available(), 1u) << "port " << port;
        uint8_t remaining[1];
        ASSERT_EQ(block.hostToDevice[port].Peek(remaining), 1u);
        EXPECT_EQ(remaining[0], 0x20 + port);
    }
    EXPECT_EQ(scope.Counters().bytesCommitted, kMidiPortsPerDirection);
}

TEST(MidiTxReservation, AnUnconfiguredLimiterSelectsNothing) {
    // The UART model gates every selection. A scope whose limiter was never
    // configured must emit nothing rather than bypass the model -- silence is
    // recoverable, flooding a 31.25 kbaud UART is not.
    MidiTransportBlock block;
    block.Arm(1);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiTxReservationScope scope;   // deliberately not configured
    MidiPacketReservation reservation;
    ASSERT_TRUE(scope.Begin(block, 1, 100, reservation));
    EXPECT_FALSE(reservation.Any());
    EXPECT_EQ(block.hostToDevice[0].Available(), 1u);
}

TEST(MidiTxReservation, TheLimiterThrottlesAcrossSuccessivePackets) {
    MidiTransportBlock block;
    block.Arm(1);
    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);

    // Keep one port permanently supplied and run a second of packets.
    uint32_t committed = 0;
    for (uint32_t packet = 0; packet < 6000; ++packet) {
        if (block.hostToDevice[0].Available() == 0) {
            ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x7F}, 0u)));
        }
        MidiPacketReservation reservation;
        ASSERT_TRUE(scope.Begin(block, 1, packet, reservation));
        ASSERT_TRUE(scope.Commit(block, reservation));
        if (reservation.Any()) ++committed;
    }
    EXPECT_GT(committed, 2900u);
    EXPECT_LT(committed, 3300u)
        << "one opportunity per packet is 6000/s; the UART takes about 3093";
}

TEST(MidiTxReservation, CancelReturnsTheEmissionCreditToTheLimiter) {
    MidiTransportBlock block;
    block.Arm(1);
    MidiTxReservationScope scope;
    scope.ConfigureLimiter(48000, 8);
    ASSERT_TRUE((block.hostToDevice[0].TryWrite(std::vector<uint8_t>{0x90}, 0u)));

    MidiPacketReservation first;
    ASSERT_TRUE(scope.Begin(block, 1, 1, first));
    ASSERT_TRUE(first.Any());
    scope.Cancel(first);

    // The credit came back, so the very next packet may carry the byte -- a
    // cancelled fill must not cost the port its turn.
    MidiPacketReservation second;
    ASSERT_TRUE(scope.Begin(block, 1, 2, second));
    EXPECT_TRUE(second.Any());
}
