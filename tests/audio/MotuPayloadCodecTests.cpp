// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Wire/MOTU/MotuPayloadCodec.hpp"
#include "Audio/Wire/MOTU/MotuDeviceTiming.hpp"
#include "Audio/Wire/MOTU/MotuBlockLayout.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"
#include "Audio/Wire/MOTU/MotuRxDiagnosticCapture.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"
#include "Audio/DriverKit/Config/MOTU/MotuProfile.hpp"
#include "Logging/LogRing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

using namespace ASFW::Audio::Wire;
using namespace ASFW::AudioEngine::Direct::Rx;
using namespace ASFW::Encoding::Motu;
using namespace ASFW::Protocols::Audio::AMDTP;

namespace {

void StoreBigEndian(uint8_t* dst, uint32_t value) noexcept {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

class TestTxSlotProvider final : public IAmdtpTxSlotProvider {
public:
    bool allowAcquire{true};
    bool allowPublish{true};
    std::array<uint8_t, 512> bytes{};
    PreparedTxPacket publishedPacket{};

    bool AcquireWritableSlot(
        uint64_t packetIndex,
        TxPacketSlotView& outSlot) noexcept override {
        if (!allowAcquire) {
            return false;
        }
        outSlot = {
            .packetIndex = packetIndex,
            .bytes = bytes.data(),
            .capacityBytes = static_cast<uint32_t>(bytes.size()),
        };
        return true;
    }

    bool PublishSlot(
        const PreparedTxPacket& packet) noexcept override {
        publishedPacket = packet;
        return allowPublish;
    }

    uint32_t SlotCount() const noexcept override {
        return 1;
    }
};

class TestAudioProfile final : public ASFW::Isoch::Audio::IAudioStreamProfile {
public:
    const char* Name() const noexcept override { return "TestAudioProfile"; }
    ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override { return {}; }
    ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override { return {}; }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }

    bool BuildDefaultTxStreamConfig(ASFW::Isoch::Audio::AudioStreamConfig& c) const noexcept override {
        c = {};
        c.direction = ASFW::Isoch::Audio::AudioStreamDirection::HostToDevice;
        c.sampleRate = 48000;
        c.streamMode = ASFW::Encoding::StreamMode::kBlocking;
        c.pcmChannels = 2;
        c.dbs = 2;
        c.midiSlots = 0;
        c.framesPerDataPacket = 8;
        c.fdf = 0x02;
        c.fmt = 0x10;
        c.sid = 0;
        return true;
    }
    bool BuildDefaultRxStreamConfig(ASFW::Isoch::Audio::AudioStreamConfig& c) const noexcept override {
        return BuildDefaultTxStreamConfig(c);
    }
};

} // namespace

TEST(MotuPayloadCodecTests, StrideAndGeometryValidation) {
    // 14 PCM chunks -> DataBlockQuadlets(14) = 1 + (((2 + 14)*3 + 3) / 4) = 13 quadlets.
    MotuRxPayloadCodec codec(14);

    EXPECT_EQ(codec.StrideQuadlets(8), 13U);
    EXPECT_EQ(codec.StrideQuadlets(19), 13U);

    // 0 chunks falls back to CIP DBS
    MotuRxPayloadCodec fallbackCodec(0);
    EXPECT_EQ(fallbackCodec.StrideQuadlets(8), 8U);

    // Geometry validation:
    // Valid: 4 channels starting at offset 0, 14 chunks, stride 13
    EXPECT_TRUE(codec.ValidateGeometry(4, 0, 13, 8));
    // Invalid: 0 channels
    EXPECT_FALSE(codec.ValidateGeometry(0, 0, 13, 8));
    // Invalid: 0 chunks
    EXPECT_FALSE(fallbackCodec.ValidateGeometry(4, 0, 13, 8));
    // Invalid: offset + channels > pcmChunks (offset 12 + 4 = 16 > 14)
    EXPECT_FALSE(codec.ValidateGeometry(4, 12, 13, 8));
    // Invalid: stride too small for required bytes (14 chunks need 10 + 14*3 = 52 bytes = 13 quadlets)
    EXPECT_FALSE(codec.ValidateGeometry(4, 0, 12, 8));
}

