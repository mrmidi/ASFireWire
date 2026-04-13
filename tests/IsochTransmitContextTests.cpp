// IsochTransmitContextTests.cpp
// ASFW - Host-safe unit tests for transmit packetization behavior
//
// NOTE:
// Full IsochTransmitContext runtime tests require DriverKit DMA/hardware wiring.
// In ASFW_HOST_TEST, we validate the same cadence/DBC/underrun behavior through
// PacketAssembler, plus a lightweight API state smoke test.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../ASFWDriver/Isoch/Transmit/IsochTransmitContext.hpp"
#include "../ASFWDriver/Isoch/Encoding/TimingUtils.hpp"
#include "../ASFWDriver/Isoch/Core/ExternalSyncBridge.hpp"
#include "../ASFWDriver/Isoch/Config/AudioConstants.hpp"
#include "../ASFWDriver/Shared/TxSharedQueue.hpp"

using namespace ASFW::Isoch;
using namespace ASFW::Encoding;

namespace {

[[nodiscard]] uint32_t ReadWireQuadlet(const uint8_t* bytes) noexcept {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

[[nodiscard]] uint16_t ReadPacketSyt(const Tx::IsochTxPacket& pkt) noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(pkt.words);
    return static_cast<uint16_t>(ReadWireQuadlet(bytes + 4) & 0xFFFFu);
}

[[nodiscard]] uint8_t ReadPacketFdf(const Tx::IsochTxPacket& pkt) noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(pkt.words);
    return static_cast<uint8_t>((ReadWireQuadlet(bytes + 4) >> 16) & 0xFFu);
}

} // namespace

TEST(IsochTransmitContext, InitialStateIsUnconfigured) {
    IsochTransmitContext ctx;
    EXPECT_EQ(ctx.GetState(), ITState::Unconfigured);
}

TEST(IsochTransmitContext, ConfigureSucceedsWithQueueChannelMetadata) {
    constexpr uint32_t kQueueChannels = 6;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochTransmitContext ctx;
    ctx.SetSharedTxQueue(storage.data(), bytes);

    EXPECT_EQ(ctx.Configure(/*channel=*/0, /*sid=*/0x3F, /*streamModeRaw=*/0, /*requestedChannels=*/kQueueChannels),
              kIOReturnSuccess);
    EXPECT_EQ(ctx.GetState(), ITState::Configured);
}

TEST(TxSharedQueueSPSC, SupportsMaxPcmChannels) {
    constexpr uint32_t kQueueChannels = ASFW::Isoch::Config::kMaxPcmChannels;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    EXPECT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));
}

TEST(IsochTransmitContext, ConfigureFailsOnRequestedChannelMismatch) {
    constexpr uint32_t kQueueChannels = 4;
    constexpr uint32_t kRequestedChannels = 6;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochTransmitContext ctx;
    ctx.SetSharedTxQueue(storage.data(), bytes);

    EXPECT_EQ(ctx.Configure(/*channel=*/0, /*sid=*/0x3F, /*streamModeRaw=*/0, /*requestedChannels=*/kRequestedChannels),
              kIOReturnBadArgument);
}

TEST(IsochTransmitContext, ConfigureFailsOnInvalidQueueChannelValue) {
    constexpr uint32_t kQueueChannels = 2;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochTransmitContext ctx;
    ctx.SetSharedTxQueue(storage.data(), bytes);

    auto* hdr = reinterpret_cast<ASFW::Shared::TxQueueHeader*>(storage.data());
    hdr->channels = 0;

    EXPECT_EQ(ctx.Configure(/*channel=*/0, /*sid=*/0x3F, /*streamModeRaw=*/0, /*requestedChannels=*/kQueueChannels),
              kIOReturnBadArgument);
}

TEST(IsochTransmitContext, StopTransitionsConfiguredContextToStopped) {
    constexpr uint32_t kQueueChannels = 6;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes =
        ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochTransmitContext ctx;
    ctx.SetSharedTxQueue(storage.data(), bytes);

    ASSERT_EQ(ctx.Configure(/*channel=*/0,
                            /*sid=*/0x3F,
                            /*streamModeRaw=*/0,
                            /*requestedChannels=*/kQueueChannels),
              kIOReturnSuccess);
    ASSERT_EQ(ctx.GetState(), ITState::Configured);

    ctx.Stop();
    EXPECT_EQ(ctx.GetState(), ITState::Stopped);

    EXPECT_EQ(ctx.Configure(/*channel=*/0,
                            /*sid=*/0x3F,
                            /*streamModeRaw=*/0,
                            /*requestedChannels=*/kQueueChannels),
              kIOReturnSuccess);
}

