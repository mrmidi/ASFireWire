// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The transmit half of the MOTU absolute-phase measurement: what the release
// observer reads from a released packet, and what it publishes over the
// control block's motuPhaseTrace bridge.

#include "Audio/DriverKit/Runtime/MotuTxPhaseTracePublisher.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"
#include "Isoch/Core/IsochTxQueue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample;
using ASFW::Audio::Runtime::MotuTxPhaseTracePublisher;
using ASFW::Isoch::IsochTxQueueControl;
using ASFW::Protocols::Audio::AMDTP::PreparedTxPacket;

// Neutral audio execution-time interface. Hardware completion access stays
// in this host fixture, not the MOTU content publisher.
class ExecutionTimeline final : public ASFW::Protocols::Audio::AMDTP::ITxPacketExecutionTimeline {
public:
    explicit ExecutionTimeline(const IsochTxQueueControl& queue) : queue_(queue) {}
    bool AnchorForPacket(uint64_t index, int64_t& ticks) const noexcept override {
        uint64_t completed=0;uint32_t timestamp=0;
        const uint64_t count=queue_.completionStampCount.load();
        if (!count || !queue_.ReadCompletionStamp(count-1,completed,timestamp) || index<completed) return false;
        ticks=ASFW::Timing::encodedTstampToOffsets(timestamp)+
              static_cast<int64_t>(index-completed)*ASFW::Encoding::Motu::kTicksPerCycle;
        return true;
    }
private: const IsochTxQueueControl& queue_;
};

constexpr uint32_t kDbs = 19;
constexpr uint32_t kFramesPerPacket = 8;
constexpr uint32_t kDataBytes = 8 + kFramesPerPacket * kDbs * 4;
constexpr uint32_t kLead = 9216;

struct Released final {
    PreparedTxPacket packet{};
    std::vector<uint8_t> bytes;
};

// A released packet: CIP header, then the first block's SPH (big-endian).
Released Packet(uint64_t index, bool isData, uint64_t firstFrame, uint32_t sph) {
    Released out{};
    out.packet.packetIndex = index;
    out.packet.isData = isData;
    out.packet.firstAudioFrame = firstFrame;
    out.packet.framesInPacket = isData ? kFramesPerPacket : 0;
    out.packet.byteCount = isData ? kDataBytes : 8;
    out.packet.dbs = kDbs;
    out.bytes.assign(isData ? kDataBytes : 8, 0);
    if (isData) {
        out.bytes[8] = static_cast<uint8_t>(sph >> 24);
        out.bytes[9] = static_cast<uint8_t>(sph >> 16);
        out.bytes[10] = static_cast<uint8_t>(sph >> 8);
        out.bytes[11] = static_cast<uint8_t>(sph);
    }
    return out;
}

uint32_t SphOf(uint64_t index) {
    return ASFW::Encoding::Motu::SphFromTick(static_cast<uint32_t>(1000 + index * 4096));
}

Released Data(uint64_t index, uint64_t firstFrame) {
    return Packet(index, true, firstFrame, SphOf(index));
}

void Release(MotuTxPhaseTracePublisher& publisher, const Released& released) {
    publisher.OnRelease(released.packet, released.bytes);
}

struct Bridge final {
    bool published{false};
    uint64_t updates{0};
    MotuPhaseTraceCadenceSample period{};
};

Bridge Read(const AudioTransportControlBlock& control) {
    Bridge out{};
    out.published = control.motuPhaseTrace.ReadLatest(out.period, out.updates);
    return out;
}

