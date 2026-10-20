// MpxMidiTxTests.cpp
// ASFW - WP-8 transmit-side composition tests
//
// Expected quadlets are written by hand against the AM824 layout, and read
// back with the receive demux only where a round trip is the claim being made.

#include <gtest/gtest.h>

#include <vector>

#include "Audio/Wire/AM824/MpxMidiMux.hpp"
#include "Audio/Wire/AM824/MpxMidiRateLimiter.hpp"

using namespace ASFW::Encoding;

namespace {

MpxMidiGeometry Geometry(uint8_t dbs = 9, uint8_t slot = 8, uint8_t ports = 8,
                         bool aligned = true) {
    return MpxMidiGeometry{dbs, slot, ports, aligned};
}

/// A packet image whose MIDI slots start as the armed empty quadlet, and whose
/// PCM slots hold a marker so an overwrite is visible.
std::vector<uint8_t> MakeArmedPacket(uint32_t blocks, const MpxMidiGeometry& g) {
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * g.dbs * 4, 0x40);
    for (uint32_t f = 0; f < blocks; ++f) {
        uint8_t* slot = out.data() + (static_cast<size_t>(f) * g.dbs + g.midiSlotIndex) * 4;
        slot[0] = 0x80; slot[1] = 0; slot[2] = 0; slot[3] = 0;
    }
    return out;
}

uint32_t SlotWord(const std::vector<uint8_t>& packet, uint32_t f,
                  const MpxMidiGeometry& g) {
    const uint8_t* s = packet.data() + (static_cast<size_t>(f) * g.dbs + g.midiSlotIndex) * 4;
    return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) |
           (uint32_t(s[2]) << 8) | uint32_t(s[3]);
}

} // namespace

//==============================================================================
// Slot composition
//==============================================================================

TEST(MpxMidiMux, WritesOneByteForItsPortAndLeavesOtherBlocksEmpty) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[0] = 0x90;
    bytes.hasByteForPort[0] = true;

    WriteMpxMidiSlot(packet.data(), 8, /*dbc=*/0, g, bytes);

    EXPECT_EQ(SlotWord(packet, 0, g), 0x8190'0000u) << "label 0x81, one byte";
    for (uint32_t f = 1; f < 8; ++f) {
        EXPECT_EQ(SlotWord(packet, f, g), kMpxMidiEmptyQuadlet) << "block " << f;
    }
}

TEST(MpxMidiMux, PlacesEachPortInItsRotationSlot) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    MpxMidiPacketBytes bytes{};
    for (uint8_t p = 0; p < 8; ++p) {
        bytes.byteForPort[p] = static_cast<uint8_t>(0x10 + p);
        bytes.hasByteForPort[p] = true;
    }
    WriteMpxMidiSlot(packet.data(), 8, /*dbc=*/0, g, bytes);
    for (uint32_t f = 0; f < 8; ++f) {
        EXPECT_EQ(SlotWord(packet, f, g),
                  0x8100'0000u | (uint32_t(0x10 + f) << 16));
    }
}

TEST(MpxMidiMux, DbcOffsetsTheRotation) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[3] = 0x7F;
    bytes.hasByteForPort[3] = true;

    WriteMpxMidiSlot(packet.data(), 8, /*dbc=*/5, g, bytes);
    // port 3 is reached at block f where (5 + f) % 8 == 3, i.e. f == 6.
    EXPECT_EQ(SlotWord(packet, 6, g), 0x817F'0000u);
    EXPECT_EQ(SlotWord(packet, 0, g), kMpxMidiEmptyQuadlet);
}