TEST(IsochTransmitContext, BlockingCadenceCountsMatchOneSecond) {
    PacketAssembler assembler(2, 0x3F);

    uint64_t dataPackets = 0;
    uint64_t noDataPackets = 0;

    // 1 second on FireWire bus cadence = 8000 cycles.
    for (int i = 0; i < 8000; ++i) {
        auto pkt = assembler.assembleNext(0x1234);
        if (pkt.isData) {
            ++dataPackets;
        } else {
            ++noDataPackets;
        }
    }

    EXPECT_EQ(dataPackets, 6000);
    EXPECT_EQ(noDataPackets, 2000);
}

TEST(IsochTransmitContext, CadenceOrderingTrace32Packets) {
    // Verify exact sequence: N-D-D-D-N-D-D-D repeated 4 times.
    std::array<bool, 32> expectedIsData = {
        false, true, true, true, false, true, true, true,
        false, true, true, true, false, true, true, true,
        false, true, true, true, false, true, true, true,
        false, true, true, true, false, true, true, true
    };

    PacketAssembler assembler(2, 0x3F);

    for (int i = 0; i < 32; ++i) {
        auto pkt = assembler.assembleNext(0xFFFF);
        EXPECT_EQ(pkt.isData, expectedIsData[i])
            << "Packet " << i << " expected "
            << (expectedIsData[i] ? "DATA" : "NO-DATA")
            << " but got " << (pkt.isData ? "DATA" : "NO-DATA");
    }
}

TEST(IsochTransmitContext, DBCNoDataBoundary) {
    // Per IEC 61883-1 blocking mode:
    // - NO-DATA carries the DBC of the next DATA packet
    // - Next DATA packet uses the same DBC value
    // - DATA increments DBC by sample count (8)
    PacketAssembler assembler(2, 0x3F);

    auto pkt0 = assembler.assembleNext(0xFFFF); // NO-DATA
    EXPECT_FALSE(pkt0.isData);

    auto pkt1 = assembler.assembleNext(0x1234); // DATA
    EXPECT_TRUE(pkt1.isData);
    EXPECT_EQ(pkt0.dbc, pkt1.dbc);

    auto pkt2 = assembler.assembleNext(0x1234); // DATA
    EXPECT_TRUE(pkt2.isData);
    EXPECT_EQ(pkt2.dbc, static_cast<uint8_t>((pkt1.dbc + 8) & 0xFF));

    auto pkt3 = assembler.assembleNext(0x1234); // DATA
    EXPECT_TRUE(pkt3.isData);
    EXPECT_EQ(pkt3.dbc, static_cast<uint8_t>((pkt2.dbc + 8) & 0xFF));

    auto pkt4 = assembler.assembleNext(0xFFFF); // NO-DATA
    EXPECT_FALSE(pkt4.isData);

    auto pkt5 = assembler.assembleNext(0x1234); // DATA
    EXPECT_TRUE(pkt5.isData);
    EXPECT_EQ(pkt4.dbc, pkt5.dbc);
    EXPECT_EQ(pkt5.dbc, static_cast<uint8_t>((pkt3.dbc + 8) & 0xFF));
}

TEST(IsochTransmitContext, UnderrunCountsOnEmptyBuffer) {
    PacketAssembler assembler(2, 0x3F);

    // One cadence group: N-D-D-D-N-D-D-D (6 DATA reads, all underrun on empty ring).
    for (int i = 0; i < 8; ++i) {
        assembler.assembleNext(0x1234);
    }

    EXPECT_EQ(assembler.underrunCount(), 6);
}

TEST(IsochTransmitContext, NoUnderrunsWithPrefilledBuffer) {
    PacketAssembler assembler(2, 0x3F);

    std::array<int32_t, 512 * 2> audioData{};
    for (size_t i = 0; i < audioData.size(); ++i) {
        audioData[i] = static_cast<int32_t>(i);
    }
    assembler.ringBuffer().write(audioData.data(), 512);

    for (int i = 0; i < 8; ++i) {
        assembler.assembleNext(0x1234);
    }

    // 8 packets in blocking mode => 6 DATA packets => 6 * 8 = 48 frames consumed.
    EXPECT_EQ(assembler.underrunCount(), 0);
    EXPECT_EQ(assembler.bufferFillLevel(), 512 - 48);
}

