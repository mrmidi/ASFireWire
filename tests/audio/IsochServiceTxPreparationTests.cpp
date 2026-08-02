#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "../ASFWDriver/Hardware/HardwareInterface.hpp"
#include "../ASFWDriver/Isoch/IsochService.hpp"
#include "../ASFWDriver/Isoch/Transmit/IsochTxLayout.hpp"
#include "../ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp"
#include "../ASFWDriver/Isoch/Core/IsochTxQueue.hpp"

namespace {

using ASFW::Driver::HardwareInterface;
using ASFW::Driver::IsochService;
using ASFW::Driver::Register32;
using ASFW::Isoch::Tx::Layout;
using ASFW::IsochTransport::AudioTimingGeometry;
using ASFW::Isoch::ExpectedTxCommitGeneration;
using ASFW::Isoch::IsochTxPacketMeta;
using ASFW::Isoch::IsochTxQueueControl;

class RecordingReceiveConsumer final : public ASFW::Isoch::IIsochReceiveConsumer {
  public:
    void OnReceiveActivated() noexcept override { ++activated; }
    void OnReceiveQuiesced() noexcept override { ++quiesced; }
    void BeginReceiveBatch(const ASFW::Isoch::IsochReceiveBatch&) noexcept override {
        ++batches;
    }
    void ConsumePacket(const ASFW::Isoch::IsochReceiveBatch&,
                       const ASFW::Isoch::IsochReceivePacket&) noexcept override {}

    uint32_t activated{0};
    uint32_t quiesced{0};
    uint32_t batches{0};
};

TEST(IsochServiceTxPreparation, CallbackRegisteredBeforeContextCreationSurvivesStartTransmit) {
    IsochService service;
    HardwareInterface hardware;

    uint32_t callbackCount = 0;
    uint64_t callbackGeneration = 0;
    service.SetTxPreparationCallback([&](uint64_t generation) {
        ++callbackCount;
        callbackGeneration = generation;
    });

    IOMemoryDescriptor* payloadDescriptor = nullptr;
    IOMemoryDescriptor* metadataDescriptor = nullptr;
    IOMemoryDescriptor* controlDescriptor = nullptr;
    ASSERT_EQ(service.AllocateTxIsochResources(
                  /*streamIndex=*/0, AudioTimingGeometry::kTxSharedSlotPackets, 512,
                  AudioTimingGeometry::kTxPacketsPerGroup, &payloadDescriptor, &metadataDescriptor,
                  &controlDescriptor),
              kIOReturnSuccess);
    ASSERT_NE(payloadDescriptor, nullptr);
    ASSERT_NE(metadataDescriptor, nullptr);
    ASSERT_NE(controlDescriptor, nullptr);

    IOAddressSegment metadataRange{};
    ASSERT_EQ(metadataDescriptor->GetAddressRange(&metadataRange), kIOReturnSuccess);
    std::memset(reinterpret_cast<void*>(metadataRange.address), 0, metadataRange.length);
    auto* metadata = reinterpret_cast<IsochTxPacketMeta*>(metadataRange.address);
    for (uint64_t packetIndex = 0; packetIndex < AudioTimingGeometry::kTxSharedSlotPackets;
         ++packetIndex) {
        auto& meta = metadata[packetIndex % AudioTimingGeometry::kTxSharedSlotPackets];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGeneration.store(
            ExpectedTxCommitGeneration(packetIndex, AudioTimingGeometry::kTxSharedSlotPackets),
            std::memory_order_release);
    }

    IOAddressSegment controlRange{};
    ASSERT_EQ(controlDescriptor->GetAddressRange(&controlRange), kIOReturnSuccess);
    std::memset(reinterpret_cast<void*>(controlRange.address), 0, controlRange.length);
    auto* control = reinterpret_cast<IsochTxQueueControl*>(controlRange.address);
    control->committedEnd.store(AudioTimingGeometry::kTxPreparationLeadPackets,
                                std::memory_order_release);

    ASSERT_EQ(service.StartTransmit(/*channel=*/3, hardware,
                                    /*sid=*/0x3f),
              kIOReturnSuccess);
    auto* context = service.TransmitContext();
    ASSERT_NE(context, nullptr);

    const Register32 commandPtrRegister =
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0));
    const uint32_t initialCommandPtr = hardware.GetTestRegister(commandPtrRegister);
    EXPECT_EQ(initialCommandPtr & 0xfU, Layout::kBlocksPerPacket);
    const uint32_t descriptorBase = initialCommandPtr & 0xfffffff0U;
    const uint32_t completedPackets = AudioTimingGeometry::kTxPacketsPerGroup;
    const uint32_t nextCommandPtr =
        descriptorBase + completedPackets * Layout::kBlocksPerPacket * Layout::kDescriptorStride;
    hardware.SetTestRegister(commandPtrRegister, nextCommandPtr | Layout::kBlocksPerPacket);

    context->HandleInterrupt();

    EXPECT_EQ(callbackCount, 1U);
    EXPECT_EQ(callbackGeneration, 1U);
    EXPECT_EQ(control->refillRequestGeneration.load(std::memory_order_acquire), 1U);
}

