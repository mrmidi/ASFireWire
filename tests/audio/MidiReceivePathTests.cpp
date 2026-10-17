// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiReceivePathTests.cpp - MPX-MIDI off the wire, through the production
// receive consumer and packet processor, into the device -> host rings.
//
// What these pin:
//   * every byte is dated from ITS OWN packet's descriptor timestamp, expanded
//     against the drain pair the consumer was handed with that packet (on the
//     midi branch the begin-hook dropped its batch and [MidiIn] t= was dead);
//   * an AM824 NO-DATA packet (FDF 0xFF) carries no MIDI even when bytes trail
//     its header (Linux amdtp-stream.c:768-770);
//   * MIDI is lifted while no CoreAudio client holds the capture buffer;
//   * the MIDI service is woken once per drain, after every packet's bytes are
//     in the rings, and not for a drain that carried none;
//   * a receive stop breaks a half-received message.

#include <gtest/gtest.h>

#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Audio/Ports/IMidiReceiveWake.hpp"
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"
#include "Isoch/Receive/IsochRxTiming.hpp"
#include "Midi/Transport/MidiRingByteSink.hpp"
#include "Midi/Transport/MidiTransportBlock.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using ASFW::AudioEngine::Direct::DirectInputWriter;
using ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer;
using ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor;
using ASFW::AudioEngine::Direct::Rx::RxMidiExtraction;
using ASFW::Midi::MidiTransportBlock;

// Two PCM slots and the MPX-MIDI slot after them, as DICE lays them out.
constexpr uint8_t kPcm = 2;
constexpr uint8_t kDbs = kPcm + 1;
constexpr ASFW::Encoding::MpxMidiGeometry kGeometry{
    .dbs = kDbs, .midiSlotIndex = kPcm, .portCount = 1, .dbcAligned = true};

uint32_t EncodeCycleTimer(uint32_t seconds, uint32_t cycle, uint32_t offset) {
    return (seconds << ASFW::Timing::kCycleTimerSecondsShift) |
           (cycle << ASFW::Timing::kCycleTimerCyclesShift) | offset;
}

void WriteBE32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value >> 24);
    dest[1] = static_cast<uint8_t>(value >> 16);
    dest[2] = static_cast<uint8_t>(value >> 8);
    dest[3] = static_cast<uint8_t>(value);
}

/// OHCI receive timestamp: seconds[15:13] cycle[12:0], little-endian in the
/// first two bytes of the buffer-fill prefix.
uint16_t ReceiveStamp(uint32_t seconds, uint32_t cycle) {
    return static_cast<uint16_t>(((seconds & 0x7u) << 13) | (cycle & 0x1FFFu));
}

/// One AM824 packet: isoch prefix, CIP header, `blocks` data blocks whose MIDI
/// quadlet is `midiQuadlets[i]` (0x80000000 = empty).
std::vector<uint8_t> MakePacket(uint16_t stamp, uint8_t dbc,
                                const std::vector<uint32_t>& midiQuadlets,
                                uint8_t fdf = 0x02, uint16_t syt = 0x1234) {
    std::vector<uint8_t> packet(8 + 8 + midiQuadlets.size() * kDbs * 4u, 0);
    packet[0] = static_cast<uint8_t>(stamp & 0xFF);
    packet[1] = static_cast<uint8_t>(stamp >> 8);
    WriteBE32(packet.data() + 8, (static_cast<uint32_t>(kDbs) << 16) | dbc);
    WriteBE32(packet.data() + 12, (1u << 31) | (0x10u << 24) |
                                      (static_cast<uint32_t>(fdf) << 16) | syt);
    for (size_t b = 0; b < midiQuadlets.size(); ++b) {
        uint8_t* block = packet.data() + 16 + b * kDbs * 4u;
        WriteBE32(block, 0x40000000u);
        WriteBE32(block + 4, 0x40000000u);
        WriteBE32(block + kPcm * 4u, midiQuadlets[b]);
    }
    return packet;
}

constexpr uint32_t MidiByte(uint8_t value) { return 0x81000000u | (uint32_t{value} << 16); }
constexpr uint32_t kEmpty = 0x80000000u;

struct CountingWake final : ASFW::Audio::Ports::IMidiReceiveWake {
    uint32_t wakes{0};
    uint64_t lastEndpoint{0};
    void MidiReceived(uint64_t endpointId) noexcept override {
        ++wakes;
        lastEndpoint = endpointId;
    }
};

