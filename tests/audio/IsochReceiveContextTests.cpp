#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Isoch/IsochReceiveContext.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Audio/DriverKit/Runtime/PayloadWriterTelemetry.hpp"

// Use a mock or stub for HardwareInterface
namespace ASFW::Driver {
// We can use the Stub linked from HardwareInterfaceStub.cpp
// Or define a mock here if we need to verify register writes
}

using namespace ASFW::Isoch;
using namespace ASFW::Shared;
using namespace ASFW::Async;
using namespace ASFW::Isoch::Memory;
using namespace testing;

TEST(PayloadWriterTelemetryTests,
     HistoricalStartupCountersDoNotMakeTheHealthySteadyStateNoisy) {
    using ASFW::Audio::Runtime::PayloadWriterTelemetryAnomalyAggregator;
    using ASFW::Audio::Runtime::PayloadWriterTelemetryRecord;

    PayloadWriterTelemetryAnomalyAggregator aggregator;
    PayloadWriterTelemetryRecord record{};
    record.visited = 1'000;
    record.written = 900;
    record.withoutPacket = 100;

    aggregator.BeginDrain();
    aggregator.Observe(record);
    EXPECT_FALSE(aggregator.Summary().HasAnomaly());

    aggregator.BeginDrain();
    record.visited += 512;
    record.written += 512;
    aggregator.Observe(record);
    EXPECT_FALSE(aggregator.Summary().HasAnomaly());

    aggregator.BeginDrain();
    ++record.visited;
    ++record.withoutPacket;
    aggregator.Observe(record);
    EXPECT_TRUE(aggregator.Summary().HasAnomaly());
    EXPECT_EQ(aggregator.Summary().withoutPacketDelta, 1u);
    EXPECT_EQ(aggregator.Summary().visitedDelta, 1u);
    EXPECT_EQ(aggregator.Summary().writtenDelta, 0u);
}

TEST(PayloadWriterTelemetryTests, DrainVisitsAllRetainedRecordsOffTheCallbackPath) {
    using ASFW::Audio::Runtime::PayloadWriterTelemetryRecord;
    using ASFW::Audio::Runtime::PayloadWriterTelemetryRing;

    PayloadWriterTelemetryRing ring;
    for (uint64_t sampleTime = 1; sampleTime <= 3; ++sampleTime) {
        PayloadWriterTelemetryRecord record{};
        record.sampleTime = sampleTime;
        ring.Record(record);
    }

    uint64_t visited = 0;
    EXPECT_EQ(ring.Drain([&visited](const PayloadWriterTelemetryRecord&) {
                  ++visited;
              }),
              0u);
    EXPECT_EQ(visited, 3u);
    EXPECT_EQ(ring.PendingCount(), 0u);
}

TEST(ZtsTelemetryLogGateTests,
     EmitsSeedThenEveryFourSecondsAndRearmsOnGridReset) {
    constexpr uint32_t kRate = 48000;
    constexpr uint64_t kStartFrame = 1536;
    ASFW::Isoch::Rx::ZtsTelemetryLogGate gate;
    ASFW::Isoch::Rx::ZtsTelemetryRecord record{};

    record.kind = static_cast<uint8_t>(ASFW::Isoch::Rx::ZtsEventKind::kSeed);
    record.sampleFrame = kStartFrame;
    EXPECT_TRUE(gate.ShouldEmit(record, kRate));

    record.kind = static_cast<uint8_t>(ASFW::Isoch::Rx::ZtsEventKind::kUpdate);
    record.sampleFrame =
        kStartFrame + kRate *
            ASFW::Isoch::Rx::ZtsTelemetryLogGate::kIntervalSeconds - 1;
    EXPECT_FALSE(gate.ShouldEmit(record, kRate));

    record.sampleFrame =
        kStartFrame + kRate *
            ASFW::Isoch::Rx::ZtsTelemetryLogGate::kIntervalSeconds;
    EXPECT_TRUE(gate.ShouldEmit(record, kRate));

    ++record.sampleFrame;
    EXPECT_FALSE(gate.ShouldEmit(record, kRate));

    record.sampleFrame = 0;
    EXPECT_TRUE(gate.ShouldEmit(record, kRate));
}

