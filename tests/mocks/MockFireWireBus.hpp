#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include <gmock/gmock.h>

namespace ASFW::Async::Mocks {

/**
 * @brief Mock FireWire bus for unit testing.
 *
 * Provides gmock-based expectations for all IFireWireBus operations.
 * Use this for precise behavior verification in unit tests.
 */
class MockFireWireBus : public IFireWireBus {
public:
    // =============================================================================
    // IFireWireBusOps - Bus Operations
    // =============================================================================

    MOCK_METHOD(AsyncHandle, ReadBlock,
                (FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                 uint32_t length, FW::FwSpeed speed, InterfaceCompletionCallback callback),
                (override));

    MOCK_METHOD(AsyncHandle, WriteBlock,
                (FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                 std::span<const uint8_t> data, FW::FwSpeed speed,
                 InterfaceCompletionCallback callback),
                (override));

    MOCK_METHOD(AsyncHandle, Lock,
                (FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                 FW::LockOp lockOp, std::span<const uint8_t> operand, uint32_t responseLength,
                 FW::FwSpeed speed, InterfaceCompletionCallback callback),
                (override));

    MOCK_METHOD(bool, Cancel, (AsyncHandle handle), (override));

    // =============================================================================
    // IFireWireBusInfo - Topology Queries
    // =============================================================================

    MOCK_METHOD(FW::FwSpeed, GetSpeed, (FW::NodeId nodeId), (const, override));

    MOCK_METHOD(uint32_t, HopCount, (FW::NodeId nodeA, FW::NodeId nodeB), (const, override));

    MOCK_METHOD(FW::Generation, GetGeneration, (), (const, override));

    MOCK_METHOD(FW::NodeId, GetLocalNodeID, (), (const, override));

    // =============================================================================
    // Helper: Set Default Behaviors
    // =============================================================================

    void SetDefaultTopology(FW::Generation gen, FW::NodeId localNodeId, FW::FwSpeed defaultSpeed) {
        using ::testing::_;
        using ::testing::Return;

        ON_CALL(*this, GetGeneration()).WillByDefault(Return(gen));
        ON_CALL(*this, GetLocalNodeID()).WillByDefault(Return(localNodeId));
        ON_CALL(*this, GetSpeed(_)).WillByDefault(Return(defaultSpeed));
        ON_CALL(*this, HopCount(_, _)).WillByDefault(Return(1));  // 1 hop default
    }
};

} // namespace ASFW::Async::Mocks