// Every DATA packet slides into the window, and the window is published once
// per cadence period (every third DATA packet by the frame cursor), with the
// newest completion as the reference and the model's lead.
TEST(MotuTxPhaseTracePublisherTests, PublishesOneWindowPerCadencePeriodFromTheReleasedBytes) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t generation = control.generation.load();
    IsochTxQueueControl queue{};
    ExecutionTimeline timeline(queue);
    const uint32_t completion = ASFW::Timing::encodeCycleTimer(3, 1234, 0);
    queue.PushCompletionStamp(80, ASFW::Timing::encodeCycleTimer(3, 1220, 0));
    queue.PushCompletionStamp(94, completion);

    MotuTxPhaseTracePublisher publisher;
    publisher.Bind(&control, &timeline, kLead);
    ASSERT_TRUE(publisher.IsBound());

    // N,D,D,D,N,D: frames 0, 8, 16, 24 on the DATA packets.
    Release(publisher, Packet(100, false, 0, 0));
    Release(publisher, Data(101, 0));  // the gate fires on a partial window
    auto bridge = Read(control);
    ASSERT_TRUE(bridge.published);
    EXPECT_EQ(bridge.updates, 1U);
    EXPECT_EQ(bridge.period.count, 1U);

    Release(publisher, Data(102, 8));
    Release(publisher, Data(103, 16));
    Release(publisher, Packet(104, false, 24, 0));
    EXPECT_EQ(Read(control).updates, 1U);
    Release(publisher, Data(105, 24));

    bridge = Read(control);
    ASSERT_TRUE(bridge.published);
    EXPECT_EQ(bridge.updates, 2U);
    EXPECT_EQ(bridge.period.streamGeneration, generation);
    EXPECT_EQ(bridge.period.count, 3U);
    const uint64_t expectedIndex[3] = {102, 103, 105};
    for (uint32_t bucket = 0; bucket < 3; ++bucket) {
        EXPECT_EQ(bridge.period.packetIndex[bucket], expectedIndex[bucket]) << bucket;
        EXPECT_EQ(bridge.period.firstSph[bucket], SphOf(expectedIndex[bucket])) << bucket;
    }
    ASSERT_TRUE(bridge.period.hasOutputLast);
    EXPECT_EQ(bridge.period.outputLastPacketIndex, 102U);
    EXPECT_EQ(bridge.period.outputLastCycleTimer, ASFW::Timing::encodeCycleTimer(0, 1242, 0));
    EXPECT_EQ(bridge.period.presentationLeadTicks, kLead);
    EXPECT_EQ(publisher.Published(), 2U);
    EXPECT_EQ(publisher.Rejected(), 0U);
}

// Only a DATA packet with a well-formed first SPH is a presentation time.
TEST(MotuTxPhaseTracePublisherTests, ReadsOnlyDataPacketsWithAWellFormedSph) {
    uint32_t sph = 0;
    const auto good = Data(7, 0);
    ASSERT_TRUE(MotuTxPhaseTracePublisher::ReadFirstSph(good.packet, good.bytes, sph));
    EXPECT_EQ(sph, SphOf(7));

    auto noData = Data(7, 0);
    noData.packet.isData = false;
    EXPECT_FALSE(MotuTxPhaseTracePublisher::ReadFirstSph(noData.packet, noData.bytes, sph));
    auto noFrames = Data(7, 0);
    noFrames.packet.framesInPacket = 0;
    EXPECT_FALSE(MotuTxPhaseTracePublisher::ReadFirstSph(noFrames.packet, noFrames.bytes, sph));
    auto shortPacket = Data(7, 0);
    shortPacket.packet.byteCount = 11;
    EXPECT_FALSE(MotuTxPhaseTracePublisher::ReadFirstSph(shortPacket.packet, shortPacket.bytes, sph));
    auto shortSpan = Data(7, 0);
    shortSpan.bytes.resize(11);
    EXPECT_FALSE(MotuTxPhaseTracePublisher::ReadFirstSph(shortSpan.packet, shortSpan.bytes, sph));
    for (const uint32_t malformed : {0x02000000u, 8000u << 12, 0x00000c00u}) {
        SCOPED_TRACE(malformed);
        const auto bad = Packet(7, true, 0, malformed);
        EXPECT_FALSE(MotuTxPhaseTracePublisher::ReadFirstSph(bad.packet, bad.bytes, sph));
    }

    // None of them enters the window.
    AudioTransportControlBlock control{};
    control.ResetForStart();
    IsochTxQueueControl queue{};
    ExecutionTimeline timeline(queue);
    MotuTxPhaseTracePublisher publisher;
    publisher.Bind(&control, &timeline, kLead);
    for (const auto* rejected : {&noData, &noFrames, &shortPacket, &shortSpan}) {
        Release(publisher, *rejected);
    }
    Release(publisher, Packet(8, true, 0, 0x02000000u));
    EXPECT_FALSE(Read(control).published);
    Release(publisher, Data(9, 0));
    const auto bridge = Read(control);
    ASSERT_TRUE(bridge.published);
    EXPECT_EQ(bridge.period.count, 1U);
    EXPECT_EQ(bridge.period.packetIndex[2], 9U);
    // No completion yet: published without a reference, which no mean accepts.
    EXPECT_FALSE(bridge.period.hasOutputLast);
}