TEST(IsochTransmitContext, TxPipelineMatchesFocusritePlaybackSytSequence) {
    constexpr uint32_t kQueueChannels = 8;
    constexpr uint32_t kAm824Slots = 9;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochAudioTxPipeline pipeline;
    pipeline.SetSharedTxQueue(storage.data(), bytes);
    ASSERT_EQ(pipeline.Configure(/*sid=*/0x00,
                                 /*streamModeRaw=*/1u,
                                 /*requestedChannels=*/kQueueChannels,
                                 /*requestedAm824Slots=*/kAm824Slots,
                                 AudioWireFormat::kAM824),
              kIOReturnSuccess);

    Core::ExternalSyncBridge bridge;
    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0xD8B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(mach_absolute_time(), std::memory_order_release);

    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);
    ASSERT_TRUE(pipeline.PrimeSyncFromExternalBridge());

    const auto pkt0 = pipeline.NextSilentPacket(/*transmitCycle=*/5150);
    const bool pkt0IsData = pkt0.isData;
    const uint16_t pkt0Syt = ReadPacketSyt(pkt0);
    const uint8_t pkt0Dbc = pkt0.dbc;

    const auto pkt1 = pipeline.NextSilentPacket(/*transmitCycle=*/5151);
    const bool pkt1IsData = pkt1.isData;
    const uint16_t pkt1Syt = ReadPacketSyt(pkt1);
    const uint8_t pkt1Dbc = pkt1.dbc;
    const uint8_t pkt1Fdf = ReadPacketFdf(pkt1);

    const auto pkt2 = pipeline.NextSilentPacket(/*transmitCycle=*/5152);
    const bool pkt2IsData = pkt2.isData;
    const uint16_t pkt2Syt = ReadPacketSyt(pkt2);
    const uint8_t pkt2Dbc = pkt2.dbc;

    const auto pkt3 = pipeline.NextSilentPacket(/*transmitCycle=*/5153);
    const bool pkt3IsData = pkt3.isData;
    const uint16_t pkt3Syt = ReadPacketSyt(pkt3);
    const uint8_t pkt3Dbc = pkt3.dbc;

    const auto pkt4 = pipeline.NextSilentPacket(/*transmitCycle=*/5154);
    const bool pkt4IsData = pkt4.isData;
    const uint16_t pkt4Syt = ReadPacketSyt(pkt4);
    const uint8_t pkt4Dbc = pkt4.dbc;

    EXPECT_FALSE(pkt0IsData);
    EXPECT_TRUE(pkt1IsData);
    EXPECT_TRUE(pkt2IsData);
    EXPECT_TRUE(pkt3IsData);
    EXPECT_FALSE(pkt4IsData);

    EXPECT_EQ(pkt0Syt, 0xFFFF);
    EXPECT_EQ(pkt1Syt, 0xD8B0);
    EXPECT_EQ(pkt2Syt, 0xF0B0);
    EXPECT_EQ(pkt3Syt, 0x04B0);
    EXPECT_EQ(pkt4Syt, 0xFFFF);

    EXPECT_EQ(pkt1Fdf, kSFC_48kHz);
    EXPECT_EQ(pkt0Dbc, 0x00);
    EXPECT_EQ(pkt1Dbc, 0x00);
    EXPECT_EQ(pkt2Dbc, 0x08);
    EXPECT_EQ(pkt3Dbc, 0x10);
    EXPECT_EQ(pkt4Dbc, 0x18);
}

TEST(IsochTransmitContext, PrimeSyncFailsWhenRxSeedIsStale) {
    constexpr uint32_t kQueueChannels = 8;
    constexpr uint32_t kAm824Slots = 9;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes =
        ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochAudioTxPipeline pipeline;
    pipeline.SetSharedTxQueue(storage.data(), bytes);
    ASSERT_EQ(pipeline.Configure(/*sid=*/0x00,
                                 /*streamModeRaw=*/1u,
                                 /*requestedChannels=*/kQueueChannels,
                                 /*requestedAm824Slots=*/kAm824Slots,
                                 AudioWireFormat::kAM824),
              kIOReturnSuccess);

    Core::ExternalSyncBridge bridge;
    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0xD8B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(1, std::memory_order_release);

    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);
    EXPECT_FALSE(pipeline.PrimeSyncFromExternalBridge());
}

TEST(IsochTransmitContext, PrimeSyncSucceedsWithFreshQualifiedSeedAfterLiveClockDrops) {
    constexpr uint32_t kQueueChannels = 8;
    constexpr uint32_t kAm824Slots = 9;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes =
        ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochAudioTxPipeline pipeline;
    pipeline.SetSharedTxQueue(storage.data(), bytes);
    ASSERT_EQ(pipeline.Configure(/*sid=*/0x00,
                                 /*streamModeRaw=*/1u,
                                 /*requestedChannels=*/kQueueChannels,
                                 /*requestedAm824Slots=*/kAm824Slots,
                                 AudioWireFormat::kAM824),
              kIOReturnSuccess);

    Core::ExternalSyncBridge bridge;
    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(false, std::memory_order_release);
    bridge.startupQualified.store(true, std::memory_order_release);
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0xD8B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(mach_absolute_time(), std::memory_order_release);

    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);
    EXPECT_TRUE(pipeline.PrimeSyncFromExternalBridge());
}