TEST(MpxMidiMux, CapsAtTheFirstEightBlocks) {
    // Linux caps MIDI to the first eight data blocks and notes some receivers
    // inspect only those. At 96 kHz a packet carries 16 blocks; the second
    // rotation must stay empty.
    const auto g = Geometry();
    auto packet = MakeArmedPacket(16, g);
    MpxMidiPacketBytes bytes{};
    for (uint8_t p = 0; p < 8; ++p) {
        bytes.byteForPort[p] = 0x40;
        bytes.hasByteForPort[p] = true;
    }
    WriteMpxMidiSlot(packet.data(), 16, 0, g, bytes);

    for (uint32_t f = 0; f < 8; ++f) {
        EXPECT_NE(SlotWord(packet, f, g), kMpxMidiEmptyQuadlet) << "block " << f;
    }
    for (uint32_t f = 8; f < 16; ++f) {
        EXPECT_EQ(SlotWord(packet, f, g), kMpxMidiEmptyQuadlet)
            << "block " << f << " is past the cap";
    }
}

TEST(MpxMidiMux, NeverTouchesAPcmSlot) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    MpxMidiPacketBytes bytes{};
    for (uint8_t p = 0; p < 8; ++p) {
        bytes.byteForPort[p] = 0x7F;
        bytes.hasByteForPort[p] = true;
    }
    WriteMpxMidiSlot(packet.data(), 8, 0, g, bytes);

    for (uint32_t f = 0; f < 8; ++f) {
        for (uint32_t slot = 0; slot < g.dbs; ++slot) {
            if (slot == g.midiSlotIndex) continue;
            const uint8_t* s = packet.data() + (f * g.dbs + slot) * 4;
            EXPECT_EQ(s[0], 0x40) << "block " << f << " slot " << slot
                                  << " -- writing MIDI must not reach PCM";
        }
    }
}

TEST(MpxMidiMux, PortsBeyondTheDeviceGetNoByte) {
    const auto g = Geometry(9, 8, /*ports=*/1);
    auto packet = MakeArmedPacket(8, g);
    MpxMidiPacketBytes bytes{};
    for (uint8_t p = 0; p < 8; ++p) {
        bytes.byteForPort[p] = 0x55;
        bytes.hasByteForPort[p] = true;
    }
    WriteMpxMidiSlot(packet.data(), 8, 0, g, bytes);
    EXPECT_EQ(SlotWord(packet, 0, g), 0x8155'0000u);
    for (uint32_t f = 1; f < 8; ++f) {
        EXPECT_EQ(SlotWord(packet, f, g), kMpxMidiEmptyQuadlet);
    }
}

TEST(MpxMidiMux, AnIdlePacketKeepsTheArmedEmptyQuadlets) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    const auto before = packet;
    WriteMpxMidiSlot(packet.data(), 8, 0, g, MpxMidiPacketBytes{});
    EXPECT_EQ(packet, before)
        << "an armed packet already carries valid 'no MIDI this block'";
}

TEST(MpxMidiMux, InvalidGeometryWritesNothing) {
    const auto g = Geometry();
    auto packet = MakeArmedPacket(8, g);
    const auto before = packet;
    MpxMidiPacketBytes bytes{};
    bytes.byteForPort[0] = 0x90;
    bytes.hasByteForPort[0] = true;
    // Slot index outside the data block would corrupt the next block's slot 0.
    WriteMpxMidiSlot(packet.data(), 8, 0, Geometry(9, 9), bytes);
    EXPECT_EQ(packet, before);
}

TEST(MpxMidiMux, PortForBlockMatchesTheDemuxRotation) {
    for (uint32_t dbc = 0; dbc < 256; dbc += 37) {
        for (uint32_t f = 0; f < 8; ++f) {
            EXPECT_EQ(MpxMidiPortForBlock(static_cast<uint8_t>(dbc), f, true),
                      (dbc + f) % 8);
            EXPECT_EQ(MpxMidiPortForBlock(static_cast<uint8_t>(dbc), f, false),
                      f % 8);
        }
    }
}

//==============================================================================
// Rate limiter
//==============================================================================

TEST(MpxMidiRateLimiter, AnIdlePortMayEmitImmediately) {
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    EXPECT_TRUE(limiter.AgeAndMayEmit(0));
}

