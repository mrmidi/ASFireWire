#include "TestHarness.hpp"

#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpTxPacketizer.hpp"

namespace ASFW::LabTests {

using namespace Protocols::Audio::AMDTP;

namespace {

AmdtpStreamConfig MakeConfig48kBlocking(uint8_t sid, uint8_t pcmChannels,
                                        uint8_t midiSlots) {
    AmdtpStreamConfig config{};
    config.sampleRate = 48000;
    config.streamMode = StreamMode::Blocking;
    config.sid = sid;
    config.dbs = 0; // derived
    config.pcmChannels = pcmChannels;
    config.midiSlots = midiSlots;
    config.fmt = 0x10;
    config.fdf = 0x02;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 512;
    return config;
}

struct PacketizerFixture final {
    static constexpr uint32_t kSlots = 16;
    static constexpr uint32_t kCapacity = 512;

    AmdtpTxPacketizer packetizer{};
    AmdtpPacketTimeline timeline{};
    PacketTimelineSlot timelineSlots[kSlots]{};
    uint8_t bytes[kSlots][kCapacity]{};

    bool Setup(const AmdtpStreamConfig& config, const AmdtpTxPolicy& policy) {
        if (!timeline.AttachSlots(timelineSlots, kSlots)) {
            return false;
        }
        packetizer.BindTimeline(&timeline);
        return packetizer.Configure(config, policy);
    }

    TxPacketSlotView Slot(uint32_t packetIndex) {
        return TxPacketSlotView{packetIndex, bytes[packetIndex % kSlots],
                                kCapacity};
    }
};

} // namespace

void RunPacketizerTests(TestContext& ctx) {
    const AmdtpTimingState kNoClock{}; // txClockValid = false

    // --- Configure validation ---
    {
        PacketizerFixture f;
        AmdtpStreamConfig bad = MakeConfig48kBlocking(0, 2, 0);
        bad.sampleRate = 44100;
        CHECK(ctx, !f.Setup(bad, AmdtpTxPolicy{}));

        AmdtpStreamConfig noSlots = MakeConfig48kBlocking(0, 0, 0);
        CHECK(ctx, !f.packetizer.Configure(noSlots, AmdtpTxPolicy{}));

        AmdtpStreamConfig good = MakeConfig48kBlocking(0, 2, 0);
        CHECK(ctx, f.packetizer.Configure(good, AmdtpTxPolicy{}));
        CHECK_EQ_U32(ctx, f.packetizer.StreamConfig().dbs, 2); // derived

        AmdtpStreamConfig tooBig = MakeConfig48kBlocking(0, 2, 0);
        tooBig.maxPacketBytes = 71; // data packet needs 72
        CHECK(ctx, !f.packetizer.Configure(tooBig, AmdtpTxPolicy{}));
    }

    // --- Golden packet images: 48k blocking stereo, sid=1 ---
    {
        PacketizerFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(1, 2, 0), AmdtpTxPolicy{}));

