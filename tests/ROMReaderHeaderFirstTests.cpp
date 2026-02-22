#include <cstdint>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/ConfigROM/ROMReader.hpp"
#include "ASFWDriver/Common/FWCommon.hpp"

namespace {

class MemoryFireWireBus final : public ASFW::Async::IFireWireBus {
public:
    void SetGeneration(uint32_t gen) { generation_ = ASFW::FW::Generation{gen}; }
    void SetLocalNode(uint8_t nodeId) { localNodeId_ = ASFW::FW::NodeId{nodeId}; }

    void SetConfigROM(uint8_t nodeId, std::vector<uint8_t> bytes) {
        configROM_[nodeId] = std::move(bytes);
    }

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation generation,
                                       ASFW::FW::NodeId nodeId,
                                       ASFW::Async::FWAddress address,
                                       uint32_t length,
                                       ASFW::FW::FwSpeed /*speed*/,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        ASFW::Async::AsyncHandle h{nextHandle_++};

        if (generation != generation_) {
            callback(ASFW::Async::AsyncStatus::kStaleGeneration, {});
            return h;
        }

        const auto it = configROM_.find(nodeId.value);
        if (it == configROM_.end()) {
            callback(ASFW::Async::AsyncStatus::kTimeout, {});
            return h;
        }

        const auto& bytes = it->second;
        const uint32_t base = ASFW::FW::ConfigROMAddr::kAddressLo;
        if (address.addressLo < base) {
            callback(ASFW::Async::AsyncStatus::kTimeout, {});
            return h;
        }

        const uint32_t offset = address.addressLo - base;
        if (offset >= bytes.size()) {
            callback(ASFW::Async::AsyncStatus::kTimeout, {});
            return h;
        }

        const uint32_t available = static_cast<uint32_t>(bytes.size() - offset);
        const uint32_t n = (length <= available) ? length : available;
        callback(ASFW::Async::AsyncStatus::kSuccess, std::span<const uint8_t>(bytes.data() + offset, n));
        return h;
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation generation,
                                        ASFW::FW::NodeId /*nodeId*/,
                                        ASFW::Async::FWAddress /*address*/,
                                        std::span<const uint8_t> /*data*/,
                                        ASFW::FW::FwSpeed /*speed*/,
                                        ASFW::Async::InterfaceCompletionCallback callback) override {
        ASFW::Async::AsyncHandle h{nextHandle_++};
        if (generation != generation_) {
            callback(ASFW::Async::AsyncStatus::kStaleGeneration, {});
            return h;
        }
        callback(ASFW::Async::AsyncStatus::kSuccess, {});
        return h;
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation generation,
                                  ASFW::FW::NodeId /*nodeId*/,
                                  ASFW::Async::FWAddress /*address*/,
                                  ASFW::FW::LockOp /*lockOp*/,
                                  std::span<const uint8_t> /*operand*/,
                                  uint32_t /*responseLength*/,
                                  ASFW::FW::FwSpeed /*speed*/,
                                  ASFW::Async::InterfaceCompletionCallback callback) override {
        ASFW::Async::AsyncHandle h{nextHandle_++};
        if (generation != generation_) {
            callback(ASFW::Async::AsyncStatus::kStaleGeneration, {});
            return h;
        }
        callback(ASFW::Async::AsyncStatus::kSuccess, {});
        return h;
    }

    bool Cancel(ASFW::Async::AsyncHandle /*handle*/) override { return false; }

    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId /*nodeId*/) const override { return ASFW::FW::FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId /*nodeA*/, ASFW::FW::NodeId /*nodeB*/) const override { return 1; }
    ASFW::FW::Generation GetGeneration() const override { return generation_; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return localNodeId_; }

private:
    ASFW::FW::Generation generation_{0};
    ASFW::FW::NodeId localNodeId_{0xFF};
    uint32_t nextHandle_{1};
    std::unordered_map<uint8_t, std::vector<uint8_t>> configROM_;
};

} // namespace

TEST(ROMReaderHeaderFirstTests, HeaderFirstUsesHigh16EntryCount) {
    MemoryFireWireBus bus;
    bus.SetGeneration(1);
    bus.SetLocalNode(0);

    // Root directory begins immediately after the 5-quadlet BIB (20 bytes).
    constexpr uint32_t kRootDirOffsetBytes = 20;

    // Header: length=3 entries, CRC=0xBEEF.
    // If ROMReader incorrectly uses low-16 as entry count, it would cap to 64 and read 65 quadlets.
    std::vector<uint8_t> romBytes(kRootDirOffsetBytes + 16, 0);
    romBytes[kRootDirOffsetBytes + 0] = 0x00;
    romBytes[kRootDirOffsetBytes + 1] = 0x03;
    romBytes[kRootDirOffsetBytes + 2] = 0xBE;
    romBytes[kRootDirOffsetBytes + 3] = 0xEF;

    bus.SetConfigROM(1, std::move(romBytes));

    ASFW::Discovery::ROMReader reader(bus, nullptr);

    bool called = false;
    reader.ReadRootDirQuadlets(1,
                               /*generation=*/ASFW::Discovery::Generation{1},
                               /*speed=*/ASFW::FW::FwSpeed::S100,
                               /*offsetBytes=*/kRootDirOffsetBytes,
                               /*count=*/0,
                               [&](const ASFW::Discovery::ROMReader::ReadResult& res) {
                                   called = true;
                                   EXPECT_TRUE(res.success);
                                   EXPECT_EQ(res.dataLength, 16u);  // (1 + 3) quadlets
                               });

    EXPECT_TRUE(called);
}

TEST(ROMReaderHeaderFirstTests, HeaderFirstCapsAt64Entries) {
    MemoryFireWireBus bus;
    bus.SetGeneration(1);
    bus.SetLocalNode(0);

    constexpr uint32_t kRootDirOffsetBytes = 20;

    // Header: length=100 entries, CRC=0.
    // Expect cap to 64 entries => total quadlets = 65.
    const uint32_t totalQuadlets = 65;
    std::vector<uint8_t> romBytes(kRootDirOffsetBytes + (totalQuadlets * 4), 0);
    romBytes[kRootDirOffsetBytes + 0] = 0x00;
    romBytes[kRootDirOffsetBytes + 1] = 0x64;  // 100
    romBytes[kRootDirOffsetBytes + 2] = 0x00;
    romBytes[kRootDirOffsetBytes + 3] = 0x00;

    bus.SetConfigROM(1, std::move(romBytes));

    ASFW::Discovery::ROMReader reader(bus, nullptr);

    bool called = false;
    reader.ReadRootDirQuadlets(1,
                               /*generation=*/ASFW::Discovery::Generation{1},
                               /*speed=*/ASFW::FW::FwSpeed::S100,
                               /*offsetBytes=*/kRootDirOffsetBytes,
                               /*count=*/0,
                               [&](const ASFW::Discovery::ROMReader::ReadResult& res) {
                                   called = true;
                                   EXPECT_TRUE(res.success);
                                   EXPECT_EQ(res.dataLength, totalQuadlets * 4u);
                               });

    EXPECT_TRUE(called);
}