TEST(MotuPayloadCodecTests, DecodeBlockExtractsFloatSamplesAndHandlesDelay) {
    constexpr uint32_t kChunks = 4;
    MotuRxPayloadCodec codec(kChunks);

    // Build one 24-byte block (4 chunks: SPH 4B + Msg 6B + 4*3B = 22B, padded to 24B = 6 quadlets)
    std::vector<uint8_t> block(DataBlockQuadlets(kChunks) * 4, 0);

    // Sample values in 24-bit top: 0.5f and -0.5f
    // 0.5f in signed 24-bit: 0x400000 -> sampleTop24: 0x40000000
    const int32_t sample0 = static_cast<int32_t>(static_cast<uint32_t>(4194304) << 8);
    // -0.5f in signed 24-bit: -4194304 -> sampleTop24: -4194304 << 8
    const int32_t sample1 = static_cast<int32_t>(static_cast<uint32_t>(-4194304) << 8);

    // Write chunk 0 at offset 10
    block[10] = static_cast<uint8_t>((sample0 >> 24) & 0xFF);
    block[11] = static_cast<uint8_t>((sample0 >> 16) & 0xFF);
    block[12] = static_cast<uint8_t>((sample0 >> 8) & 0xFF);

    // Write chunk 1 at offset 13
    block[13] = static_cast<uint8_t>((sample1 >> 24) & 0xFF);
    block[14] = static_cast<uint8_t>((sample1 >> 16) & 0xFF);
    block[15] = static_cast<uint8_t>((sample1 >> 8) & 0xFF);

    std::array<float, 2> frameOut{};
    std::array<float, 2> delayedOut{};
    RxCaptureChannelMap map{};
    map.delayedChannelMask = (1U << 1); // Channel 1 is delayed
    map.delayFrames = 1;

    codec.DecodeBlock(block, 2, 0, map, frameOut.data(), delayedOut.data());

    // Channel 0 is not delayed: frameOut has ~0.5f
    EXPECT_NEAR(frameOut[0], 0.5f, 1e-5f);
    // Channel 1 is delayed: frameOut is zeroed, delayedOut has ~-0.5f
    EXPECT_EQ(frameOut[1], 0.0f);
    EXPECT_NEAR(delayedOut[1], -0.5f, 1e-5f);
}

TEST(MotuPayloadCodecTests, TxTimingStamperStampsTheRegulatorClockAndZeroesBeforeTheSeed) {
    constexpr uint32_t kDbs = 4;
    MotuTxTimingStamper stamper{};
    ASSERT_TRUE(stamper.Configure(kDbs, 48000, kDefaultPresentationLeadTicks, 0));
    EXPECT_TRUE(stamper.IsSytUnaware());

    // 1 slot: 8 bytes CIP + 2 blocks * 16 bytes, initialised with dirty memory.
    std::vector<uint8_t> slotBytes(40, 0xFF);
    TxPacketSlotView slot{
        .packetIndex = 0,
        .bytes = slotBytes.data(),
        .capacityBytes = static_cast<uint32_t>(slotBytes.size()),
    };
    PreparedTxPacket packet{
        .packetIndex = 0,
        .byteCount = static_cast<uint32_t>(slotBytes.size()),
        .isData = true,
        .framesInPacket = 2,
        .dbs = kDbs,
    };
    const auto sphAt = [&](size_t offset) {
        return ReadSph(std::span<const uint8_t>(slotBytes).subspan(offset, 4));
    };

    // No transmit time and no seed yet: nothing to stamp, every SPH zeroed.
    AmdtpTimingState timing{.transmitCycle = 10, .transmitCycleValid = false};
    EXPECT_EQ(stamper.StampPacket(slot, packet, timing),
              ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable);
    EXPECT_EQ(sphAt(8), 0U);
    EXPECT_EQ(sphAt(24), 0U);

    // The first transmit time seeds the clock: transmit time plus the lead,
    // then one 48 kHz sample period (512 ticks) per block.
    timing.transmitCycleValid = true;
    timing.transmitCycle = 20;
    timing.transmitTicks = 20 * kTicksPerCycle + 100;
    EXPECT_EQ(stamper.StampPacket(slot, packet, timing), ::ASFW::Audio::TxTimingStampResult::kOk);
    const uint32_t seed = 20 * kTicksPerCycle + 100 + kDefaultPresentationLeadTicks;
    EXPECT_EQ(sphAt(8), SphFromTick(seed));
    EXPECT_EQ(sphAt(24), SphFromTick(seed + 512));
}

