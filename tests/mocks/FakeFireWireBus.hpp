#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include <unordered_map>
#include <vector>
#include <cstring>

namespace ASFW::Async::Fakes {

/**
 * @brief Fake FireWire bus with programmable responses.
 *
 * Unlike MockFireWireBus (gmock-based with expectations), FakeFireWireBus
 * provides a simple programmable implementation for integration tests.
 * You "load" fake memory and it returns canned data when reads occur.
 *
 * **Use cases**:
 * - Integration tests: Load real Config ROM data and test parsing
 * - Regression tests: Use captured ROM dumps from real devices
 * - Behavioral tests: Verify scan logic without hardware
 *
 * **Example usage**:
 *
 *   FakeFireWireBus fakeBus;
 *
 *   // Program fake Config ROM for node 0
 *   fakeBus.SetMemory(0, 0xF0000400, {
 *       0x04, 0x04, 0x00, 0x00,  // BIB header (bus_info_length=4, crc_length=4)
 *       0x31, 0x33, 0x39, 0x34,  // Bus name "1394"
 *       0x00, 0x00, 0x00, 0x01,  // Node capabilities
 *       0x00, 0x11, 0x22, 0x33,  // GUID high
 *       0x44, 0x55, 0x66, 0x77   // GUID low
 *   });
 *
 *   // Set topology state
 *   fakeBus.SetGeneration(Generation{1});
 *   fakeBus.SetLocalNodeID(NodeId{0});
 *   fakeBus.SetSpeed(NodeId{0}, FwSpeed::S400);
 *
 *   // Use in test
 *   ROMReader reader(fakeBus);
 *   reader.ReadBIB(0, Generation{1}, FwSpeed::S400, [](auto result) {
 *       EXPECT_TRUE(result.success);
 *       // result.data contains the fake ROM data
 *   });
 */
class FakeFireWireBus : public IFireWireBus {
public:
    FakeFireWireBus()
        : generation_{0},
          localNodeId_{0xFF},
          nextHandle_{1} {}

    // =============================================================================
    // Programming API: Set up fake behavior
    // =============================================================================

    /**
     * @brief Load fake memory for a specific node and address.
     *
     * @param nodeId Target node (0-63)
     * @param address Starting address (32-bit offset, e.g. 0xF0000400 for Config ROM)
     * @param data Fake memory contents (will be copied)
     *
     * When ReadBlock() is called with matching node/address, this data is returned.
     */
    void SetMemory(uint8_t nodeId, uint32_t address, std::vector<uint8_t> data) {
        uint64_t key = MakeMemoryKey(nodeId, address);
        memory_[key] = std::move(data);
    }

    /**
     * @brief Set current bus generation number.
     */
    void SetGeneration(Generation gen) {
        generation_ = gen;
    }

    /**
     * @brief Set local node ID.
     */
    void SetLocalNodeID(NodeId node) {
        localNodeId_ = node;
    }

    /**
     * @brief Set negotiated speed for a specific node.
     */
    void SetSpeed(NodeId node, FwSpeed speed) {
        speeds_[node.value] = speed;
    }

    /**
     * @brief Set hop count between two nodes.
     */
    void SetHopCount(NodeId nodeA, NodeId nodeB, uint32_t hops) {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        hopCounts_[key] = hops;
    }

    /**
     * @brief Clear all programmed memory and topology state.
     */
    void Reset() {
        memory_.clear();
        speeds_.clear();
        hopCounts_.clear();
        generation_ = Generation{0};
        localNodeId_ = NodeId{0xFF};
        nextHandle_ = AsyncHandle{1};
    }

    // =============================================================================
    // IFireWireBusOps Implementation
    // =============================================================================

    AsyncHandle ReadBlock(Generation generation, NodeId nodeId, FWAddress address,
                         uint32_t length, FwSpeed speed,
                         CompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        // Check generation mismatch
        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return handle;
        }

        // Look up fake memory
        uint64_t key = MakeMemoryKey(nodeId.value, address.addressLo);
        auto it = memory_.find(key);
        if (it == memory_.end()) {
            // No data programmed for this address → timeout
            callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
            return handle;
        }

        const auto& data = it->second;
        if (length > data.size()) {
            // Not enough data → return what we have
            callback(AsyncStatus::kSuccess, std::span{data.data(), data.size()});
        } else {
            callback(AsyncStatus::kSuccess, std::span{data.data(), length});
        }

        return handle;
    }

    AsyncHandle WriteBlock(Generation generation, NodeId nodeId, FWAddress address,
                          std::span<const uint8_t> data, FwSpeed speed,
                          CompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        // Check generation mismatch
        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return handle;
        }

        // Writes succeed by default (no memory effect)
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>{});
        return handle;
    }

    AsyncHandle Lock(Generation generation, NodeId nodeId, FWAddress address,
                    LockOp lockOp, uint32_t arg, FwSpeed speed,
                    CompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        // Check generation mismatch
        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return handle;
        }

        // Lock operations succeed by default (return arg as old value)
        std::array<uint8_t, 4> oldValue = {
            static_cast<uint8_t>(arg >> 24),
            static_cast<uint8_t>(arg >> 16),
            static_cast<uint8_t>(arg >> 8),
            static_cast<uint8_t>(arg)
        };
        callback(AsyncStatus::kSuccess, std::span{oldValue});
        return handle;
    }

    bool Cancel(AsyncHandle handle) override {
        // Fake implementation: always return false (already completed)
        return false;
    }

    // =============================================================================
    // IFireWireBusInfo Implementation
    // =============================================================================

    FwSpeed GetSpeed(NodeId nodeId) const override {
        auto it = speeds_.find(nodeId.value);
        if (it != speeds_.end()) {
            return it->second;
        }
        return FwSpeed::S100;  // Default to safest speed
    }

    uint32_t HopCount(NodeId nodeA, NodeId nodeB) const override {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        auto it = hopCounts_.find(key);
        if (it != hopCounts_.end()) {
            return it->second;
        }
        return UINT32_MAX;  // Unknown topology
    }

    Generation GetGeneration() const override {
        return generation_;
    }

    NodeId GetLocalNodeID() const override {
        return localNodeId_;
    }

private:
    // Memory key: (nodeId << 32) | address
    static uint64_t MakeMemoryKey(uint8_t nodeId, uint32_t address) {
        return (static_cast<uint64_t>(nodeId) << 32) | address;
    }

    // Hop count key: symmetric (nodeA, nodeB) = (nodeB, nodeA)
    static uint32_t MakeHopKey(uint8_t nodeA, uint8_t nodeB) {
        if (nodeA > nodeB) std::swap(nodeA, nodeB);
        return (static_cast<uint32_t>(nodeA) << 16) | nodeB;
    }

    // Fake memory storage: key = (nodeId << 32) | address, value = data
    std::unordered_map<uint64_t, std::vector<uint8_t>> memory_;

    // Topology state
    std::unordered_map<uint8_t, FwSpeed> speeds_;
    std::unordered_map<uint32_t, uint32_t> hopCounts_;
    Generation generation_;
    NodeId localNodeId_;

    // Handle allocation
    AsyncHandle nextHandle_;
};

} // namespace ASFW::Async::Fakes
