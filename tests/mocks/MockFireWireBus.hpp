#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include <gmock/gmock.h>

namespace ASFW::Async::Mocks {

/**
 * @brief Mock FireWire bus for unit testing.
 *
 * Provides gmock-based expectations for all IFireWireBus operations.
 * Use this for precise behavior verification in unit tests.
 *
 * Example usage:
 *
 *   MockFireWireBus mockBus;
 *
 *   // Expect a quadlet read to Config ROM
 *   EXPECT_CALL(mockBus, ReadBlock(
 *       Generation{1},
 *       NodeId{0},
 *       testing::Field(&FWAddress::addressLo, 0xF0000400),
 *       4,  // quadlet size
 *       FwSpeed::S100,
 *       testing::_
 *   )).WillOnce([](auto, auto, auto, auto, auto, auto callback) {
 *       // Simulate successful response
 *       std::array<uint8_t, 4> data = {0x04, 0x04, 0x00, 0x00};  // BIB header
 *       callback(AsyncStatus::kSuccess, std::span{data});
 *       return AsyncHandle{1};
 *   });
 *
 *   ROMReader reader(mockBus);
 *   reader.ReadBIB(0, Generation{1}, FwSpeed::S100, [](auto result) {
 *       EXPECT_TRUE(result.success);
 *   });
 *
 * @note Only virtual methods need MOCK_METHOD. Non-virtual helpers (ReadQuad/WriteQuad)
 *       are implemented inline in IFireWireBusOps and call the virtual methods.
 */
class MockFireWireBus : public IFireWireBus {
public:
    // =============================================================================
    // IFireWireBusOps - Bus Operations
    // =============================================================================

    /**
     * @brief Mock for ReadBlock operation.
     *
     * Note: ReadQuad() is a non-virtual helper that calls this method with length=4.
     */
    MOCK_METHOD(AsyncHandle, ReadBlock,
                (Generation generation, NodeId nodeId, FWAddress address,
                 uint32_t length, FwSpeed speed, CompletionCallback callback),
                (override));

    /**
     * @brief Mock for WriteBlock operation.
     *
     * Note: WriteQuad() is a non-virtual helper that calls this method.
     */
    MOCK_METHOD(AsyncHandle, WriteBlock,
                (Generation generation, NodeId nodeId, FWAddress address,
                 std::span<const uint8_t> data, FwSpeed speed,
                 CompletionCallback callback),
                (override));

    /**
     * @brief Mock for Lock operation (atomic compare-and-swap, fetch-add, etc.).
     */
    MOCK_METHOD(AsyncHandle, Lock,
                (Generation generation, NodeId nodeId, FWAddress address,
                 LockOp lockOp, uint32_t arg, FwSpeed speed,
                 CompletionCallback callback),
                (override));

    /**
     * @brief Mock for Cancel operation.
     */
    MOCK_METHOD(bool, Cancel, (AsyncHandle handle), (override));

    // =============================================================================
    // IFireWireBusInfo - Topology Queries
    // =============================================================================

    /**
     * @brief Mock for GetSpeed query.
     *
     * Default behavior: Returns S100 (safest speed).
     * Override with ON_CALL or EXPECT_CALL as needed.
     */
    MOCK_METHOD(FwSpeed, GetSpeed, (NodeId nodeId), (const, override));

    /**
     * @brief Mock for HopCount query.
     *
     * Default behavior: Returns UINT32_MAX (unknown topology).
     * Override with ON_CALL or EXPECT_CALL as needed.
     */
    MOCK_METHOD(uint32_t, HopCount, (NodeId nodeA, NodeId nodeB), (const, override));

    /**
     * @brief Mock for GetGeneration query.
     *
     * Default behavior: Returns Generation{0}.
     * Override with ON_CALL or EXPECT_CALL as needed.
     */
    MOCK_METHOD(Generation, GetGeneration, (), (const, override));

    /**
     * @brief Mock for GetLocalNodeID query.
     *
     * Default behavior: Returns NodeId{0xFF} (invalid).
     * Override with ON_CALL or EXPECT_CALL as needed.
     */
    MOCK_METHOD(NodeId, GetLocalNodeID, (), (const, override));

    // =============================================================================
    // Helper: Set Default Behaviors
    // =============================================================================

    /**
     * @brief Set default return values for topology queries.
     *
     * Call this in test setup to provide reasonable defaults:
     *
     *   MockFireWireBus mockBus;
     *   mockBus.SetDefaultTopology(
     *       Generation{1},      // Current generation
     *       NodeId{0xFFC0},     // Local node ID (bus 0, node 0)
     *       FwSpeed::S400       // Default speed
     *   );
     */
    void SetDefaultTopology(Generation gen, NodeId localNodeId, FwSpeed defaultSpeed) {
        using ::testing::_;
        using ::testing::Return;

        ON_CALL(*this, GetGeneration()).WillByDefault(Return(gen));
        ON_CALL(*this, GetLocalNodeID()).WillByDefault(Return(localNodeId));
        ON_CALL(*this, GetSpeed(_)).WillByDefault(Return(defaultSpeed));
        ON_CALL(*this, HopCount(_, _)).WillByDefault(Return(1));  // 1 hop default
    }
};

} // namespace ASFW::Async::Mocks