TEST(MotuPayloadCodecTests, MissingTimingFallsBackToNoDataPacketInEngine) {
    using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
    using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;

    DiceTxStreamEngine engine{};
    TestAudioProfile profile{};
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    ASSERT_TRUE(engine.Configure(profile, config));

    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);

    // A configured stamper whose clock has no seed yet.
    constexpr uint32_t kDbs = 2;
    MotuTxTimingStamper stamper{};
    ASSERT_TRUE(stamper.Configure(kDbs, 48000, kDefaultPresentationLeadTicks, 0));
    engine.BindTimingStamper(&stamper);

    // Timing requested Data with 8 blocks, but without a transmit time to
    // seed the clock from.
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;
    timing.transmitCycleValid = false;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;

    EXPECT_EQ(engine.NextAudioFrame(), 0U);
    EXPECT_EQ(engine.Counters().timingUnavailableReverts.load(), 0U);

    // Prepare slot: stamper will fail with kTimingUnavailable -> engine reverts to NO-DATA
    const auto result = engine.PrepareNextTransmitSlot(0, timing);
    EXPECT_EQ(result, TxSlotPrepareResult::kPrepared);

    // Verify published packet was reverted to NO-DATA
    EXPECT_FALSE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.publishedPacket.byteCount, 8U);
    EXPECT_EQ(provider.publishedPacket.framesInPacket, 0U);
    EXPECT_EQ(provider.publishedPacket.syt, 0xFFFFU);
    EXPECT_EQ(provider.publishedPacket.dbc, 0U);

    // Audio frame cursor must NOT have advanced
    EXPECT_EQ(engine.NextAudioFrame(), 0U);

    // Counter must reflect the revert
    EXPECT_EQ(engine.Counters().timingUnavailableReverts.load(), 1U);
}

TEST(MotuPayloadCodecTests, ProductionProfilePreservesMotuCipOnDataAndNoData) {
    using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
    using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;
    ASFW::Isoch::Audio::MOTU::Profiles::MotuProfile profile{3};
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    DiceTxStreamEngine engine{};
    ASSERT_TRUE(engine.Configure(profile, config));
    TestTxSlotProvider provider{};
    engine.BindSlotProvider(&provider);

    MotuTxTimingStamper stamper{};
    const auto txPolicy = profile.TxStreamPolicy();
    ASSERT_TRUE(stamper.Configure(config.dbs, config.sampleRate, txPolicy.motuPresentationLeadTicks,
                                  txPolicy.motuPresentationOffsetFrames));
    engine.BindTimingStamper(&stamper);
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.nextDataSyt = 0x1234;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.transmitCycleValid = true;
    timing.transmitCycle = 100;
    timing.transmitTicks = 100 * kTicksPerCycle;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(0, timing), TxSlotPrepareResult::kPrepared);
    ASSERT_TRUE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.bytes[2] & 0x04, 0x04); // SPH bit, on the wire.
    EXPECT_EQ(provider.bytes[3], 8);           // End-event DBC.
    EXPECT_EQ((std::array<uint8_t, 4>{provider.bytes[4], provider.bytes[5], provider.bytes[6], provider.bytes[7]}),
              (std::array<uint8_t, 4>{0x82, 0x22, 0xff, 0xff}));

    timing.disposition = AmdtpPacketDisposition::NoData;
    ASSERT_EQ(engine.PrepareNextTransmitSlot(1, timing), TxSlotPrepareResult::kPrepared);
    EXPECT_FALSE(provider.publishedPacket.isData);
    EXPECT_EQ(provider.publishedPacket.byteCount, 8U);
    EXPECT_EQ(provider.bytes[3], 8);
    EXPECT_EQ((std::array<uint8_t, 4>{provider.bytes[4], provider.bytes[5], provider.bytes[6], provider.bytes[7]}),
              (std::array<uint8_t, 4>{0x82, 0x22, 0xff, 0xff}));
}

