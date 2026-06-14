#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "../ASFWDriver/Hardware/HardwareInterface.hpp"
#include "../ASFWDriver/Isoch/IsochService.hpp"
#include "../ASFWDriver/Isoch/Transmit/IsochTxLayout.hpp"
#include "../ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp"
#include "../ASFWDriver/Shared/Isoch/IsochAudioTransport.hpp"

namespace {

using ASFW::Driver::HardwareInterface;
using ASFW::Driver::IsochService;
using ASFW::Driver::Register32;
using ASFW::Isoch::Tx::Layout;
using ASFW::IsochTransport::AudioTimingGeometry;
using ASFW::IsochTransport::ExpectedCommitGen;
using ASFW::IsochTransport::TxPacketMeta;
using ASFW::IsochTransport::TxStreamControl;

TEST(IsochServiceTxPreparation,
     CallbackRegisteredBeforeContextCreationSurvivesStartTransmit) {
    IsochService service;
    HardwareInterface hardware;

    uint32_t callbackCount = 0;
    uint64_t callbackGeneration = 0;
    service.SetTxPreparationCallback(
        [&](uint64_t generation) {
            ++callbackCount;
            callbackGeneration = generation;
        });

    IOMemoryDescriptor* payloadDescriptor = nullptr;
    IOMemoryDescriptor* metadataDescriptor = nullptr;
    IOMemoryDescriptor* controlDescriptor = nullptr;
    ASSERT_EQ(
        service.AllocateTxIsochResources(
            AudioTimingGeometry::kTxSharedSlotPackets,
            512,
            AudioTimingGeometry::kTxPacketsPerGroup,
            &payloadDescriptor,
            &metadataDescriptor,
            &controlDescriptor),
        kIOReturnSuccess);
    ASSERT_NE(payloadDescriptor, nullptr);
    ASSERT_NE(metadataDescriptor, nullptr);
    ASSERT_NE(controlDescriptor, nullptr);

    IOAddressSegment metadataRange{};
    ASSERT_EQ(metadataDescriptor->GetAddressRange(&metadataRange),
              kIOReturnSuccess);
    std::memset(reinterpret_cast<void*>(metadataRange.address),
                0,
                metadataRange.length);
    auto* metadata =
        reinterpret_cast<TxPacketMeta*>(metadataRange.address);
    for (uint64_t packetIndex = 0;
         packetIndex < AudioTimingGeometry::kTxSharedSlotPackets;
         ++packetIndex) {
        auto& meta =
            metadata[packetIndex %
                     AudioTimingGeometry::kTxSharedSlotPackets];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGen.store(
            ExpectedCommitGen(
                packetIndex,
                AudioTimingGeometry::kTxSharedSlotPackets),
            std::memory_order_release);
    }

    IOAddressSegment controlRange{};
    ASSERT_EQ(controlDescriptor->GetAddressRange(&controlRange),
              kIOReturnSuccess);
    std::memset(reinterpret_cast<void*>(controlRange.address),
                0,
                controlRange.length);
    auto* control =
        reinterpret_cast<TxStreamControl*>(controlRange.address);
    control->exposeCursor.store(
        AudioTimingGeometry::kTxPreparationLeadPackets,
        std::memory_order_release);

    ASSERT_EQ(service.StartTransmit(/*channel=*/3,
                                    hardware,
                                    /*sid=*/0x3f),
              kIOReturnSuccess);
    auto* context = service.TransmitContext();
    ASSERT_NE(context, nullptr);

    const Register32 commandPtrRegister =
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0));
    const uint32_t initialCommandPtr =
        hardware.GetTestRegister(commandPtrRegister);
    EXPECT_EQ(initialCommandPtr & 0xfU, Layout::kBlocksPerPacket);
    const uint32_t descriptorBase = initialCommandPtr & 0xfffffff0U;
    const uint32_t completedPackets =
        AudioTimingGeometry::kTxPacketsPerGroup;
    const uint32_t nextCommandPtr =
        descriptorBase +
        completedPackets *
            Layout::kBlocksPerPacket *
            Layout::kDescriptorStride;
    hardware.SetTestRegister(
        commandPtrRegister,
        nextCommandPtr | Layout::kBlocksPerPacket);

    context->HandleInterrupt();

    EXPECT_EQ(callbackCount, 1U);
    EXPECT_EQ(callbackGeneration, 1U);
    EXPECT_EQ(
        control->preparationRequestGeneration.load(
            std::memory_order_acquire),
        1U);
}

} // namespace
