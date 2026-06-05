#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>

namespace ASFW::Async::Fakes {

/**
 * @brief Fake FireWire bus with programmable responses.
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

    void SetMemory(uint8_t nodeId, uint32_t address, std::vector<uint8_t> data) {
        uint64_t key = MakeMemoryKey(nodeId, address);
        memory_[key] = std::move(data);
    }

    void SetGeneration(FW::Generation gen) {
        generation_ = gen;
    }

    void SetLocalNodeID(FW::NodeId node) {
        localNodeId_ = node;
    }

    void SetSpeed(FW::NodeId node, FW::FwSpeed speed) {
        speeds_[node.value] = speed;
    }

    void SetHopCount(FW::NodeId nodeA, FW::NodeId nodeB, uint32_t hops) {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        hopCounts_[key] = hops;
    }

    void Reset() {
        memory_.clear();
        speeds_.clear();
        hopCounts_.clear();
        generation_ = FW::Generation{0};
        localNodeId_ = FW::NodeId{0xFF};
        nextHandle_ = AsyncHandle{1};
    }

    // =============================================================================
    // IFireWireBusOps Implementation
    // =============================================================================

    AsyncHandle ReadBlock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                          uint32_t length, FW::FwSpeed speed,
                          InterfaceCompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, std::span<const uint8_t>{});
            return handle;
        }

        // Try exact match first
        uint64_t key = MakeMemoryKey(nodeId.value, address.addressLo);
        auto it = memory_.find(key);
        if (it != memory_.end()) {
            const auto& data = it->second;
            if (length > data.size()) {
                callback(AsyncStatus::kSuccess, std::span{data.data(), data.size()});
            } else {
                callback(AsyncStatus::kSuccess, std::span{data.data(), length});
            }
            return handle;
        }

        // Fallback to range-based match if address falls within a programmed memory region
        for (const auto& [entryKey, data] : memory_) {
            uint8_t entryNodeId = static_cast<uint8_t>(entryKey >> 32);
            uint32_t baseAddress = static_cast<uint32_t>(entryKey & 0xFFFFFFFFULL);
            if (entryNodeId == nodeId.value && address.addressLo >= baseAddress &&
                address.addressLo < baseAddress + data.size()) {
                uint32_t offset = address.addressLo - baseAddress;
                uint32_t bytesToReturn = std::min(length, static_cast<uint32_t>(data.size() - offset));
                callback(AsyncStatus::kSuccess, std::span{data.data() + offset, bytesToReturn});
                return handle;
            }
        }

        callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
        return handle;
    }

    AsyncHandle WriteBlock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                           std::span<const uint8_t> data, FW::FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, std::span<const uint8_t>{});
            return handle;
        }

        callback(AsyncStatus::kSuccess, std::span<const uint8_t>{});
        return handle;
    }

    AsyncHandle Lock(FW::Generation generation, FW::NodeId nodeId, FWAddress address,
                     FW::LockOp lockOp, std::span<const uint8_t> operand, uint32_t responseLength,
                     FW::FwSpeed speed, InterfaceCompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        if (generation != generation_) {
            callback(AsyncStatus::kStaleGeneration, std::span<const uint8_t>{});
            return handle;
        }

        std::array<uint8_t, 4> oldValue = {0, 0, 0, 0};
        if (operand.size() >= 4) {
            std::copy(operand.begin(), operand.begin() + 4, oldValue.begin());
        }
        callback(AsyncStatus::kSuccess, std::span{oldValue});
        return handle;
    }

    bool Cancel(AsyncHandle handle) override {
        return false;
    }

    // =============================================================================
    // IFireWireBusInfo Implementation
    // =============================================================================

    FW::FwSpeed GetSpeed(FW::NodeId nodeId) const override {
        auto it = speeds_.find(nodeId.value);
        if (it != speeds_.end()) {
            return it->second;
        }
        return FW::FwSpeed::S100;
    }

    uint32_t HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const override {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        auto it = hopCounts_.find(key);
        if (it != hopCounts_.end()) {
            return it->second;
        }
        return UINT32_MAX;
    }

    FW::Generation GetGeneration() const override {
        return generation_;
    }

    FW::NodeId GetLocalNodeID() const override {
        return localNodeId_;
    }

private:
    static uint64_t MakeMemoryKey(uint8_t nodeId, uint32_t address) {
        return (static_cast<uint64_t>(nodeId) << 32) | address;
    }

    static uint32_t MakeHopKey(uint8_t nodeA, uint8_t nodeB) {
        if (nodeA > nodeB) std::swap(nodeA, nodeB);
        return (static_cast<uint32_t>(nodeA) << 16) | nodeB;
    }

    std::unordered_map<uint64_t, std::vector<uint8_t>> memory_;
    std::unordered_map<uint8_t, FW::FwSpeed> speeds_;
    std::unordered_map<uint32_t, uint32_t> hopCounts_;
    FW::Generation generation_;
    FW::NodeId localNodeId_;
    AsyncHandle nextHandle_;
};

} // namespace ASFW::Async::Fakes
