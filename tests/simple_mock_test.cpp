/**
 * Simple standalone test to verify FakeFireWireBus works correctly.
 * This test is self-contained and doesn't require DriverKit or the full ASFW build.
 *
 * Compile: g++ -std=c++20 -I../ASFWDriver simple_mock_test.cpp -o simple_mock_test
 * Run: ./simple_mock_test
 */

#include <iostream>
#include <cassert>
#include <cstdint>
#include <vector>
#include <span>
#include <functional>
#include <unordered_map>

// Minimal type definitions needed for the test
namespace ASFW::Async {

// AsyncHandle type
struct FWHandle {
    uint32_t value{0};
    explicit operator bool() const { return value != 0; }
};
using AsyncHandle = FWHandle;

// AsyncStatus enum
enum class AsyncStatus {
    kSuccess = 0,
    kTimeout,
    kBusReset,
    kResponseError,
    kCancelled
};

// FWAddress structure
struct FWAddress {
    uint16_t nodeID;
    uint16_t addressHi;
    uint32_t addressLo;
};

// FwSpeed enum
enum class FwSpeed : uint8_t {
    S100 = 0,
    S200 = 1,
    S400 = 2,
    S800 = 3
};

// Generation wrapper
struct Generation {
    uint32_t value;
    explicit constexpr Generation(uint32_t v) : value(v) {}
    constexpr bool operator==(const Generation& other) const { return value == other.value; }
    constexpr bool operator!=(const Generation& other) const { return value != other.value; }
};

// NodeId wrapper
struct NodeId {
    uint8_t value;
    explicit constexpr NodeId(uint8_t v) : value(v) {}
    constexpr bool operator==(const NodeId& other) const { return value == other.value; }
};

// LockOp enum
// CRITICAL: Values MUST match IEEE 1394 extended tCode wire format!
enum class LockOp : uint8_t {
    kMaskSwap = 1,        // extTcode 0x1
    kCompareSwap = 2,     // extTcode 0x2
    kFetchAdd = 3         // extTcode 0x3
};

// Completion callback
using CompletionCallback = std::function<void(AsyncStatus, std::span<const uint8_t>)>;

// Interface base classes
class IFireWireBusOps {
public:
    virtual ~IFireWireBusOps() = default;
    virtual AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed, CompletionCallback) = 0;
    virtual AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed, CompletionCallback) = 0;
    virtual AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, uint32_t, FwSpeed, CompletionCallback) = 0;
    virtual bool Cancel(AsyncHandle) = 0;
};

class IFireWireBusInfo {
public:
    virtual ~IFireWireBusInfo() = default;
    virtual FwSpeed GetSpeed(NodeId) const = 0;
    virtual uint32_t HopCount(NodeId, NodeId) const = 0;
    virtual Generation GetGeneration() const = 0;
    virtual NodeId GetLocalNodeID() const = 0;
};

class IFireWireBus : public IFireWireBusOps, public IFireWireBusInfo {
public:
    virtual ~IFireWireBus() = default;
};

} // namespace ASFW::Async

// Inline the FakeFireWireBus implementation
namespace ASFW::Async::Fakes {

class FakeFireWireBus : public IFireWireBus {
public:
    FakeFireWireBus() : generation_{0}, localNodeId_{0xFF}, nextHandle_{1} {}

    void SetMemory(uint8_t nodeId, uint32_t address, std::vector<uint8_t> data) {
        uint64_t key = MakeMemoryKey(nodeId, address);
        memory_[key] = std::move(data);
    }

    void SetGeneration(Generation gen) { generation_ = gen; }
    void SetLocalNodeID(NodeId node) { localNodeId_ = node; }
    void SetSpeed(NodeId node, FwSpeed speed) { speeds_[node.value] = speed; }
    void SetHopCount(NodeId nodeA, NodeId nodeB, uint32_t hops) {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        hopCounts_[key] = hops;
    }

    AsyncHandle ReadBlock(Generation generation, NodeId nodeId, FWAddress address,
                         uint32_t length, FwSpeed speed, CompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};

        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return handle;
        }

        uint64_t key = MakeMemoryKey(nodeId.value, address.addressLo);
        auto it = memory_.find(key);
        if (it == memory_.end()) {
            callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
            return handle;
        }

