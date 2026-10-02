#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Protocols/AVC/FCPResponseRouter.hpp"
#include "DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

namespace {

using ASFW::Async::AsyncStatus;
using ASFW::Async::Testing::DeferredFireWireBus;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::FWDevice;
using ASFW::FW::Generation;
using ASFW::Protocols::AVC::AVCUnit;
using ASFW::Protocols::AVC::FCPResponseRouter;
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

    std::shared_ptr<AVCUnit> Unit(uint64_t) override { return nullptr; }
    std::vector<std::shared_ptr<AVCUnit>> Units() override { return {}; }
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

using Reply = ASFW::AVC::Expected<ASFW::AVC::Response>;

ASFW::AVC::CommandFrame UnitInfo() {
    return *ASFW::AVC::CommandFrame::Make(ASFW::AVC::CommandType::kControl, ASFW::AVC::SubunitAddress::Unit(),
                                          ASFW::AVC::Opcode::kUnitInfo, {});
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
        ConfigROM rom{};
        rom.bib.guid = record.guid;
        rom.gen = record.gen;
        rom.nodeId = record.nodeId;
        (void)routes_.UpsertFromROM(rom, {});

        auto transport = std::make_shared<FCPTransport>();
        FCPTransportConfig config{};
        config.timeoutMs = 10;
        config.maxRetries = 0;
        EXPECT_TRUE(transport->init(&bus_, &bus_, device_.get(), routes_, scheduler_, config));
        return transport;
    }

    DeferredFireWireBus bus_;
    FakeSessionScheduler scheduler_;
    DeviceRegistry routes_;
    std::shared_ptr<FWDevice> device_;
};

TEST_F(FCPResponseRouterTests, RoutesResponseFromWithinRegisteredResponseSpace) {
    auto transport = MakeTransport();
    int completionCount = 0;
    bool replied = false;
    transport->Submit(UnitInfo(), Generation{1}, [&completionCount, &replied](Reply reply) {
        ++completionCount;
        replied = reply.has_value();
    });
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
    // Our write response goes out when RouteBlockWrite returns; the command
    // completes, and may submit the next one, only after that (AV/C General
    // 4.2 §6.5).
    EXPECT_EQ(completionCount, 0);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
    EXPECT_TRUE(replied);
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
    transport->Submit(UnitInfo(), Generation{1}, [&completionCount](Reply) { ++completionCount; });
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
    EXPECT_EQ(completionCount, 0);
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
    EXPECT_TRUE(weakTransport.expired());
}

TEST_F(FCPResponseRouterTests, DoesNotRouteResponseWithoutCapturedGeneration) {
    auto transport = MakeTransport();
    int completionCount = 0;
    transport->Submit(UnitInfo(), Generation{1}, [&completionCount](Reply) { ++completionCount; });
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
    scheduler_.Advance(0);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(discovery.AcquiredNodeID(), 2);
}

} // namespace