TEST(MotuPayloadCodecTests, ProductionProfileCarriesTheModelPresentationLeadAndSetpoint) {
    for (const uint32_t version : {0x1U, 0x3U, 0x15U}) {
        SCOPED_TRACE(version);
        const ASFW::Isoch::Audio::MOTU::Profiles::MotuProfile profile{version};
        const auto* model = FindModel(version);
        ASSERT_NE(model, nullptr);
        EXPECT_EQ(profile.TxStreamPolicy().motuPresentationLeadTicks,
                  model->runtime.presentationLeadTicks);
        EXPECT_EQ(profile.TxStreamPolicy().motuPresentationOffsetFrames,
                  model->runtime.presentationOffsetFrames);
    }
}

TEST(MotuPayloadCodecTests, MotuRxDiagnosticCaptureRecordsStartupAndSteadyState) {
    MotuRxDiagnosticCapture capture{};

    // 1. Capture session for 44.1 kHz
    MotuRxStreamMetadata meta441{};
    std::strncpy(meta441.model, "828mk2", sizeof(meta441.model) - 1);
    meta441.firmwareVersion = 0x0100;
    meta441.sampleRateHz = 44100;
    std::strncpy(meta441.clockSource, "Internal", sizeof(meta441.clockSource) - 1);
    meta441.guid = 0x0001f20000000001ULL;

    ASSERT_TRUE(capture.Arm(meta441));
    EXPECT_TRUE(capture.IsArmed());
    EXPECT_FALSE(capture.IsFull());
    EXPECT_EQ(capture.Count(), 0U);

    constexpr uint32_t kDbs = 4;
    // Startup: first 4 packets are NO-DATA (only 16-byte isoch+CIP prefix, 0 data blocks)
    for (uint32_t i = 0; i < 4; ++i) {
        std::vector<uint8_t> noDataPkt(16, 0);
        // Isoch header at [0..7], CIP0 at [8..11], CIP1 at [12..15]
        StoreBigEndian(noDataPkt.data() + 8, 0x82000000U | (kDbs << 16) | (i & 0xFFU));
        StoreBigEndian(noDataPkt.data() + 12, 0x9002FFFFU); // NO-DATA SYT=0xFFFF
        capture.RecordPacket(/*epoch=*/1, static_cast<uint16_t>(100 + i), noDataPkt, kDbs, 16);
    }

    // Steady-state: next 60 packets carry data blocks with SPH
    for (uint32_t i = 4; i < 64; ++i) {
        const uint32_t blocks = (i % 2 == 0) ? 5 : 6;
        std::vector<uint8_t> dataPkt(16 + blocks * kDbs * 4, 0);
        StoreBigEndian(dataPkt.data() + 8, 0x82000000U | (kDbs << 16) | (i & 0xFFU));
        StoreBigEndian(dataPkt.data() + 12, 0x90020000U | (i * 100)); // SYT
        const uint16_t cycle = static_cast<uint16_t>(100 + i);
        const uint32_t baseTick = ::ASFW::Encoding::Motu::BaseTickForCycle(cycle);
        for (uint32_t b = 0; b < blocks; ++b) {
            const uint32_t tick = baseTick + b * 500;
            const uint32_t sph = ::ASFW::Encoding::Motu::SphFromTick(tick);
            StoreBigEndian(dataPkt.data() + 16 + b * kDbs * 4, sph);
        }
        capture.RecordPacket(/*epoch=*/1, cycle, dataPkt, kDbs, 16);
    }

    EXPECT_EQ(capture.Count(), 64U);
    EXPECT_TRUE(capture.IsFull());
    EXPECT_FALSE(capture.IsArmed()); // Auto-disarmed when full

    // 65th packet must be dropped and counted as overflow
    std::vector<uint8_t> extraPkt(16, 0);
    capture.RecordPacket(/*epoch=*/1, 200, extraPkt, kDbs, 16);
    EXPECT_EQ(capture.OverflowCount(), 1U);
    EXPECT_EQ(capture.Count(), 64U);

    // Verify first 4 are recorded as having 0 SPH
    const auto packets = capture.Packets();
    ASSERT_EQ(packets.size(), 64U);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(packets[i].sphCount, 0U);
        EXPECT_EQ(packets[i].payloadBytes, 16U);
    }

    // Verify data packets have raw SPH and decoded ticks
    EXPECT_EQ(packets[4].sphCount, 5U);
    EXPECT_NE(packets[4].rawSph[0], 0U);
    EXPECT_EQ(packets[4].decodedSphTick[0], 0U);
    EXPECT_EQ(packets[4].decodedSphTick[1], 500U);

    std::string summary441 = capture.FormatCaptureSummary();
    EXPECT_FALSE(summary441.empty());
    EXPECT_NE(summary441.find("828mk2"), std::string::npos);
    EXPECT_NE(summary441.find("44100 Hz"), std::string::npos);

    // 2. Capture session for 48 kHz
    MotuRxStreamMetadata meta48{};
    std::strncpy(meta48.model, "Traveler", sizeof(meta48.model) - 1);
    meta48.firmwareVersion = 0x0200;
    meta48.sampleRateHz = 48000;
    std::strncpy(meta48.clockSource, "ADAT", sizeof(meta48.clockSource) - 1);
    meta48.guid = 0x0001f20000000002ULL;

    ASSERT_TRUE(capture.Arm(meta48));
    EXPECT_TRUE(capture.IsArmed());
    EXPECT_FALSE(capture.IsFull());
    EXPECT_EQ(capture.Count(), 0U);
    EXPECT_EQ(capture.OverflowCount(), 0U);

    for (uint32_t i = 0; i < 64; ++i) {
        constexpr uint32_t kBlocks48 = 6;
        std::vector<uint8_t> dataPkt(16 + kBlocks48 * kDbs * 4, 0);
        StoreBigEndian(dataPkt.data() + 8, 0x82000000U | (kDbs << 16) | (i & 0xFFU));
        StoreBigEndian(dataPkt.data() + 12, 0x90020000U);
        const uint16_t cycle = static_cast<uint16_t>(200 + i);
        const uint32_t baseTick = ::ASFW::Encoding::Motu::BaseTickForCycle(cycle);
        for (uint32_t b = 0; b < kBlocks48; ++b) {
            const uint32_t tick = baseTick + b * 400;
            const uint32_t sph = ::ASFW::Encoding::Motu::SphFromTick(tick);
            StoreBigEndian(dataPkt.data() + 16 + b * kDbs * 4, sph);
        }
        capture.RecordPacket(/*epoch=*/2, cycle, dataPkt, kDbs, 16);
    }

    EXPECT_EQ(capture.Count(), 64U);
    EXPECT_TRUE(capture.IsFull());
    EXPECT_FALSE(capture.IsArmed());

    std::string summary48 = capture.FormatCaptureSummary();
    EXPECT_FALSE(summary48.empty());
    EXPECT_NE(summary48.find("Traveler"), std::string::npos);
    EXPECT_NE(summary48.find("48000 Hz"), std::string::npos);
}