TEST(IsochTransmitContext, PrimeSyncAllowsQualifiedStartupSeedWithinStartupGraceWindow) {
    constexpr uint32_t kQueueChannels = 8;
    constexpr uint32_t kAm824Slots = 9;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes =
        ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochAudioTxPipeline pipeline;
    pipeline.SetSharedTxQueue(storage.data(), bytes);
    ASSERT_EQ(pipeline.Configure(/*sid=*/0x00,
                                 /*streamModeRaw=*/1u,
                                 /*requestedChannels=*/kQueueChannels,
                                 /*requestedAm824Slots=*/kAm824Slots,
                                 AudioWireFormat::kAM824),
              kIOReturnSuccess);

    ASSERT_TRUE(ASFW::Timing::initializeHostTimebase());
    const uint64_t nowTicks = mach_absolute_time();
    const uint64_t graceTicks =
        ASFW::Timing::nanosToHostTicks(ASFW::Isoch::Core::kExternalSyncStartupSeedGraceNanos);
    ASSERT_GT(graceTicks, 0u);

    Core::ExternalSyncBridge bridge;
    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(false, std::memory_order_release);
    bridge.startupQualified.store(true, std::memory_order_release);
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0xD8B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(nowTicks - (graceTicks / 2), std::memory_order_release);

    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);
    EXPECT_TRUE(pipeline.PrimeSyncFromExternalBridge());
}

TEST(IsochTransmitContext, FirstDataPacketKeepsSeededSytWhenBridgeAdvancesByWholePackets) {
    constexpr uint32_t kQueueChannels = 8;
    constexpr uint32_t kAm824Slots = 9;
    constexpr uint32_t kCapacityFrames = 256;
    const uint64_t bytes =
        ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kCapacityFrames, kQueueChannels);
    std::vector<uint8_t> storage(bytes);

    ASSERT_TRUE(ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(storage.data(),
                                                                    bytes,
                                                                    kCapacityFrames,
                                                                    kQueueChannels));

    IsochAudioTxPipeline pipeline;
    pipeline.SetSharedTxQueue(storage.data(), bytes);
    ASSERT_EQ(pipeline.Configure(/*sid=*/0x00,
                                 /*streamModeRaw=*/1u,
                                 /*requestedChannels=*/kQueueChannels,
                                 /*requestedAm824Slots=*/kAm824Slots,
                                 AudioWireFormat::kAM824),
              kIOReturnSuccess);

    Core::ExternalSyncBridge bridge;
    bridge.active.store(true, std::memory_order_release);
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0x54B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(mach_absolute_time(), std::memory_order_release);

    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);
    ASSERT_TRUE(pipeline.PrimeSyncFromExternalBridge());

    // Simulate RX advancing by two DATA packets before TX emits its first DATA packet.
    bridge.lastPackedRx.store(Core::ExternalSyncBridge::PackRxSample(/*syt=*/0xD4B0,
                                                                     Core::ExternalSyncBridge::kFdf48k,
                                                                     static_cast<uint8_t>(kAm824Slots)),
                              std::memory_order_release);
    bridge.lastUpdateHostTicks.store(mach_absolute_time(), std::memory_order_release);

    const auto pkt0 = pipeline.NextSilentPacket(/*transmitCycle=*/3830);
    const bool pkt0IsData = pkt0.isData;
    const uint16_t pkt0Syt = ReadPacketSyt(pkt0);

    const auto pkt1 = pipeline.NextSilentPacket(/*transmitCycle=*/3831);
    const bool pkt1IsData = pkt1.isData;
    const uint16_t pkt1Syt = ReadPacketSyt(pkt1);

    const auto pkt2 = pipeline.NextSilentPacket(/*transmitCycle=*/3832);
    const bool pkt2IsData = pkt2.isData;
    const uint16_t pkt2Syt = ReadPacketSyt(pkt2);

    EXPECT_FALSE(pkt0IsData);
    EXPECT_TRUE(pkt1IsData);
    EXPECT_TRUE(pkt2IsData);
    EXPECT_EQ(pkt0Syt, 0xFFFF);
    EXPECT_EQ(pkt1Syt, 0x54B0);
    EXPECT_EQ(pkt2Syt, 0x68B0);
}