        const auto& data = it->second;
        if (length > data.size()) {
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
        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
        } else {
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>{});
        }
        return handle;
    }

    AsyncHandle Lock(Generation generation, NodeId nodeId, FWAddress address,
                    LockOp lockOp, uint32_t arg, FwSpeed speed,
                    CompletionCallback callback) override {
        AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};
        if (generation != generation_) {
            callback(AsyncStatus::kBusReset, std::span<const uint8_t>{});
            return handle;
        }
        std::array<uint8_t, 4> oldValue = {
            static_cast<uint8_t>(arg >> 24),
            static_cast<uint8_t>(arg >> 16),
            static_cast<uint8_t>(arg >> 8),
            static_cast<uint8_t>(arg)
        };
        callback(AsyncStatus::kSuccess, std::span{oldValue});
        return handle;
    }

    bool Cancel(AsyncHandle) override { return false; }

    FwSpeed GetSpeed(NodeId nodeId) const override {
        auto it = speeds_.find(nodeId.value);
        return (it != speeds_.end()) ? it->second : FwSpeed::S100;
    }

    uint32_t HopCount(NodeId nodeA, NodeId nodeB) const override {
        uint32_t key = MakeHopKey(nodeA.value, nodeB.value);
        auto it = hopCounts_.find(key);
        return (it != hopCounts_.end()) ? it->second : UINT32_MAX;
    }

    Generation GetGeneration() const override { return generation_; }
    NodeId GetLocalNodeID() const override { return localNodeId_; }

private:
    static uint64_t MakeMemoryKey(uint8_t nodeId, uint32_t address) {
        return (static_cast<uint64_t>(nodeId) << 32) | address;
    }

    static uint32_t MakeHopKey(uint8_t nodeA, uint8_t nodeB) {
        if (nodeA > nodeB) std::swap(nodeA, nodeB);
        return (static_cast<uint32_t>(nodeA) << 16) | nodeB;
    }

    std::unordered_map<uint64_t, std::vector<uint8_t>> memory_;
    std::unordered_map<uint8_t, FwSpeed> speeds_;
    std::unordered_map<uint32_t, uint32_t> hopCounts_;
    Generation generation_;
    NodeId localNodeId_;
    AsyncHandle nextHandle_;
};

} // namespace ASFW::Async::Fakes

// Test functions
using namespace ASFW::Async;
using namespace ASFW::Async::Fakes;

void test_basic_read_success() {
    std::cout << "TEST: Basic read success... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{1});
    bus.SetLocalNodeID(NodeId{0});

    // Program fake Config ROM
    std::vector<uint8_t> romData = {
        0x04, 0x04, 0x00, 0x00,  // BIB header
        0x31, 0x33, 0x39, 0x34   // "1394"
    };
    bus.SetMemory(0, 0xF0000400, romData);

    bool callbackInvoked = false;
    AsyncStatus receivedStatus = AsyncStatus::kTimeout;
    std::vector<uint8_t> receivedData;

    bus.ReadBlock(
        Generation{1},
        NodeId{0},
        FWAddress{0, 0xFFFF, 0xF0000400},
        8,
        FwSpeed::S100,
        [&](AsyncStatus status, std::span<const uint8_t> data) {
            callbackInvoked = true;
            receivedStatus = status;
            receivedData.assign(data.begin(), data.end());
        }
    );

    assert(callbackInvoked && "Callback should be invoked");
    assert(receivedStatus == AsyncStatus::kSuccess && "Status should be success");
    assert(receivedData.size() == 8 && "Should receive 8 bytes");
    assert(receivedData[0] == 0x04 && "First byte should match");
    assert(receivedData[4] == 0x31 && "Fifth byte should match");

    std::cout << "PASSED ✓\n";
}

void test_read_timeout() {
    std::cout << "TEST: Read timeout (unprogrammed address)... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{1});

    bool callbackInvoked = false;
    AsyncStatus receivedStatus = AsyncStatus::kSuccess;

    bus.ReadBlock(
        Generation{1},
        NodeId{0},
        FWAddress{0, 0xFFFF, 0x12345678},  // Unprogrammed address
        4,
        FwSpeed::S100,
        [&](AsyncStatus status, std::span<const uint8_t> data) {
            callbackInvoked = true;
            receivedStatus = status;
        }
    );

    assert(callbackInvoked && "Callback should be invoked");
    assert(receivedStatus == AsyncStatus::kTimeout && "Status should be timeout");

    std::cout << "PASSED ✓\n";
}