TEST(IsochServiceTxPreparation, ActiveTransmitStopRetainsQueueUntilHardwareQuiesces) {
    IsochService service;
    HardwareInterface hardware;
    IOMemoryDescriptor* payloadDescriptor = nullptr;
    IOMemoryDescriptor* metadataDescriptor = nullptr;
    IOMemoryDescriptor* controlDescriptor = nullptr;
    ASSERT_EQ(service.AllocateTxIsochResources(
                  0, AudioTimingGeometry::kTxSharedSlotPackets, 512,
                  AudioTimingGeometry::kTxPacketsPerGroup, &payloadDescriptor,
                  &metadataDescriptor, &controlDescriptor),
              kIOReturnSuccess);

    IOAddressSegment metadataRange{};
    ASSERT_EQ(metadataDescriptor->GetAddressRange(&metadataRange), kIOReturnSuccess);
    auto* metadata = reinterpret_cast<IsochTxPacketMeta*>(metadataRange.address);
    for (uint64_t packetIndex = 0;
         packetIndex < AudioTimingGeometry::kTxSharedSlotPackets;
         ++packetIndex) {
        auto& meta = metadata[packetIndex % AudioTimingGeometry::kTxSharedSlotPackets];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGeneration.store(
            ExpectedTxCommitGeneration(
                packetIndex, AudioTimingGeometry::kTxSharedSlotPackets),
            std::memory_order_release);
    }
    IOAddressSegment controlRange{};
    ASSERT_EQ(controlDescriptor->GetAddressRange(&controlRange), kIOReturnSuccess);
    auto* queue = reinterpret_cast<IsochTxQueueControl*>(controlRange.address);
    queue->ResetProducerForStart();
    queue->committedEnd.store(AudioTimingGeometry::kTxPreparationLeadPackets,
                              std::memory_order_release);

    ASSERT_EQ(service.StartTransmit(3, hardware, 0x3f), kIOReturnSuccess);
    auto* context = service.TransmitContext();
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->GetState(), ASFW::Isoch::ITState::Running);

    const Register32 controlSet = static_cast<Register32>(
        DMAContextHelpers::IsoXmitContextControlSet(0));
    hardware.SetTestRegister(controlSet, ASFW::Driver::ContextControl::kActive);
    EXPECT_EQ(service.StopAll(), kIOReturnTimeout);
    EXPECT_EQ(context->GetState(), ASFW::Isoch::ITState::Running);

    hardware.SetTestRegister(controlSet, 0);
    EXPECT_EQ(service.StopAll(), kIOReturnSuccess);
    EXPECT_EQ(context->GetState(), ASFW::Isoch::ITState::Stopped);
}

// Secondary-stream container: a multi-stream DICE device (Venice F32 = 2×16)
// needs IsochService to manage a second IR and second IT context on their own
// OHCI context indices, while the master (stream 0) is untouched. This pass only
// builds the container + records the de-interleave channel offset; the engine
// wires the decode/slab later.
TEST(IsochServiceTxPreparation, SecondaryStreamRejectsIndexZeroAndOutOfRange) {
    IsochService service;
    HardwareInterface hardware;

    // Index 0 is the master — must go through PrepareReceive/PrepareTransmit.
    EXPECT_EQ(service.PrepareReceiveStream(0, /*channel=*/1, hardware,
                                           /*offset=*/0, /*streamChannels=*/16),
              kIOReturnBadArgument);
    EXPECT_EQ(service.PrepareTransmitStream(0, /*channel=*/0, hardware, /*sid=*/0x3f),
              kIOReturnBadArgument);
    // Out of range.
    EXPECT_EQ(service.PrepareReceiveStream(IsochService::kMaxStreamsPerDirection, 2, hardware,
                                           16, 16),
              kIOReturnBadArgument);
}

