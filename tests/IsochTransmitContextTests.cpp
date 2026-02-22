// IsochTransmitContextTests.cpp
// ASFW - Host-safe unit tests for transmit packetization behavior
//
// NOTE:
// Full IsochTransmitContext runtime tests require DriverKit DMA/hardware wiring.
// In ASFW_HOST_TEST, we validate the same cadence/DBC/underrun behavior through
// PacketAssembler, plus a lightweight API state smoke test.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "../ASFWDriver/Isoch/Transmit/IsochTransmitContext.hpp"
#include "../ASFWDriver/Shared/TxSharedQueue.hpp"

using namespace ASFW::Isoch;
using namespace ASFW::Encoding;

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
