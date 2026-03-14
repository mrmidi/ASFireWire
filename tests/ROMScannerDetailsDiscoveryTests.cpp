#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "../ASFWDriver/Common/FWCommon.hpp"
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;

namespace {

class ScriptedRomBus : public ASFW::Async::IFireWireBus {
  public:
    struct PendingRead {
        ASFW::Async::FWAddress address{};
        uint32_t length{0};
        ASFW::Async::InterfaceCompletionCallback callback;
    };

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress address, uint32_t length,
                                       ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        std::lock_guard lock(mtx_);
        pendingReads_.push_back(PendingRead{
            .address = address,
            .length = length,
            .callback = std::move(callback),
        });
        cv_.notify_all();
        return ASFW::Async::AsyncHandle{static_cast<uint32_t>(pendingReads_.size())};
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress, std::span<const uint8_t>,
                                        ASFW::FW::FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress,
                                  ASFW::FW::LockOp, std::span<const uint8_t>, uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return ASFW::FW::FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{0}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    bool ServeNextReadFromImage(std::span<const uint32_t> romQuadletsBE) {
        PendingRead pending;
        {
            std::unique_lock lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return !pendingReads_.empty(); });
            if (pendingReads_.empty()) {
                return false;
            }
            pending = std::move(pendingReads_.front());
            pendingReads_.pop_front();
        }

        if (pending.length != 4U) {
            ADD_FAILURE() << "Expected quadlet read length=4, got " << pending.length;
            if (pending.callback) {
                pending.callback(ASFW::Async::AsyncStatus::kHardwareError, {});
            }
            return true;
        }

        if (pending.address.addressLo < ASFW::FW::ConfigROMAddr::kAddressLo) {
            ADD_FAILURE() << "Unexpected addressLo 0x" << std::hex << pending.address.addressLo;
            if (pending.callback) {
                pending.callback(ASFW::Async::AsyncStatus::kHardwareError, {});
            }
            return true;
        }

        const uint32_t offsetBytes =
            pending.address.addressLo - ASFW::FW::ConfigROMAddr::kAddressLo;
        if ((offsetBytes % 4U) != 0U) {
            ADD_FAILURE() << "Expected quadlet-aligned offsetBytes, got " << offsetBytes;
            if (pending.callback) {
                pending.callback(ASFW::Async::AsyncStatus::kHardwareError, {});
            }
            return true;
        }

        const uint32_t quadIndex = offsetBytes / 4U;
        if (quadIndex >= romQuadletsBE.size()) {
            ADD_FAILURE() << "Read past ROM image: quadIndex=" << quadIndex
                          << " romSize=" << romQuadletsBE.size();
            if (pending.callback) {
                pending.callback(ASFW::Async::AsyncStatus::kShortRead, {});
            }
            return true;
        }

        const uint32_t q = romQuadletsBE[quadIndex];
        const std::array<uint8_t, 4> bytes = {
            static_cast<uint8_t>((q >> 24) & 0xFFU),
            static_cast<uint8_t>((q >> 16) & 0xFFU),
            static_cast<uint8_t>((q >> 8) & 0xFFU),
            static_cast<uint8_t>(q & 0xFFU),
        };

        if (pending.callback) {
            pending.callback(ASFW::Async::AsyncStatus::kSuccess, std::span{bytes});
        }
        return true;
    }

  private:
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    std::deque<PendingRead> pendingReads_;
};

[[nodiscard]] constexpr uint32_t MakeBIBHeader(uint8_t busInfoLength, uint8_t crcLength,
                                               uint16_t crc) {
    return (static_cast<uint32_t>(busInfoLength) << 24) | (static_cast<uint32_t>(crcLength) << 16) |
           static_cast<uint32_t>(crc);
}

[[nodiscard]] constexpr uint32_t MakeDirHeader(uint16_t len) {
    return (static_cast<uint32_t>(len) << 16);
}

[[nodiscard]] constexpr uint32_t MakeImmediateEntry(uint8_t keyId, uint32_t value24) {
    return (static_cast<uint32_t>(ASFW::FW::EntryType::kImmediate) << 30) |
           (static_cast<uint32_t>(keyId & 0x3FU) << 24) | (value24 & 0x00FFFFFFU);
}

