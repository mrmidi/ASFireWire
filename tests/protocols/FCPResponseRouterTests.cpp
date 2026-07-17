#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Protocols/AVC/FCPResponseRouter.hpp"
#include "DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

namespace {

using ASFW::Async::AsyncStatus;
using ASFW::Async::Testing::DeferredFireWireBus;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::FWDevice;
using ASFW::FW::Generation;
using ASFW::Protocols::AVC::AVCUnit;
using ASFW::Protocols::AVC::FCPFrame;
using ASFW::Protocols::AVC::FCPResponseRouter;
using ASFW::Protocols::AVC::FCPStatus;
using ASFW::Protocols::AVC::FCPTransport;
using ASFW::Protocols::AVC::FCPTransportConfig;
using ASFW::Protocols::AVC::IAVCDiscovery;
using ASFW::Protocols::AVC::kFCPResponseAddress;
using ASFW::Protocols::AVC::kFCPResponseAddressEnd;
using ASFW::Protocols::Ports::BlockWriteDisposition;
using ASFW::Protocols::Ports::BlockWriteRequestView;
using ASFW::Testing::FakeSessionScheduler;

class OneShotDiscovery final : public IAVCDiscovery {
public:
    explicit OneShotDiscovery(std::shared_ptr<FCPTransport> transport)
        : transport_(std::move(transport)) {}

    std::vector<AVCUnit*> GetAllAVCUnits() override { return {}; }
    void ReScanAllUnits() override {}
    FCPTransport* GetFCPTransportForNodeID(uint16_t) override { return nullptr; }

    std::shared_ptr<FCPTransport> AcquireFCPTransportForNodeID(uint16_t nodeID) override {
        acquiredNodeID_ = nodeID;
        return std::move(transport_);
    }

    [[nodiscard]] uint16_t AcquiredNodeID() const noexcept { return acquiredNodeID_; }

private:
    std::shared_ptr<FCPTransport> transport_;
    uint16_t acquiredNodeID_{0};
};

FCPFrame MakeUnitInfoCommand() {
    FCPFrame command{};
    command.length = 3;
    command.data[0] = 0x00;
    command.data[1] = 0xFF;
    command.data[2] = 0x30;
    return command;
}

class FCPResponseRouterTests : public ::testing::Test {
protected:
    std::shared_ptr<FCPTransport> MakeTransport() {
        DeviceRecord record{};
        record.guid = 0x0001020304050607ULL;
        record.nodeId = 2;
        record.gen = Generation{1};
        device_ = FWDevice::Create(record, ConfigROM{});
        EXPECT_NE(device_, nullptr);

        auto transport = std::make_shared<FCPTransport>();
        FCPTransportConfig config{};
        config.timeoutMs = 10;
        config.maxRetries = 0;
        EXPECT_TRUE(transport->init(&bus_, &bus_, device_.get(), scheduler_, config));
        return transport;
    }

    DeferredFireWireBus bus_;
    FakeSessionScheduler scheduler_;
    std::shared_ptr<FWDevice> device_;
};

TEST_F(FCPResponseRouterTests, RoutesResponseFromWithinRegisteredResponseSpace) {
    auto transport = MakeTransport();
    int completionCount = 0;
    FCPStatus completionStatus = FCPStatus::kTransportError;
    ASSERT_TRUE(transport->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount, &completionStatus](FCPStatus status, const FCPFrame&) {
                                 ++completionCount;
                                 completionStatus = status;
                             })
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    const std::weak_ptr<FCPTransport> weakTransport = transport;
    OneShotDiscovery discovery(std::move(transport));
    FCPResponseRouter router(discovery);
    constexpr std::array<uint8_t, 3> response{0x0C, 0xFF, 0x30};
    const BlockWriteRequestView request{
        .sourceID = 2,
        .destOffset = kFCPResponseAddress + 4,
        .generation = 1,
        .payload = response,
    };

    EXPECT_EQ(router.RouteBlockWrite(request), BlockWriteDisposition::kComplete);
    EXPECT_EQ(discovery.AcquiredNodeID(), 2);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(completionStatus, FCPStatus::kOk);
    EXPECT_TRUE(weakTransport.expired());
}

TEST_F(FCPResponseRouterTests, RejectsWritesOutsideResponseSpace) {
    OneShotDiscovery discovery(nullptr);
    FCPResponseRouter router(discovery);
    constexpr std::array<uint8_t, 3> response{0x0C, 0xFF, 0x30};

    const BlockWriteRequestView before{
        .sourceID = 2,
        .destOffset = kFCPResponseAddress - 1,
        .generation = 1,
        .payload = response,
    };
    EXPECT_EQ(router.RouteBlockWrite(before), BlockWriteDisposition::kAddressError);

    const BlockWriteRequestView after{
        .sourceID = 2,
        .destOffset = kFCPResponseAddressEnd,
        .generation = 1,
        .payload = response,
    };
    EXPECT_EQ(router.RouteBlockWrite(after), BlockWriteDisposition::kAddressError);
}

TEST_F(FCPResponseRouterTests, UsesCapturedGenerationRatherThanCurrentBusGeneration) {
    auto transport = MakeTransport();
    int completionCount = 0;
    ASSERT_TRUE(transport->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; })
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    const std::weak_ptr<FCPTransport> weakTransport = transport;
    OneShotDiscovery discovery(std::move(transport));
    FCPResponseRouter router(discovery);
    bus_.SetGeneration(Generation{2});
    constexpr std::array<uint8_t, 3> response{0x0C, 0xFF, 0x30};
    const BlockWriteRequestView request{
        .sourceID = 2,
        .destOffset = kFCPResponseAddress,
        .generation = 1,
        .payload = response,
    };

    EXPECT_EQ(router.RouteBlockWrite(request), BlockWriteDisposition::kComplete);
    EXPECT_EQ(completionCount, 1);
    EXPECT_TRUE(weakTransport.expired());
}

TEST_F(FCPResponseRouterTests, DoesNotRouteResponseWithoutCapturedGeneration) {
    auto transport = MakeTransport();
    int completionCount = 0;
    ASSERT_TRUE(transport->SubmitCommand(
                             MakeUnitInfoCommand(),
                             [&completionCount](FCPStatus, const FCPFrame&) { ++completionCount; })
                    .IsValid());
    ASSERT_TRUE(bus_.CompleteNextWrite(AsyncStatus::kSuccess));

    OneShotDiscovery discovery(transport);
    FCPResponseRouter router(discovery);
    constexpr std::array<uint8_t, 3> response{0x0C, 0xFF, 0x30};
    const BlockWriteRequestView request{
        .sourceID = 2,
        .destOffset = kFCPResponseAddress,
        .payload = response,
    };

    EXPECT_EQ(router.RouteBlockWrite(request), BlockWriteDisposition::kComplete);
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(discovery.AcquiredNodeID(), 0);

    const BlockWriteRequestView taggedRequest{
        .sourceID = 2,
        .destOffset = kFCPResponseAddress,
        .generation = 1,
        .payload = response,
    };
    EXPECT_EQ(router.RouteBlockWrite(taggedRequest), BlockWriteDisposition::kComplete);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(discovery.AcquiredNodeID(), 2);
}

} // namespace