TEST(MotuPayloadCodecTests, CaptureControlCannotRaceAdmittedWriter) {
    MotuRxDiagnosticCapture capture;
    MotuRxStreamMetadata metadata{};
    ASSERT_TRUE(capture.Arm(metadata));
    capture.SetOnRecordAdmittedForTesting([&] {
        EXPECT_FALSE(capture.Disarm());
        EXPECT_FALSE(capture.Arm(metadata));
        MotuRxDiagnosticCapture::Snapshot snapshot;
        EXPECT_FALSE(capture.CopySnapshot(snapshot));
    });
    capture.RecordPacket(7, 0xe001, {}, 4, 16);
    EXPECT_EQ(capture.Count(), 1U);
    ASSERT_TRUE(capture.Disarm());
    const auto old = capture.Packets();
    ASSERT_TRUE(capture.Arm(metadata));
    capture.RecordPacket(8, 2, {}, 4, 16);
    ASSERT_EQ(old.size(), 1U);
    EXPECT_EQ(old[0].epoch, 7U);
    EXPECT_EQ(old[0].rxTimestamp, 0xe001);
    EXPECT_FALSE(old[0].timestampValid);
}

TEST(MotuPayloadCodecTests, CaptureReaderOwnsSnapshotAndReportsBusyPacketDrop) {
    MotuRxDiagnosticCapture capture;
    MotuRxStreamMetadata metadata{};
    ASSERT_TRUE(capture.Arm(metadata));
    capture.RecordPacket(3, 10, {}, 4, 16);
    capture.SetOnSnapshotLockedForTesting([&] {
        EXPECT_FALSE(capture.Disarm());
        EXPECT_FALSE(capture.Arm(metadata));
        capture.RecordPacket(9, 20, {}, 4, 16);
    });
    MotuRxDiagnosticCapture::Snapshot snapshot;
    ASSERT_TRUE(capture.CopySnapshot(snapshot));
    EXPECT_EQ(snapshot.count, 1U);
    EXPECT_EQ(snapshot.dropped, 1U);
    EXPECT_EQ(snapshot.packets[0].epoch, 3U);
}