[[nodiscard]] constexpr uint32_t EncodeSigned24(int32_t value) {
    return static_cast<uint32_t>(value) & 0x00FFFFFFU;
}

[[nodiscard]] constexpr uint32_t MakeTargetEntry(uint8_t keyType, uint8_t keyId,
                                                 uint32_t entryIndex, uint32_t targetRel) {
    const int32_t signedValue = static_cast<int32_t>(targetRel) - static_cast<int32_t>(entryIndex);
    return (static_cast<uint32_t>(keyType & 0x3U) << 30) |
           (static_cast<uint32_t>(keyId & 0x3FU) << 24) | EncodeSigned24(signedValue);
}

[[nodiscard]] std::vector<uint32_t> MakeTextLeafBE(std::string_view text, bool validTypeSpec) {
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() + 1U);
    bytes.insert(bytes.end(), text.begin(), text.end());
    bytes.push_back(0); // NUL terminator for parser early-exit

    const uint32_t textQuadlets = (static_cast<uint32_t>(bytes.size()) + 3U) / 4U;
    bytes.resize(static_cast<size_t>(textQuadlets) * 4U, 0);

    std::vector<uint32_t> out;
    out.reserve(static_cast<size_t>(3U + textQuadlets));

    const uint16_t leafLength = static_cast<uint16_t>(2U + textQuadlets);
    out.push_back(static_cast<uint32_t>(leafLength) << 16);

    const uint32_t typeSpec = validTypeSpec ? 0U : 0x01000000U; // descriptorType != 0
    out.push_back(typeSpec);
    out.push_back(0U); // width/charset/lang = 0 (ASCII per parser)

    for (uint32_t i = 0; i < textQuadlets; ++i) {
        const size_t base = static_cast<size_t>(i) * 4U;
        const uint32_t q = (static_cast<uint32_t>(bytes[base + 0]) << 24) |
                           (static_cast<uint32_t>(bytes[base + 1]) << 16) |
                           (static_cast<uint32_t>(bytes[base + 2]) << 8) |
                           (static_cast<uint32_t>(bytes[base + 3]) << 0);
        out.push_back(q);
    }

    return out;
}

void WriteQuadlets(std::vector<uint32_t>& rom, uint32_t startQuadlet,
                   std::span<const uint32_t> quadletsBE) {
    const size_t required = static_cast<size_t>(startQuadlet) + quadletsBE.size();
    if (rom.size() < required) {
        rom.resize(required, 0);
    }
    for (size_t i = 0; i < quadletsBE.size(); ++i) {
        rom[startQuadlet + i] = quadletsBE[i];
    }
}

[[nodiscard]] std::vector<uint32_t> BuildROMImageVendorModelLeafs() {
    // BIB (quadlets 0..4), Root Dir starts at quadlet 5 (busInfoLength=4).
    constexpr uint32_t kRootDirStart = 5;

    std::vector<uint32_t> rom(64, 0);
    rom[0] = MakeBIBHeader(/*busInfoLength=*/4, /*crcLength=*/64, /*crc=*/0);
    rom[2] = 0;
    rom[3] = 0;
    rom[4] = 0;

    const auto vendorLeaf = MakeTextLeafBE("ACME_CORP", /*validTypeSpec=*/true);
    const uint32_t vendorLeafAbs = kRootDirStart + 1U + 4U; // header + 4 entries
    WriteQuadlets(rom, vendorLeafAbs, vendorLeaf);

    const auto modelLeaf = MakeTextLeafBE("MODEL_X", /*validTypeSpec=*/true);
    const uint32_t modelLeafAbs = vendorLeafAbs + static_cast<uint32_t>(vendorLeaf.size());
    WriteQuadlets(rom, modelLeafAbs, modelLeaf);

    // Root directory: [VendorId, VendorTextLeaf, ModelId, ModelTextLeaf]
    rom[kRootDirStart + 0] = MakeDirHeader(/*len=*/4);
    rom[kRootDirStart + 1] = MakeImmediateEntry(ASFW::FW::ConfigKey::kModuleVendorId, 0x00AABBCCU);
    rom[kRootDirStart + 2] =
        MakeTargetEntry(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/2, /*targetRel=*/vendorLeafAbs - kRootDirStart);
    rom[kRootDirStart + 3] = MakeImmediateEntry(ASFW::FW::ConfigKey::kModelId, 0x00000123U);
    rom[kRootDirStart + 4] =
        MakeTargetEntry(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/4, /*targetRel=*/modelLeafAbs - kRootDirStart);

    return rom;
}