/// The endpoint's view as the consumer gets it: no audio input buffer at all
/// (no CoreAudio client), only the MIDI attachment.
class MidiOnlyBindingSource final : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
public:
    MidiOnlyBindingSource(MidiTransportBlock* block, ASFW::Audio::Ports::IMidiReceiveWake* wake) {
        snapshot_.generation = 1;
        snapshot_.valid = true;
        snapshot_.midiRx.generation = 1;
        snapshot_.midiRx.endpointId = 0xABCD;
        snapshot_.midiRx.block = block;
        snapshot_.midiRx.geometry = kGeometry;
        snapshot_.midiRx.wake = wake;
    }
    bool CopyDirectAudioBinding(
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out = snapshot_;
        return true;
    }

private:
    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot_{};
};

// The host-time conversion reads the process timebase; pin it to 1:1 so a
// packet's age in nanoseconds is its age in host ticks.
struct TimebaseGuard {
    mach_timebase_info_data_t saved = ASFW::Timing::gHostTimebaseInfo;
    TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = {1, 1}; }
    ~TimebaseGuard() { ASFW::Timing::gHostTimebaseInfo = saved; }
};

struct ConsumerRig {
    TimebaseGuard timebase;
    std::unique_ptr<MidiTransportBlock> block = std::make_unique<MidiTransportBlock>();
    CountingWake wake;
    MidiOnlyBindingSource source{block.get(), &wake};
    DirectAudioReceiveConsumer consumer{&source, {.am824Slots = kDbs, .streamChannels = kPcm}};
    ASFW::Isoch::IsochReceiveBatch batch{
        .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
        .drainHostTicks = 50'000'000,
    };

    ConsumerRig() {
        block->Arm(1);
        consumer.OnReceiveActivated();
    }

    void Feed(const std::vector<uint8_t>& packet) {
        const ASFW::Isoch::IsochReceivePacket isoch{.descriptorIndex = 0,
                                                    .payload = {packet.data(), packet.size()}};
        consumer.ConsumePacket(batch, isoch);
    }
};

} // namespace

TEST(MidiReceivePathTests, EveryByteIsDatedFromItsOwnPacket) {
    ConsumerRig rig;
    // Two packets of one drain, received at cycles 290 and 295 of the drain's
    // second; the drain read its cycle timer at cycle 300.
    const uint16_t first = ReceiveStamp(13, 290);
    const uint16_t second = ReceiveStamp(13, 295);
    rig.consumer.BeginReceiveBatch(rig.batch);
    rig.Feed(MakePacket(first, /*dbc=*/0, {MidiByte(0x90), kEmpty}));
    rig.Feed(MakePacket(second, /*dbc=*/8, {MidiByte(0x3C), kEmpty}));
    rig.consumer.EndReceiveBatch(rig.batch);

    std::array<uint8_t, 4> bytes{};
    std::array<uint64_t, 4> stamps{};
    ASSERT_EQ(rig.block->deviceToHost[0].Peek(bytes, stamps), 2u);
    EXPECT_EQ(bytes[0], 0x90);
    EXPECT_EQ(bytes[1], 0x3C);

    const uint64_t expectFirst = ASFW::Isoch::Rx::PacketHostTicks(
        first, rig.batch.drainCycleTimer, rig.batch.drainHostTicks);
    const uint64_t expectSecond = ASFW::Isoch::Rx::PacketHostTicks(
        second, rig.batch.drainCycleTimer, rig.batch.drainHostTicks);
    EXPECT_EQ(stamps[0], expectFirst);
    EXPECT_EQ(stamps[1], expectSecond);
    // Not the drain instant and not zero: each packet is earlier than the
    // drain read, the first one more so.
    EXPECT_NE(stamps[0], 0u);
    EXPECT_LT(stamps[0], stamps[1]);
    EXPECT_LT(stamps[1], rig.batch.drainHostTicks);
}

TEST(MidiReceivePathTests, OneWakePerDrainAfterTheLastPacket) {
    ConsumerRig rig;
    rig.consumer.BeginReceiveBatch(rig.batch);
    rig.Feed(MakePacket(ReceiveStamp(13, 298), 0, {MidiByte(0x90), kEmpty}));
    rig.Feed(MakePacket(ReceiveStamp(13, 299), 8, {MidiByte(0x3C), kEmpty}));
    // Not from the packets: a wake there lets the MIDI queue drain before the
    // later packets' bytes are written.
    EXPECT_EQ(rig.wake.wakes, 0u);
    rig.consumer.EndReceiveBatch(rig.batch);
    EXPECT_EQ(rig.wake.wakes, 1u);
    EXPECT_EQ(rig.wake.lastEndpoint, 0xABCDu);

    // A drain that carried only empty MIDI quadlets wakes nobody.
    rig.consumer.BeginReceiveBatch(rig.batch);
    rig.Feed(MakePacket(ReceiveStamp(13, 299), 16, {kEmpty, kEmpty}));
    rig.consumer.EndReceiveBatch(rig.batch);
    EXPECT_EQ(rig.wake.wakes, 1u);
}