TEST(MotuPayloadCodecTests, DiagnosticUsesConfiguredStrideAndExportsEverySph) {
    MotuRxDiagnosticCapture capture;
    MotuRxStreamMetadata metadata{};
    metadata.guid = 0x1234567800000001ULL;
    ASSERT_TRUE(capture.Arm(metadata));
    MotuRxTimingObserver observer;
    observer.BindDiagnosticCapture(&capture, 4, metadata.guid);
    std::vector<uint8_t> bytes(16 + 6 * 16 + 1, 0);
    StoreBigEndian(bytes.data() + 8, 1U << 16); // Device reports wrong DBS.
    for (unsigned i = 0; i < 6; ++i) StoreBigEndian(bytes.data() + 16 + i * 16, i + 1);
    observer.ObserveRawPacket(3, 0xe001, bytes);
    const auto packets = capture.Packets();
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].sphCount, 6U);
    EXPECT_EQ(packets[0].rawSph[5], 6U);
    EXPECT_EQ(packets[0].trailingBytes, 1U);
    EXPECT_TRUE(packets[0].timestampValid);
    EXPECT_NE(capture.FormatCaptureSummary().find("0x00000006"), std::string::npos);
    capture.RecordPacket(4, 0, bytes, 4, 16, metadata.guid + 1);
    EXPECT_EQ(capture.Count(), 1U); // Another device cannot contaminate this capture.
}

TEST(MotuPayloadCodecTests, TimingRecoveryCoalescesAndRearmsAfterSuccessOrRestart) {
    using ASFW::Protocols::Audio::DICE::DiceTxStreamEngine;
    using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;
    struct Stamper final : ASFW::Audio::ITxDeviceTimingStamper {
        bool available{false};
        ASFW::Audio::TxTimingStampResult StampPacket(const TxPacketSlotView&,
            const PreparedTxPacket&, const AmdtpTimingState&) noexcept override {
            return available ? ASFW::Audio::TxTimingStampResult::kOk
                             : ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        }
        bool IsSytUnaware() const noexcept override { return true; }
    } stamper;
    DiceTxStreamEngine engine;
    TestAudioProfile profile;
    ASFW::Isoch::Audio::AudioStreamConfig config;
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(config));
    ASSERT_TRUE(engine.Configure(profile, config));
    TestTxSlotProvider provider;
    engine.BindSlotProvider(&provider);
    engine.BindTimingStamper(&stamper);
    unsigned calls = 0;
    bool accept = false;
    engine.SetTimingLossCallback([&] { ++calls; return accept; });
    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;
    timing.replayValid = true;
    timing.replayDataBlocks = 8;
    uint32_t packet = 0;
    const auto burst = [&] {
        for (unsigned i = 0; i < DiceTxStreamEngine::kMaxConsecutiveTimingReverts; ++i)
            EXPECT_EQ(engine.PrepareNextTransmitSlot(packet++, timing), TxSlotPrepareResult::kPrepared);
    };
    burst(); EXPECT_EQ(calls, 1U); // Startup request rejected: retry is possible.
    accept = true;
    burst(); EXPECT_EQ(calls, 2U);
    burst(); EXPECT_EQ(calls, 2U); // One accepted request per continuous outage.
    stamper.available = true;
    EXPECT_EQ(engine.PrepareNextTransmitSlot(packet++, timing), TxSlotPrepareResult::kPrepared);
    stamper.available = false;
    burst(); EXPECT_EQ(calls, 3U);
    engine.ResetForStart(0, 0);
    burst(); EXPECT_EQ(calls, 4U);
}

// ---------------------------------------------------------------------------
// The RX side of the SPH regulator: the observer measures the device clock
// from received SPH and publishes it over the control block's motuRxSphClock
// bridge, for every MOTU protocol version.
// ---------------------------------------------------------------------------

namespace {

class ControlBindingSource final : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
public:
    ControlBindingSource(ASFW::Audio::Runtime::AudioTransportControlBlock* control,
                         uint32_t sampleRateHz) noexcept
        : control_(control), sampleRateHz_(sampleRateHz) {}