class IsochReceiveContextTest : public Test {
protected:
    void SetUp() override {
        // Create dependencies
        hardware_ = new ::ASFW::Driver::HardwareInterface(); // Using the Stub
        
        IsochMemoryConfig config;
        config.numDescriptors = 512;
        config.packetSizeBytes = 4096;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 4096;
        
        // Use concrete class for testing explicit init, but pass as interface
        auto concreteMgr = IsochDMAMemoryManager::Create(config);
        ASSERT_TRUE(concreteMgr->Initialize(*hardware_));
        dmaMemory_ = concreteMgr;
        
        context_ = IsochReceiveContext::Create(hardware_, dmaMemory_);
        ASSERT_TRUE(context_);
    }

    void TearDown() override {
        if (context_) {
            (void)context_->Stop();
            context_.reset();
        }
        if (hardware_) {
            delete hardware_;
            hardware_ = nullptr;
        }
    }

    ::ASFW::Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<IIsochDMAMemory> dmaMemory_;
    std::unique_ptr<IsochReceiveContext> context_;
};

TEST_F(IsochReceiveContextTest, Initialization) {
    EXPECT_TRUE(context_);
}

TEST_F(IsochReceiveContextTest, ConfigurationAllocatesRings) {
    // Configure Channel 0, Context 0
    auto kr = context_->Configure(0, 0);
    EXPECT_EQ(kr, kIOReturnSuccess);
    
    // Verify rings were allocated in Fake Memory
    // 512 descriptors * 16 bytes = 8192
    // 512 buffers * 4096 bytes = 2MB
    // Total should be > 2MB
    // Verify rings were allocated
    // Check Available logic? 
    // Total size should be allocated blocks.
    // Available should decrease?
    // IsochDMAMemoryManager uses cursors. 
    // Allocation happens at Configure->SetupRings time.
    
    // We can check TotalSize() which is sum of slabs.
    EXPECT_GT(dmaMemory_->TotalSize(), 2000000);
}

TEST_F(IsochReceiveContextTest, StartProgramsRegisters) {
    context_->Configure(0, 0);
    auto kr = context_->Start();
    EXPECT_EQ(kr, kIOReturnSuccess);
    
    // We would need to inspect HardwareInterface stub state to verify writes
    // Since we are using the simple Stub, we trust it returns success.
    // Ideally we'd use a MockHardwareInterface to verify ::Write calls.
}

TEST_F(IsochReceiveContextTest, StopFlushesRunClearAndOnlyThenMarksStopped) {
    ASSERT_EQ(context_->Configure(0, 0), kIOReturnSuccess);
    ASSERT_EQ(context_->Start(), kIOReturnSuccess);

    const auto controlSet = static_cast<::ASFW::Driver::Register32>(
        ::DMAContextHelpers::IsoRcvContextControlSet(0));
    hardware_->SetTestRegister(controlSet, 0);

    EXPECT_EQ(context_->Stop(), kIOReturnSuccess);
    EXPECT_EQ(context_->GetState(), IRPolicy::State::Stopped);

    const auto operations = hardware_->CopyTestOperations();
    EXPECT_THAT(operations, Contains(::ASFW::Driver::HardwareInterface::TestOperation::WriteAndFlush));
}

TEST_F(IsochReceiveContextTest, StopRetainsBindingStateWhenActiveNeverClears) {
    ASSERT_EQ(context_->Configure(0, 0), kIOReturnSuccess);
    ASSERT_EQ(context_->Start(), kIOReturnSuccess);

    const auto controlSet = static_cast<::ASFW::Driver::Register32>(
        ::DMAContextHelpers::IsoRcvContextControlSet(0));
    hardware_->SetTestRegister(controlSet, ::ASFW::Driver::ContextControl::kActive);

    EXPECT_EQ(context_->Stop(), kIOReturnTimeout);
    EXPECT_EQ(context_->GetState(), IRPolicy::State::Running);
}

TEST_F(IsochReceiveContextTest, PollProcessesPackets) {
    context_->Configure(0, 0);
    context_->Start();
    
    // Inject a packet into the buffer ring
    // 1. Get the descriptor ring allocation
    // 2. Modify descriptor 0 to have status != 0
    
    // For now, testing that Poll returns 0 on empty rings
    EXPECT_EQ(context_->Poll(), 0);
    
    // TODO: Advanced test - Write to FakeMemory to simulate packet arrival
    // This requires knowing the addresses allocated.
    // IsochReceiveContext doesn't expose rings directly.
}