TEST(MpxMidiRateLimiter, ThrottlesBelowTheSlotRate) {
    // 48 kHz blocking gives one opportunity per DATA packet, 6000/s. MIDI 1.0
    // is 3125 B/s, so roughly every second opportunity must be refused.
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);

    uint32_t emitted = 0;
    constexpr uint32_t kOpportunities = 6000;   // about one second
    for (uint32_t i = 0; i < kOpportunities; ++i) {
        if (limiter.AgeAndMayEmit(0)) {
            limiter.Debit(0);
            ++emitted;
        }
    }
    // Against the modelled 3093 B/s, with margin for the discrete accumulator.
    EXPECT_GT(emitted, 2900u);
    EXPECT_LT(emitted, 3300u)
        << "must stay under the UART rate, not the slot rate";
}

TEST(MpxMidiRateLimiter, PortsAreIndependent) {
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    ASSERT_TRUE(limiter.AgeAndMayEmit(0));
    limiter.Debit(0);
    // Port 1 has spent nothing and must not be throttled by port 0.
    EXPECT_TRUE(limiter.AgeAndMayEmit(1));
}

TEST(MpxMidiRateLimiter, RollbackReturnsTheDebitOnly) {
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    ASSERT_TRUE(limiter.AgeAndMayEmit(0));
    limiter.Debit(0);
    const uint32_t charged = limiter.UsedFor(0);
    ASSERT_GT(charged, 0u);

    limiter.RollbackDebit(0);
    EXPECT_EQ(limiter.UsedFor(0), 0u);
    EXPECT_TRUE(limiter.AgeAndMayEmit(0)) << "the returned byte may be spent again";
}

TEST(MpxMidiRateLimiter, AgingIsNotUndoneByRollback) {
    // A cancelled packet still went out; the device still clocked for that
    // long. Undoing elapsed time would let a run of cancellations burst.
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    ASSERT_TRUE(limiter.AgeAndMayEmit(0));
    limiter.Debit(0);
    const uint32_t afterDebit = limiter.UsedFor(0);

    (void)limiter.AgeAndMayEmit(0);      // one packet elapses
    const uint32_t afterAging = limiter.UsedFor(0);
    ASSERT_LT(afterAging, afterDebit);

    limiter.RollbackDebit(0);
    EXPECT_LT(limiter.UsedFor(0), afterAging)
        << "rollback returns the byte, it does not rewind the clock";
}

TEST(MpxMidiRateLimiter, HigherRatesDrainProportionally) {
    // The SYT interval doubles with the rate, so the per-packet drain doubles
    // too and the achievable byte rate stays near the UART rate.
    MpxMidiRateLimiter fast;
    fast.Configure(96000, 16);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < 6000; ++i) {
        if (fast.AgeAndMayEmit(0)) { fast.Debit(0); ++emitted; }
    }
    EXPECT_GT(emitted, 2900u);
    EXPECT_LT(emitted, 3300u);
}

TEST(MpxMidiRateLimiter, ResetClearsEveryPort) {
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    for (uint32_t p = 0; p < MpxMidiRateLimiter::kPorts; ++p) {
        ASSERT_TRUE(limiter.AgeAndMayEmit(p));
        limiter.Debit(p);
    }
    limiter.Reset();
    for (uint32_t p = 0; p < MpxMidiRateLimiter::kPorts; ++p) {
        EXPECT_EQ(limiter.UsedFor(p), 0u);
        EXPECT_TRUE(limiter.AgeAndMayEmit(p));
    }
}

TEST(MpxMidiRateLimiter, UnconfiguredEmitsNothing) {
    MpxMidiRateLimiter limiter;
    EXPECT_FALSE(limiter.Configured());
    EXPECT_FALSE(limiter.AgeAndMayEmit(0));
}

TEST(MpxMidiRateLimiter, OutOfRangePortsAreRefused) {
    MpxMidiRateLimiter limiter;
    limiter.Configure(48000, 8);
    EXPECT_FALSE(limiter.AgeAndMayEmit(MpxMidiRateLimiter::kPorts));
    limiter.Debit(MpxMidiRateLimiter::kPorts);        // must not corrupt memory
    limiter.RollbackDebit(MpxMidiRateLimiter::kPorts);
    EXPECT_EQ(limiter.UsedFor(MpxMidiRateLimiter::kPorts), 0u);
}