[[nodiscard]] std::vector<uint32_t> BuildROMImageVendorModelLeafsWithBIBCrcLength4() {
    auto rom = BuildROMImageVendorModelLeafs();
    rom[0] = MakeBIBHeader(/*busInfoLength=*/4, /*crcLength=*/4, /*crc=*/0);
    return rom;
}

[[nodiscard]] std::vector<uint32_t> BuildROMImageDescriptorDirFallback() {
    constexpr uint32_t kRootDirStart = 5;
    constexpr uint32_t kDescriptorDirAbs = 8;

    std::vector<uint32_t> rom(64, 0);
    rom[0] = MakeBIBHeader(/*busInfoLength=*/4, /*crcLength=*/64, /*crc=*/0);
    rom[2] = 0;
    rom[3] = 0;
    rom[4] = 0;

    // Root directory: [VendorId, VendorTextDescriptorDir]
    rom[kRootDirStart + 0] = MakeDirHeader(/*len=*/2);
    rom[kRootDirStart + 1] = MakeImmediateEntry(ASFW::FW::ConfigKey::kModuleVendorId, 0x00112233U);
    rom[kRootDirStart + 2] =
        MakeTargetEntry(ASFW::FW::EntryType::kDirectory, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/2, /*targetRel=*/kDescriptorDirAbs - kRootDirStart);

    // Descriptor directory at quadlet 8: two candidates.
    rom[kDescriptorDirAbs + 0] = MakeDirHeader(/*len=*/2);

    const auto invalidLeaf = MakeTextLeafBE("BAD", /*validTypeSpec=*/false);
    const uint32_t invalidLeafAbs = kDescriptorDirAbs + 1U + 2U; // header + 2 entries
    WriteQuadlets(rom, invalidLeafAbs, invalidLeaf);

    const auto validLeaf = MakeTextLeafBE("VENDOR_OK", /*validTypeSpec=*/true);
    const uint32_t validLeafAbs = invalidLeafAbs + static_cast<uint32_t>(invalidLeaf.size());
    WriteQuadlets(rom, validLeafAbs, validLeaf);

    rom[kDescriptorDirAbs + 1] =
        MakeTargetEntry(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/1, /*targetRel=*/invalidLeafAbs - kDescriptorDirAbs);
    rom[kDescriptorDirAbs + 2] =
        MakeTargetEntry(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/2, /*targetRel=*/validLeafAbs - kDescriptorDirAbs);

    return rom;
}

[[nodiscard]] std::vector<uint32_t> BuildROMImageUnitDirModelName() {
    constexpr uint32_t kRootDirStart = 5;
    constexpr uint32_t kUnitDirAbs = 7;

    std::vector<uint32_t> rom(64, 0);
    rom[0] = MakeBIBHeader(/*busInfoLength=*/4, /*crcLength=*/64, /*crc=*/0);
    rom[2] = 0;
    rom[3] = 0;
    rom[4] = 0;

    // Root directory: [Unit_Directory -> target]
    rom[kRootDirStart + 0] = MakeDirHeader(/*len=*/1);
    rom[kRootDirStart + 1] =
        MakeTargetEntry(ASFW::FW::EntryType::kDirectory, ASFW::FW::ConfigKey::kUnitDirectory,
                        /*entryIndex=*/1, /*targetRel=*/kUnitDirAbs - kRootDirStart);

    // Unit directory: [ModelId, ModelTextLeaf]
    rom[kUnitDirAbs + 0] = MakeDirHeader(/*len=*/2);
    rom[kUnitDirAbs + 1] = MakeImmediateEntry(ASFW::FW::ConfigKey::kModelId, 0x0000BEEF);

    const auto modelLeaf = MakeTextLeafBE("UNIT_MODEL", /*validTypeSpec=*/true);
    const uint32_t modelLeafAbs = kUnitDirAbs + 1U + 2U; // header + 2 entries
    WriteQuadlets(rom, modelLeafAbs, modelLeaf);

    rom[kUnitDirAbs + 2] =
        MakeTargetEntry(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                        /*entryIndex=*/2, /*targetRel=*/modelLeafAbs - kUnitDirAbs);

    return rom;
}