    bool CopyDirectAudioBinding(ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        if (!bound) {
            return false;
        }
        out = {};
        out.generation = 1;
        out.control = control_;
        out.sampleRateHz = sampleRateHz_;
        out.valid = true;
        return true;
    }

    bool bound{true};

private:
    ASFW::Audio::Runtime::AudioTransportControlBlock* control_;
    uint32_t sampleRateHz_;
};

// A received MOTU packet of `blocks` data blocks whose first SPH sits at
// `ticks`: the 8-byte receive prefix, the CIP header, then the blocks.
std::vector<uint8_t> ReceivedMotuPacket(int64_t ticks, uint32_t blocks, uint32_t dbs = 4) {
    std::vector<uint8_t> payload(16 + blocks * dbs * 4, 0);
    for (uint32_t block = 0; block < blocks; ++block) {
        StoreBigEndian(payload.data() + 16 + block * dbs * 4,
                       SphFromTick(WrapTick(ticks + static_cast<int64_t>(block) * 512)));
    }
    return payload;
}

struct ObservedClock final {
    bool published{false};
    uint64_t updates{0};
    ASFW::Audio::Runtime::MotuRxSphClockSample sample{};
};

ObservedClock ReadClock(const ASFW::Audio::Runtime::AudioTransportControlBlock& control) {
    ObservedClock out{};
    out.published = control.motuRxSphClock.ReadLatest(out.sample, out.updates);
    return out;
}

} // namespace

TEST(MotuPayloadCodecTests, RxTimingObserverPublishesTheDeviceClockToTheBridge) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t generation = control.generation.load(std::memory_order_acquire);
    ControlBindingSource source(&control, 48000);
    MotuRxTimingObserver observer(&source);

    // The SPH crosses the one-second wrap after the first packet.
    constexpr int64_t kStart = 24576000 - 1000;
    observer.OnBatchBegin(nullptr);
    // The first packet only anchors the meter: nothing measured yet.
    observer.ObservePacket(ReceivedMotuPacket(kStart, 8), 4, 8, 0, 0);
    EXPECT_FALSE(ReadClock(control).published);

    observer.ObservePacket(ReceivedMotuPacket(kStart + 4096, 8), 4, 8, 1, 0);
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 0, 2, 0);  // NO-DATA: skipped
    observer.ObservePacket(ReceivedMotuPacket(kStart + 8193, 8), 4, 8, 3, 0);

    const auto clock = ReadClock(control);
    ASSERT_TRUE(clock.published);
    EXPECT_EQ(clock.updates, 2U);
    EXPECT_EQ(clock.sample.streamGeneration, generation);
    EXPECT_EQ(clock.sample.rxFrames, 16U);
    EXPECT_EQ(clock.sample.rxTicks, 8193);
}

// Receive activation, quiesce and replay discontinuities all call Reset()
// inside one stream. The step across the gap is not measured, and the
// cumulative clock stays monotonic, so the bridge keeps accepting it.
TEST(MotuPayloadCodecTests, RxTimingObserverKeepsTheClockMonotonicAcrossResetWithinAStream) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 48000);
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 0, 0);
    observer.ObservePacket(ReceivedMotuPacket(4096, 8), 4, 8, 1, 0);
    observer.ObservePacket(ReceivedMotuPacket(8192, 8), 4, 8, 2, 0);
    ASSERT_EQ(ReadClock(control).sample.rxFrames, 16U);

    observer.Reset();
    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(1048576, 8), 4, 8, 300, 0);
    observer.ObservePacket(ReceivedMotuPacket(1048576 + 4096, 8), 4, 8, 301, 0);

    const auto clock = ReadClock(control);
    EXPECT_EQ(clock.updates, 3U);
    EXPECT_EQ(clock.sample.rxFrames, 24U);
    EXPECT_EQ(clock.sample.rxTicks, 12288);
}