// The period carries the control block's live generation. Between a start's
// generation step and the re-arm of the bridge the two differ, and the bridge
// refuses the period: counted, not published.
TEST(MotuTxPhaseTracePublisherTests, StampsTheLiveGenerationAndCountsARefusedPeriod) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t generation = control.generation.load();
    IsochTxQueueControl queue{};
    ExecutionTimeline timeline(queue);
    queue.PushCompletionStamp(1, ASFW::Timing::encodeCycleTimer(0, 1, 0));
    MotuTxPhaseTracePublisher publisher;
    publisher.Bind(&control, &timeline, kLead);

    Release(publisher, Data(10, 0));
    ASSERT_EQ(Read(control).updates, 1U);

    control.generation.fetch_add(1);
    Release(publisher, Data(11, 24));
    EXPECT_EQ(publisher.Published(), 1U);
    EXPECT_EQ(publisher.Rejected(), 1U);
    auto bridge = Read(control);
    EXPECT_EQ(bridge.updates, 1U);
    EXPECT_EQ(bridge.period.streamGeneration, generation);
    EXPECT_EQ(bridge.period.packetIndex[2], 10U);

    // The next start re-arms the bridge, and arming the producer re-binds.
    control.ResetForStart();
    const uint64_t next = control.generation.load();
    publisher.Bind(&control, &timeline, kLead);
    EXPECT_EQ(publisher.Rejected(), 0U);
    Release(publisher, Data(12, 48));
    bridge = Read(control);
    ASSERT_TRUE(bridge.published);
    EXPECT_EQ(bridge.updates, 1U);
    EXPECT_EQ(bridge.period.streamGeneration, next);
}

// Binding starts from an empty window: the previous stream's packets belong to
// another phase. An unbound publisher reads nothing.
TEST(MotuTxPhaseTracePublisherTests, BindingStartsAnEmptyWindowAndUnbindingStopsIt) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    IsochTxQueueControl queue{};
    ExecutionTimeline timeline(queue);
    MotuTxPhaseTracePublisher publisher;
    EXPECT_FALSE(publisher.IsBound());
    Release(publisher, Data(1, 0));
    EXPECT_FALSE(Read(control).published);

    publisher.Bind(&control, &timeline, kLead);
    Release(publisher, Data(2, 8));
    Release(publisher, Data(3, 16));
    EXPECT_FALSE(Read(control).published);  // the gate has not fired

    publisher.Bind(&control, &timeline, kLead);
    Release(publisher, Data(5, 24));
    auto bridge = Read(control);
    ASSERT_TRUE(bridge.published);
    EXPECT_EQ(bridge.period.count, 1U);

    publisher.Unbind();
    EXPECT_FALSE(publisher.IsBound());
    Release(publisher, Data(6, 48));
    bridge = Read(control);
    EXPECT_EQ(bridge.updates, 1U);
}

} // namespace