TEST(MidiReceivePathTests, MidiFlowsWithNoCaptureBufferBound) {
    ConsumerRig rig;
    rig.consumer.BeginReceiveBatch(rig.batch);
    rig.Feed(MakePacket(ReceiveStamp(13, 299), 0, {MidiByte(0xF8), kEmpty}));
    rig.consumer.EndReceiveBatch(rig.batch);
    EXPECT_TRUE(rig.consumer.MidiBound());
    EXPECT_EQ(rig.block->deviceToHost[0].Available(), 1u);
}

TEST(MidiReceivePathTests, AReceiveStopBreaksAHalfReceivedMessage) {
    ConsumerRig rig;
    rig.consumer.BeginReceiveBatch(rig.batch);
    rig.Feed(MakePacket(ReceiveStamp(13, 299), 0, {MidiByte(0x90), kEmpty}));
    rig.consumer.EndReceiveBatch(rig.batch);
    const uint64_t gapsBefore = rig.block->deviceToHost[0].discontinuities.load();

    rig.consumer.OnReceiveQuiesced();
    EXPECT_GT(rig.block->deviceToHost[0].discontinuities.load(), gapsBefore);
    EXPECT_FALSE(rig.consumer.MidiBound());
}

TEST(MidiReceivePathTests, Am824NoDataCarriesNoMidiEvenWithTrailingBytes) {
    auto block = std::make_unique<MidiTransportBlock>();
    block->Arm(1);
    ASFW::Midi::MidiRingByteSink sink;
    sink.Bind(block.get(), 1);
    ASFW::Encoding::MpxMidiDemuxCounters counters{};
    const RxMidiExtraction midi{.sink = &sink, .geometry = kGeometry, .counters = &counters,
                                .drainCycleTimer = EncodeCycleTimer(13, 300, 0),
                                .drainHostTicks = 50'000'000};

    DirectInputWriter writer;  // unbound: no CoreAudio client
    RxAudioPacketProcessor processor(writer);
    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDbs);
    const auto packet = MakePacket(ReceiveStamp(13, 299), 0, {MidiByte(0x90), kEmpty},
                                   /*fdf=*/0xFF, /*syt=*/0xFFFF);
    const auto result = processor.ProcessPacket(packet.data(), packet.size(), 0, kPcm, codec,
                                                0, true, {}, false,
                                                ASFW::Encoding::AudioPacketFraming::kCip, midi);
    EXPECT_EQ(result.midiBytesDelivered, 0u);
    EXPECT_FALSE(result.midiGap);  // NO-DATA is a normal packet, not a loss
    EXPECT_TRUE(block->deviceToHost[0].Empty());
    EXPECT_EQ(block->deviceToHost[0].discontinuities.load(), 0u);
}

TEST(MidiReceivePathTests, APacketOfTheWrongShapeIsAGapNotBytes) {
    auto block = std::make_unique<MidiTransportBlock>();
    block->Arm(1);
    ASFW::Midi::MidiRingByteSink sink;
    sink.Bind(block.get(), 1);
    // The device sends DBS 3; the MIDI slot was projected for DBS 4.
    const RxMidiExtraction midi{
        .sink = &sink,
        .geometry = {.dbs = 4, .midiSlotIndex = 3, .portCount = 1, .dbcAligned = true}};

    DirectInputWriter writer;
    RxAudioPacketProcessor processor(writer);
    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDbs);
    const auto packet = MakePacket(ReceiveStamp(13, 299), 0, {MidiByte(0x90), kEmpty});
    const auto result = processor.ProcessPacket(packet.data(), packet.size(), 0, kPcm, codec,
                                                0, true, {}, false,
                                                ASFW::Encoding::AudioPacketFraming::kCip, midi);
    EXPECT_EQ(result.midiBytesDelivered, 0u);
    EXPECT_TRUE(result.midiGap);
    EXPECT_TRUE(block->deviceToHost[0].Empty());
    EXPECT_EQ(block->deviceToHost[0].discontinuities.load(), 1u);
}