// StartIO re-arms the bridge with a new generation, possibly on the same
// mapped control block: the measurement starts over with it.
TEST(MotuPayloadCodecTests, RxTimingObserverStartsTheClockOverOnANewControlGeneration) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 48000);
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 0, 0);
    observer.ObservePacket(ReceivedMotuPacket(4096, 8), 4, 8, 1, 0);
    observer.ObservePacket(ReceivedMotuPacket(8192, 8), 4, 8, 2, 0);

    control.ResetForStart();
    const uint64_t nextGeneration = control.generation.load(std::memory_order_acquire);
    observer.Reset();
    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(2097152, 8), 4, 8, 3, 0);
    observer.ObservePacket(ReceivedMotuPacket(2097152 + 4096, 8), 4, 8, 4, 0);

    const auto clock = ReadClock(control);
    ASSERT_TRUE(clock.published);
    EXPECT_EQ(clock.updates, 1U);
    EXPECT_EQ(clock.sample.streamGeneration, nextGeneration);
    EXPECT_EQ(clock.sample.rxFrames, 8U);
    EXPECT_EQ(clock.sample.rxTicks, 4096);
}

// Higher rates carry more frames per packet; the rate the meter judges steps
// by comes from the binding.
TEST(MotuPayloadCodecTests, RxTimingObserverMeasuresAtTheBoundRate) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 96000);
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(0, 16), 4, 16, 0, 0);
    observer.ObservePacket(ReceivedMotuPacket(4096, 16), 4, 16, 1, 0);  // 16 frames x 256 ticks

    const auto clock = ReadClock(control);
    ASSERT_TRUE(clock.published);
    EXPECT_EQ(clock.sample.rxFrames, 16U);
    EXPECT_EQ(clock.sample.rxTicks, 4096);
}

// The receive replay establishes once the device's timing is observed: the
// first DATA packet with an SPH since the last Reset() or stream generation.
TEST(MotuPayloadCodecTests, RxTimingObserverEstablishesTimingOnTheFirstDataPacket) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 48000);
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    EXPECT_FALSE(observer.IsTimingEstablished());
    // A NO-DATA packet carries no SPH; a truncated one has none to read.
    observer.ObservePacket(std::vector<uint8_t>(16, 0), 4, 0, 0, 0);
    observer.ObservePacket(std::vector<uint8_t>(18, 0), 4, 1, 1, 0);
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 2, 0);
    EXPECT_TRUE(observer.IsTimingEstablished());

    // Receive activation, quiesce and replay discontinuities start over.
    observer.Reset();
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.OnBatchBegin(nullptr);
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket(ReceivedMotuPacket(4096, 8), 4, 8, 3, 0);
    EXPECT_TRUE(observer.IsTimingEstablished());
}

// A new stream generation starts over too, even on the same control block. A
// batch without a binding observes nothing, and the same stream picks up
// where it was once the binding is back.
TEST(MotuPayloadCodecTests, RxTimingObserverEstablishesPerGenerationAndOnlyWhileBound) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 48000);
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 0, 0);
    ASSERT_TRUE(observer.IsTimingEstablished());

    source.bound = false;
    observer.OnBatchBegin(nullptr);
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket(ReceivedMotuPacket(4096, 8), 4, 8, 1, 0);
    EXPECT_FALSE(observer.IsTimingEstablished());
    source.bound = true;
    observer.OnBatchBegin(nullptr);
    EXPECT_TRUE(observer.IsTimingEstablished());

    control.ResetForStart();
    observer.OnBatchBegin(nullptr);
    EXPECT_FALSE(observer.IsTimingEstablished());
    observer.ObservePacket(ReceivedMotuPacket(8192, 8), 4, 8, 2, 0);
    EXPECT_TRUE(observer.IsTimingEstablished());

    // Never without any binding source at all.
    MotuRxTimingObserver unbound{};
    unbound.OnBatchBegin(nullptr);
    unbound.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 0, 0);
    EXPECT_FALSE(unbound.IsTimingEstablished());
}

TEST(MotuPayloadCodecTests, RxTimingObserverPublishesNothingWithoutABinding) {
    ASFW::Audio::Runtime::AudioTransportControlBlock control{};
    control.ResetForStart();
    ControlBindingSource source(&control, 48000);
    source.bound = false;
    MotuRxTimingObserver observer(&source);

    observer.OnBatchBegin(nullptr);
    observer.ObservePacket(ReceivedMotuPacket(0, 8), 4, 8, 0, 0);
    observer.ObservePacket(ReceivedMotuPacket(4096, 8), 4, 8, 1, 0);
    observer.DrainTelemetry(16);
    EXPECT_FALSE(ReadClock(control).published);
}
