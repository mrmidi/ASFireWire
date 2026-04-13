#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Isoch/Encoding/TimingUtils.hpp"
#include "Isoch/Receive/IsochAudioRxPipeline.hpp"

namespace {

using ASFW::Driver::HardwareInterface;
using ASFW::Isoch::Core::ExternalSyncBridge;
using ASFW::Isoch::Rx::IsochAudioRxPipeline;

class IsochAudioRxPipelineTests : public ::testing::Test {
};

TEST_F(IsochAudioRxPipelineTests, TimingLossCallbackFiresOncePerEstablishedToStaleTransition) {
    IsochAudioRxPipeline pipeline;
    ExternalSyncBridge bridge;
    HardwareInterface hardware;
    int callbackCount = 0;

    pipeline.ConfigureFor48k();
    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.SetTimingLossCallback([&callbackCount] { ++callbackCount; });
    pipeline.OnStart();

    const uint64_t nowTicks = mach_absolute_time();
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.startupQualified.store(true, std::memory_order_release);
    bridge.lastUpdateHostTicks.store(
        nowTicks - ASFW::Timing::nanosToHostTicks(150'000'000ULL),
        std::memory_order_release);

    pipeline.OnPollEnd(hardware, /*packetsProcessed=*/0, /*pollStartMachTicks=*/nowTicks);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));

    pipeline.OnPollEnd(hardware, /*packetsProcessed=*/0, /*pollStartMachTicks=*/nowTicks);
    EXPECT_EQ(callbackCount, 1);
}

} // namespace