struct ScanResult {
    int callbackCount{0};
    bool hadBusyNodes{false};
    std::vector<ConfigROM> roms;
};

[[nodiscard]] ScanResult RunScanToCompletion(ScriptedRomBus& bus,
                                             std::span<const uint32_t> romImageBE) {
    SpeedPolicy speedPolicy;

    ROMScannerParams params{};
    params.doIRMCheck = false;

    ROMScanner scanner(bus, speedPolicy, params);

    TopologySnapshot topology;
    topology.generation = 3;
    topology.busBase16 = 0xFFC0;
    topology.nodes.push_back({.nodeId = 1, .linkActive = true});

    ROMScanRequest request{};
    request.gen = Generation{topology.generation};
    request.topology = topology;
    request.localNodeId = 0;
    request.targetNodes = {1};

    std::mutex mtx;
    std::condition_variable cv;
    ScanResult out{};
    std::atomic<bool> done{false};

    const bool started =
        scanner.Start(request, [&](Generation /*gen*/, std::vector<ConfigROM> roms, bool busy) {
            std::lock_guard lock(mtx);
            out.callbackCount++;
            out.hadBusyNodes = busy;
            out.roms = std::move(roms);
            done.store(true);
            cv.notify_all();
        });

    EXPECT_TRUE(started);

    for (int i = 0; i < 512 && !done.load(); ++i) {
        if (!bus.ServeNextReadFromImage(romImageBE)) {
            break;
        }
    }

    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return done.load(); });
    }

    EXPECT_TRUE(done.load());

    return out;
}

} // namespace

TEST(ROMScannerDetails, VendorAndModelLeafs_Parsed) {
    ScriptedRomBus bus;
    const auto rom = BuildROMImageVendorModelLeafs();

    const auto res = RunScanToCompletion(bus, rom);

    EXPECT_EQ(res.callbackCount, 1);
    ASSERT_EQ(res.roms.size(), 1u);
    EXPECT_EQ(res.roms[0].vendorName, "ACME_CORP");
    EXPECT_EQ(res.roms[0].modelName, "MODEL_X");
}

TEST(ROMScannerDetails, VendorAndModelLeafs_ParsedWhenBIBCrcLengthEqualsBusInfoLength) {
    ScriptedRomBus bus;
    const auto rom = BuildROMImageVendorModelLeafsWithBIBCrcLength4();

    const auto res = RunScanToCompletion(bus, rom);

    EXPECT_EQ(res.callbackCount, 1);
    ASSERT_EQ(res.roms.size(), 1u);
    EXPECT_EQ(res.roms[0].vendorName, "ACME_CORP");
    EXPECT_EQ(res.roms[0].modelName, "MODEL_X");
    EXPECT_GT(res.roms[0].rawQuadlets.size(), 5u);
}

TEST(ROMScannerDetails, DescriptorDirFallback_PicksFirstValidLeaf) {
    ScriptedRomBus bus;
    const auto rom = BuildROMImageDescriptorDirFallback();

    const auto res = RunScanToCompletion(bus, rom);

    EXPECT_EQ(res.callbackCount, 1);
    ASSERT_EQ(res.roms.size(), 1u);
    EXPECT_EQ(res.roms[0].vendorName, "VENDOR_OK");
    EXPECT_TRUE(res.roms[0].modelName.empty());
}

TEST(ROMScannerDetails, UnitDirectory_ModelNameParsed) {
    ScriptedRomBus bus;
    const auto rom = BuildROMImageUnitDirModelName();

    const auto res = RunScanToCompletion(bus, rom);

    EXPECT_EQ(res.callbackCount, 1);
    ASSERT_EQ(res.roms.size(), 1u);
    ASSERT_EQ(res.roms[0].unitDirectories.size(), 1u);
    ASSERT_TRUE(res.roms[0].unitDirectories[0].modelName.has_value());
    EXPECT_EQ(res.roms[0].unitDirectories[0].modelName.value(), "UNIT_MODEL");
}