void test_generation_mismatch() {
    std::cout << "TEST: Generation mismatch (bus reset)... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{1});
    bus.SetMemory(0, 0xF0000400, {0x01, 0x02, 0x03, 0x04});

    bool callbackInvoked = false;
    AsyncStatus receivedStatus = AsyncStatus::kSuccess;

    // Try to read with wrong generation
    bus.ReadBlock(
        Generation{99},  // Wrong generation!
        NodeId{0},
        FWAddress{0, 0xFFFF, 0xF0000400},
        4,
        FwSpeed::S100,
        [&](AsyncStatus status, std::span<const uint8_t> data) {
            callbackInvoked = true;
            receivedStatus = status;
        }
    );

    assert(callbackInvoked && "Callback should be invoked");
    assert(receivedStatus == AsyncStatus::kBusReset && "Status should be bus reset");

    std::cout << "PASSED ✓\n";
}

void test_topology_queries() {
    std::cout << "TEST: Topology queries... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{42});
    bus.SetLocalNodeID(NodeId{5});
    bus.SetSpeed(NodeId{10}, FwSpeed::S400);
    bus.SetHopCount(NodeId{5}, NodeId{10}, 3);

    assert(bus.GetGeneration().value == 42 && "Generation should match");
    assert(bus.GetLocalNodeID().value == 5 && "Local node should match");
    assert(bus.GetSpeed(NodeId{10}) == FwSpeed::S400 && "Speed should match");
    assert(bus.GetSpeed(NodeId{99}) == FwSpeed::S100 && "Unknown node defaults to S100");
    assert(bus.HopCount(NodeId{5}, NodeId{10}) == 3 && "Hop count should match");
    assert(bus.HopCount(NodeId{10}, NodeId{5}) == 3 && "Hop count symmetric");
    assert(bus.HopCount(NodeId{1}, NodeId{2}) == UINT32_MAX && "Unknown hops");

    std::cout << "PASSED ✓\n";
}

void test_write_operation() {
    std::cout << "TEST: Write operation... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{1});

    bool callbackInvoked = false;
    AsyncStatus receivedStatus = AsyncStatus::kTimeout;

    std::array<uint8_t, 4> writeData = {0xDE, 0xAD, 0xBE, 0xEF};

    bus.WriteBlock(
        Generation{1},
        NodeId{0},
        FWAddress{0, 0xFFFF, 0xF0001000},
        std::span{writeData},
        FwSpeed::S400,
        [&](AsyncStatus status, std::span<const uint8_t> data) {
            callbackInvoked = true;
            receivedStatus = status;
        }
    );

    assert(callbackInvoked && "Callback should be invoked");
    assert(receivedStatus == AsyncStatus::kSuccess && "Write should succeed");

    std::cout << "PASSED ✓\n";
}

void test_lock_operation() {
    std::cout << "TEST: Lock operation... ";

    FakeFireWireBus bus;
    bus.SetGeneration(Generation{1});

    bool callbackInvoked = false;
    AsyncStatus receivedStatus = AsyncStatus::kTimeout;
    std::vector<uint8_t> receivedData;

    bus.Lock(
        Generation{1},
        NodeId{0},
        FWAddress{0, 0xFFFF, 0xF0002000},
        LockOp::kFetchAdd,
        0x12345678,
        FwSpeed::S400,
        [&](AsyncStatus status, std::span<const uint8_t> data) {
            callbackInvoked = true;
            receivedStatus = status;
            receivedData.assign(data.begin(), data.end());
        }
    );

    assert(callbackInvoked && "Callback should be invoked");
    assert(receivedStatus == AsyncStatus::kSuccess && "Lock should succeed");
    assert(receivedData.size() == 4 && "Should receive 4 bytes (old value)");

    // Verify old value matches argument (fake implementation returns arg as old value)
    uint32_t oldValue = (receivedData[0] << 24) | (receivedData[1] << 16) |
                       (receivedData[2] << 8) | receivedData[3];
    assert(oldValue == 0x12345678 && "Old value should match");

    std::cout << "PASSED ✓\n";
}

int main() {
    std::cout << "======================================\n";
    std::cout << "Running FakeFireWireBus Tests\n";
    std::cout << "======================================\n\n";

    try {
        test_basic_read_success();
        test_read_timeout();
        test_generation_mismatch();
        test_topology_queries();
        test_write_operation();
        test_lock_operation();

        std::cout << "\n======================================\n";
        std::cout << "All tests PASSED! ✓✓✓\n";
        std::cout << "======================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nFAILED: " << e.what() << "\n";
        return 1;
    }
}
