#include <gtest/gtest.h>

#include "ASFWDriver/ConfigROM/ROMScannerEventBus.hpp"

#include <vector>

namespace ASFW::Discovery {

TEST(ROMScannerEventBusTests, DrainsInPublishOrder) {
    ROMScannerEventBus bus;

    ROMScannerEvent first{};
    first.type = ROMScannerEventType::BIBComplete;
    first.payload.nodeId = 3;
    first.payload.generation = 11;

    ROMScannerEvent second{};
    second.type = ROMScannerEventType::RootDirComplete;
    second.payload.nodeId = 9;
    second.payload.generation = 11;

    bus.Publish(std::move(first));
    bus.Publish(std::move(second));

    std::vector<uint8_t> drainedNodeIds;
    bus.Drain([&drainedNodeIds](const ROMScannerEvent& event) {
        drainedNodeIds.push_back(event.payload.nodeId);
    });

    ASSERT_EQ(drainedNodeIds.size(), 2u);
    EXPECT_EQ(drainedNodeIds[0], 3u);
    EXPECT_EQ(drainedNodeIds[1], 9u);
}

TEST(ROMScannerEventBusTests, ClearDropsPendingEvents) {
    ROMScannerEventBus bus;

    ROMScannerEvent event{};
    event.type = ROMScannerEventType::IRMReadComplete;
    event.payload.nodeId = 4;
    event.payload.generation = 17;

    bus.Publish(std::move(event));
    bus.Clear();

    size_t count = 0;
    bus.Drain([&count](const ROMScannerEvent&) {
        ++count;
    });

    EXPECT_EQ(count, 0u);
}

} // namespace ASFW::Discovery