TEST(IsochServiceTxPreparation, SecondaryReceiveStreamCreatesContextAndRecordsOffset) {
    IsochService service;
    HardwareInterface hardware;

    EXPECT_EQ(service.ReceiveContext(1), nullptr);

    ASSERT_EQ(service.PrepareReceiveStream(/*streamIndex=*/1, /*channel=*/2, hardware,
                                           /*channelOffset=*/16, /*streamChannels=*/16),
              kIOReturnSuccess);

    // Master untouched; secondary now exists and its de-interleave offset is recorded.
    EXPECT_EQ(service.ReceiveContext(0), nullptr);
    EXPECT_NE(service.ReceiveContext(1), nullptr);
    EXPECT_EQ(service.CaptureStreamChannelOffset(0), 0u);
    EXPECT_EQ(service.CaptureStreamChannelOffset(1), 16u);

    // StopAll tears the whole service down without touching the (absent) master.
    EXPECT_EQ(service.StopAll(), kIOReturnSuccess);
}

TEST(IsochServiceTxPreparation, StopAllPropagatesActiveReceiveTimeoutAndRetainsContext) {
    IsochService service;
    HardwareInterface hardware;

    ASSERT_EQ(service.PrepareReceive(/*channel=*/2, hardware),
              kIOReturnSuccess);
    ASSERT_EQ(service.StartPreparedReceive(), kIOReturnSuccess);

    const Register32 controlSet =
        static_cast<Register32>(DMAContextHelpers::IsoRcvContextControlSet(0));
    hardware.SetTestRegister(controlSet, ASFW::Driver::ContextControl::kActive);

    EXPECT_EQ(service.StopAll(), kIOReturnTimeout);
    ASSERT_NE(service.ReceiveContext(0), nullptr);
    EXPECT_EQ(service.ReceiveContext(0)->GetState(), ASFW::Isoch::IRPolicy::State::Running);
}

TEST(IsochServiceTxPreparation, ReceiveConsumerAttachesBeforePreparedStart) {
    IsochService service;
    HardwareInterface hardware;
    RecordingReceiveConsumer consumer;

    service.SetReceiveConsumer(/*streamIndex=*/0, &consumer);
    ASSERT_EQ(service.PrepareReceive(/*channel=*/2, hardware),
              kIOReturnSuccess);
    ASSERT_EQ(service.StartPreparedReceive(), kIOReturnSuccess);
    ASSERT_NE(service.ReceiveContext(), nullptr);
    EXPECT_EQ(consumer.activated, 1u);

    EXPECT_EQ(service.ReceiveContext()->Poll(), 0u);
    EXPECT_EQ(consumer.batches, 1u);

    EXPECT_EQ(service.StopReceive(), kIOReturnSuccess);
    EXPECT_EQ(consumer.quiesced, 1u);
}

TEST(IsochServiceTxPreparation, SecondaryTransmitStreamCreatesIndependentContext) {
    IsochService service;
    HardwareInterface hardware;

    // The secondary stream's shared slab must be allocated before its context is
    // prepared (StartIO allocates, the duplex bringup then wires the context).
    IOMemoryDescriptor* payloadDescriptor = nullptr;
    IOMemoryDescriptor* metadataDescriptor = nullptr;
    IOMemoryDescriptor* controlDescriptor = nullptr;
    ASSERT_EQ(service.AllocateTxIsochResources(
                  /*streamIndex=*/1, AudioTimingGeometry::kTxSharedSlotPackets, 512,
                  AudioTimingGeometry::kTxPacketsPerGroup, &payloadDescriptor, &metadataDescriptor,
                  &controlDescriptor),
              kIOReturnSuccess);

    ASSERT_EQ(service.PrepareTransmitStream(/*streamIndex=*/1, /*channel=*/4, hardware,
                                            /*sid=*/0x3f),
              kIOReturnSuccess);

    EXPECT_EQ(service.TransmitContext(0), nullptr); // master not created
    EXPECT_NE(service.TransmitContext(1), nullptr); // secondary created
    EXPECT_NE(service.TransmitContext(1), service.TransmitContext(0));

    EXPECT_EQ(service.StopAll(), kIOReturnSuccess);

    if (payloadDescriptor)
        payloadDescriptor->release();
    if (metadataDescriptor)
        metadataDescriptor->release();
    if (controlDescriptor)
        controlDescriptor->release();
}

} // namespace