        // Cycle 0 is no-data (N,D,D,D): 8 bytes, dbc=0 carried unchanged.
        PreparedTxPacket packet{};
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(0), kNoClock, packet));
        CHECK(ctx, !packet.isData);
        CHECK_EQ_U32(ctx, packet.byteCount, 8);
        CHECK_EQ_U32(ctx, packet.dbc, 0);
        CHECK_EQ_U32(ctx, packet.syt, 0xFFFF);
        // q0 = sid<<24 | dbs<<16 | dbc = 0x01020000 ; q1 = 0x90FFFFFF
        const uint8_t expectedNoData[8] = {0x01, 0x02, 0x00, 0x00,
                                           0x90, 0xFF, 0xFF, 0xFF};
        bool noDataBytesOk = true;
        for (int i = 0; i < 8; ++i) {
            noDataBytesOk = noDataBytesOk && (f.bytes[0][i] == expectedNoData[i]);
        }
        CHECK(ctx, noDataBytesOk);

        // Cycle 1 is data: 72 bytes, dbc=0 (no-data did not advance), clock
        // invalid → syt = 0xFFFF, payload cleared to zero.
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(1), kNoClock, packet));
        CHECK(ctx, packet.isData);
        CHECK_EQ_U32(ctx, packet.byteCount, 72);
        CHECK_EQ_U32(ctx, packet.dbc, 0);
        CHECK_EQ_U64(ctx, packet.firstAudioFrame, 0);
        CHECK_EQ_U32(ctx, packet.framesInPacket, 8);
        const uint8_t expectedData[8] = {0x01, 0x02, 0x00, 0x00,
                                         0x90, 0x02, 0xFF, 0xFF};
        bool dataBytesOk = true;
        for (int i = 0; i < 8; ++i) {
            dataBytesOk = dataBytesOk && (f.bytes[1][i] == expectedData[i]);
        }
        CHECK(ctx, dataBytesOk);
        bool payloadZero = true;
        for (int i = 8; i < 72; ++i) {
            payloadZero = payloadZero && (f.bytes[1][i] == 0);
        }
        CHECK(ctx, payloadZero);

        // Cycle 2 data: dbc advanced to 8; valid clock stamps the SYT.
        AmdtpTimingState clock{};
        clock.txClockValid = true;
        clock.nextDataSyt = 0x1234;
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(2), clock, packet));
        CHECK_EQ_U32(ctx, packet.dbc, 8);
        CHECK_EQ_U32(ctx, packet.syt, 0x1234);
        CHECK_EQ_U64(ctx, packet.firstAudioFrame, 8);
        CHECK(ctx, f.bytes[2][3] == 0x08); // dbc in q0[7:0]
        CHECK(ctx, f.bytes[2][6] == 0x12 && f.bytes[2][7] == 0x34);

        // Timeline integration: frames resolve to the right packets,
        // no-data position retired.
        CHECK(ctx, f.timeline.FindSlotForAudioFrame(0) != nullptr);
        CHECK_EQ_U32(ctx, f.timeline.FindSlotForAudioFrame(0)->packetIndex, 1);
        CHECK_EQ_U32(ctx, f.timeline.FindSlotForAudioFrame(15)->packetIndex, 2);
        CHECK(ctx, f.timeline.SlotByIndex(0) != nullptr);
        CHECK(ctx, !f.timeline.SlotByIndex(0)->isData);
    }

    // --- Non-audio slot defaults: dbs=3 (2 PCM + 1 MIDI) ---
    {
        PacketizerFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(0, 2, 1), AmdtpTxPolicy{}));
        CHECK_EQ_U32(ctx, f.packetizer.StreamConfig().dbs, 3);

        PreparedTxPacket packet{};
        // Skip the leading no-data cycle.
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(0), kNoClock, packet));
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(1), kNoClock, packet));
        CHECK(ctx, packet.isData);
        CHECK_EQ_U32(ctx, packet.byteCount, 8 + 8 * 3 * 4);

        // Every frame: PCM slots zeroed, slot 2 = 0x80000000 BE.
        bool defaultsOk = true;
        for (uint32_t frame = 0; frame < 8; ++frame) {
            const uint8_t* fp = &f.bytes[1][8 + frame * 12];
            for (int i = 0; i < 8; ++i) {
                defaultsOk = defaultsOk && (fp[i] == 0);
            }
            defaultsOk = defaultsOk && fp[8] == 0x80 && fp[9] == 0 &&
                         fp[10] == 0 && fp[11] == 0;
        }
        CHECK(ctx, defaultsOk);
    }

    // --- Failure paths mutate no state ---
    {
        PacketizerFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(0, 2, 0), AmdtpTxPolicy{}));

        PreparedTxPacket packet{};
        // Null bytes
        CHECK(ctx, !f.packetizer.PrepareNextPacket(
                       TxPacketSlotView{0, nullptr, 512}, kNoClock, packet));
        // Undersized slot (cycle 0 is no-data needing 8; force data first)
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(0), kNoClock, packet));
        CHECK(ctx, !f.packetizer.PrepareNextPacket(
                       TxPacketSlotView{1, f.bytes[1], 71}, kNoClock, packet));
        // Retry with a good slot: still the same cycle (data) and dbc 0.
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(1), kNoClock, packet));
        CHECK(ctx, packet.isData);
        CHECK_EQ_U32(ctx, packet.dbc, 0);

        // Unconfigured / unbound packetizer refuses
        AmdtpTxPacketizer bare;
        CHECK(ctx, !bare.PrepareNextPacket(f.Slot(2), kNoClock, packet));
    }

    // --- Reset seeds DBC and frame origin ---
    {
        PacketizerFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(0, 2, 0), AmdtpTxPolicy{}));
        f.packetizer.Reset(0xF0, 1000);

        PreparedTxPacket packet{};
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(0), kNoClock, packet));
        CHECK_EQ_U32(ctx, packet.dbc, 0xF0); // no-data carries seed unchanged
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(1), kNoClock, packet));
        CHECK(ctx, packet.isData);
        CHECK_EQ_U32(ctx, packet.dbc, 0xF0);
        CHECK_EQ_U64(ctx, packet.firstAudioFrame, 1000);
    }

    // --- Long run: P1 cadence ratio + P2 DBC continuity + P4 frame tiling ---
    {
        PacketizerFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(0, 2, 0), AmdtpTxPolicy{}));

        uint32_t dataPackets = 0;
        uint64_t expectedNextFrame = 0;
        uint8_t expectedDbc = 0;
        bool continuityOk = true;

        constexpr uint32_t kCycles = 1000000;
        for (uint32_t cycle = 0; cycle < kCycles; ++cycle) {
            PreparedTxPacket packet{};
            if (!f.packetizer.PrepareNextPacket(f.Slot(cycle), kNoClock,
                                                packet)) {
                continuityOk = false;
                break;
            }
            if (packet.dbc != expectedDbc) {
                continuityOk = false;
                break;
            }
            if (packet.isData) {
                if (packet.firstAudioFrame != expectedNextFrame) {
                    continuityOk = false;
                    break;
                }
                expectedNextFrame += packet.framesInPacket;
                expectedDbc =
                    static_cast<uint8_t>(expectedDbc + packet.framesInPacket);
                ++dataPackets;
            }
        }
        CHECK(ctx, continuityOk);
        CHECK_EQ_U32(ctx, dataPackets, kCycles * 3 / 4);
        CHECK_EQ_U64(ctx, expectedNextFrame, static_cast<uint64_t>(kCycles) * 6);
    }

    // --- Non-blocking 48k: every cycle data, 6 frames, dbc += 6 ---
    {
        PacketizerFixture f;
        AmdtpStreamConfig config = MakeConfig48kBlocking(0, 2, 0);
        config.streamMode = StreamMode::NonBlocking;
        CHECK(ctx, f.Setup(config, AmdtpTxPolicy{}));

        PreparedTxPacket packet{};
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(0), kNoClock, packet));
        CHECK(ctx, packet.isData);
        CHECK_EQ_U32(ctx, packet.framesInPacket, 6);
        CHECK_EQ_U32(ctx, packet.byteCount, 8 + 6 * 2 * 4);
        CHECK_EQ_U32(ctx, packet.dbc, 0);
        CHECK(ctx, f.packetizer.PrepareNextPacket(f.Slot(1), kNoClock, packet));
        CHECK_EQ_U32(ctx, packet.dbc, 6);
    }
}

} // namespace ASFW::LabTests
