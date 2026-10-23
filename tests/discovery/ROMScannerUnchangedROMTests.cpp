// After a bus reset the scan reads each node's bus info block and, when it is
// unchanged, completes from the previous generation's copy of the ROM instead
// of reading the rest (Apple IOFireWireROMCache::hasROMChanged). These tests
// scan a device once, then scan it again with that result as the previous
// generation, and check which Config ROM quadlets the second scan read.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "../ASFWDriver/Common/FWCommon.hpp"
#include "../ASFWDriver/ConfigROM/Common/ConfigROMPolicies.hpp"
#include "../ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "../ASFWDriver/Controller/ControllerTypes.hpp"
#include "../ASFWDriver/Discovery/SpeedPolicy.hpp"

using namespace ASFW::Discovery;
using namespace ASFW::Driver;
using ASFW::Async::AsyncStatus;

namespace {

constexpr uint32_t kConfigROMBase = ASFW::FW::ConfigROMAddr::kAddressLo;
constexpr uint32_t kBusInfoBlockQuadlets = 5;
constexpr uint32_t kGuidHi = 0x000A9204;
constexpr uint32_t kGuidLo = 0x050839AE;

// Serves Config ROM quadlet reads from an image (host-order quadlets, sent
// big-endian), answers the IRM CSR read and compare-swap, and records every
// request. Requests wait in a queue until ServeAll(); the scanner has no
// dispatch queue here, so each completion runs to the next request inline.
class RomImageBus final : public ASFW::Async::IFireWireBus {
  public:
    struct Served {
        bool lock{false};
        uint32_t addressLo{0};
        FwSpeed speed{FwSpeed::S100};
    };

    explicit RomImageBus(std::vector<uint32_t> image) : image_(std::move(image)) {}

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress address, uint32_t, FwSpeed speed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        pending_.push_back({false, address.addressLo, speed, std::move(callback)});
        return ASFW::Async::AsyncHandle{++handles_};
    }

    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress, std::span<const uint8_t>, FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback) override {
        return ASFW::Async::AsyncHandle{0};
    }

    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                  ASFW::Async::FWAddress address, ASFW::FW::LockOp,
                                  std::span<const uint8_t>, uint32_t, FwSpeed speed,
                                  ASFW::Async::InterfaceCompletionCallback callback) override {
        pending_.push_back({true, address.addressLo, speed, std::move(callback)});
        return ASFW::Async::AsyncHandle{++handles_};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return FwSpeed::S100; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{0}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    // The next read of this ROM quadlet times out (once).
    void FailReadOfQuadlet(uint32_t quadlet) { failQuadlet_ = quadlet; }

    void ServeAll() {
        while (!pending_.empty()) {
            Pending request = std::move(pending_.front());
            pending_.pop_front();
            served_.push_back({request.lock, request.addressLo, request.speed});
            Serve(request);
        }
    }

    // Config ROM reads at or past `quadlet`.
    [[nodiscard]] size_t ROMReadsFrom(uint32_t quadlet) const {
        size_t count = 0;
        for (const auto& request : served_) {
            if (!request.lock && request.addressLo >= kConfigROMBase + quadlet * 4U) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] size_t IRMReads() const {
        size_t count = 0;
        for (const auto& request : served_) {
            if (!request.lock && request.addressLo < kConfigROMBase) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] size_t Locks() const {
        size_t count = 0;
        for (const auto& request : served_) {
            count += request.lock ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] const std::vector<Served>& ServedRequests() const { return served_; }

  private:
    struct Pending {
        bool lock{false};
        uint32_t addressLo{0};
        FwSpeed speed{FwSpeed::S100};
        ASFW::Async::InterfaceCompletionCallback callback;
    };

    static std::array<uint8_t, 4> BigEndian(uint32_t quadlet) {
        return {static_cast<uint8_t>(quadlet >> 24), static_cast<uint8_t>(quadlet >> 16),
                static_cast<uint8_t>(quadlet >> 8), static_cast<uint8_t>(quadlet)};
    }

    void Serve(Pending& request) {
        if (request.lock || request.addressLo < kConfigROMBase) {
            // IRM CHANNELS_AVAILABLE read and compare-swap: all channels free.
            const auto bytes = BigEndian(0xFFFFFFFFU);
            request.callback(AsyncStatus::kSuccess, std::span<const uint8_t>{bytes});
            return;
        }
        const uint32_t quadlet = (request.addressLo - kConfigROMBase) / 4U;
        if (failQuadlet_ == quadlet) {
            failQuadlet_.reset();
            request.callback(AsyncStatus::kTimeout, {});
            return;
        }
        if (quadlet >= image_.size()) {
            request.callback(AsyncStatus::kHardwareError, {});
            return;
        }
        const auto bytes = BigEndian(image_[quadlet]);
        request.callback(AsyncStatus::kSuccess, std::span<const uint8_t>{bytes});
    }

    std::vector<uint32_t> image_;
    std::deque<Pending> pending_;
    std::vector<Served> served_;
    std::optional<uint32_t> failQuadlet_;
    uint32_t handles_{0};
};

struct DeviceROM {
    uint8_t romGeneration{1};
    bool withUnit{true};
    bool irmc{false};
    uint8_t linkSpd{0};
    uint16_t crc{0x1234};
    uint32_t guidLo{kGuidLo};
    std::string_view vendor{"ACME"};
};

constexpr uint32_t DirectoryHeader(uint16_t length) { return static_cast<uint32_t>(length) << 16; }

constexpr uint32_t Immediate(uint8_t key, uint32_t value) {
    return (static_cast<uint32_t>(ASFW::FW::EntryType::kImmediate) << 30) |
           (static_cast<uint32_t>(key) << 24) | (value & 0x00FFFFFFU);
}

// An entry at `entryQuadlet` pointing `targetQuadlet` (both absolute).
constexpr uint32_t Offset(uint8_t type, uint8_t key, uint32_t entryQuadlet, uint32_t targetQuadlet) {
    return (static_cast<uint32_t>(type) << 30) | (static_cast<uint32_t>(key) << 24) |
           ((targetQuadlet - entryQuadlet) & 0x00FFFFFFU);
}

// Bus info block, root directory [vendor ID, vendor text leaf, (unit
// directory)], an optional unit directory, then the text leaf.
std::vector<uint32_t> BuildImage(const DeviceROM& device) {
    auto options = ASFW::FW::DecodeBusOptions(0);
    options.irmc = device.irmc;
    options.maxRec = 8;
    options.generation = device.romGeneration;
    options.linkSpd = device.linkSpd;

    constexpr uint32_t kRoot = 5;
    const uint32_t rootEntries = device.withUnit ? 3U : 2U;
    const uint32_t unitDir = kRoot + 1U + rootEntries;
    const uint32_t leaf = device.withUnit ? unitDir + 3U : unitDir;

    std::vector<uint32_t> rom = {
        (4U << 24) | (0x20U << 16) | device.crc, 0x31333934, ASFW::FW::EncodeBusOptions(options),
        kGuidHi, device.guidLo};
    rom.push_back(DirectoryHeader(static_cast<uint16_t>(rootEntries)));
    rom.push_back(Immediate(ASFW::FW::ConfigKey::kModuleVendorId, 0x000A92));
    rom.push_back(Offset(ASFW::FW::EntryType::kLeaf, ASFW::FW::ConfigKey::kTextualDescriptor,
                         kRoot + 2U, leaf));
    if (device.withUnit) {
        rom.push_back(Offset(ASFW::FW::EntryType::kDirectory, ASFW::FW::ConfigKey::kUnitDirectory,
                             kRoot + 3U, unitDir));
        rom.push_back(DirectoryHeader(2));
        rom.push_back(Immediate(ASFW::FW::ConfigKey::kUnitSpecId, 0x00A02D));
        rom.push_back(Immediate(ASFW::FW::ConfigKey::kUnitSwVersion, 0x010001));
    }

    // Minimal ASCII text leaf (IEEE 1212-2001 Figure 28), NUL-padded.
    std::vector<uint8_t> text(device.vendor.begin(), device.vendor.end());
    text.resize(((text.size() + 1U + 3U) / 4U) * 4U, 0);
    rom.push_back(static_cast<uint32_t>(2U + text.size() / 4U) << 16);
    rom.push_back(0);
    rom.push_back(0);
    for (size_t i = 0; i < text.size(); i += 4) {
        rom.push_back((static_cast<uint32_t>(text[i]) << 24) | (static_cast<uint32_t>(text[i + 1]) << 16) |
                      (static_cast<uint32_t>(text[i + 2]) << 8) | text[i + 3]);
    }
    return rom;
}

struct ScanSetup {
    Generation gen{7};
    uint8_t deviceNode{1};
    uint8_t pathSpeedCode{0};
    bool irmCheck{false};
    std::vector<ConfigROM> previous;
};

struct ScanOutcome {
    int completions{0};
    std::vector<ConfigROM> roms;
    std::optional<FwSpeed> observedSpeed;
};

// Local node 0 and the device, cabled directly: a valid two-node topology,
// as in ROMSpeedProbeTests, so the path speed drives the speed probe.
ScanOutcome Scan(RomImageBus& bus, const ScanSetup& setup) {
    SpeedPolicy speedPolicy;
    ROMScannerParams params{};
    params.configROMReadyRetries = 0;
    params.doIRMCheck = setup.irmCheck;
    ROMScanner scanner(bus, speedPolicy, params);

    TopologySnapshot topology{};
    topology.generation = setup.gen.value;
    topology.localNodeId = 0;
    topology.rootNodeId = setup.deviceNode;
    topology.irmNodeId = setup.deviceNode;
    topology.graphStatus = TopologyGraphStatus::Valid;
    topology.physical.nodes.push_back(
        {.physicalId = 0, .linkActive = true, .speedCode = 3, .portCount = 1});
    topology.physical.nodes.push_back({.physicalId = setup.deviceNode, .linkActive = true,
                                       .speedCode = setup.pathSpeedCode, .portCount = 1});
    topology.physical.nodes[0].links[0] = {.connected = true, .remoteNodeId = setup.deviceNode};
    topology.physical.nodes[1].links[0] = {.connected = true, .remoteNodeId = 0};

    ROMScanRequest request{};
    request.gen = setup.gen;
    request.topology = topology;
    request.localNodeId = 0;
    request.previousROMs = setup.previous;

    ScanOutcome outcome;
    EXPECT_TRUE(scanner.Start(request, [&outcome](Generation, std::vector<ConfigROM> roms, bool) {
        ++outcome.completions;
        outcome.roms = std::move(roms);
    }));
    bus.ServeAll();
    outcome.observedSpeed = speedPolicy.ObservedSpeed(ASFW::FW::NodeId{setup.deviceNode});
    return outcome;
}

// The previous generation: one full scan of `device`.
std::vector<ConfigROM> FirstScan(const DeviceROM& device, Generation gen = Generation{6}) {
    RomImageBus bus(BuildImage(device));
    auto outcome = Scan(bus, {.gen = gen});
    EXPECT_EQ(outcome.completions, 1);
    EXPECT_EQ(outcome.roms.size(), 1U);
    return outcome.roms;
}

} // namespace

TEST(ROMScannerUnchangedROM, FirstScanReadsTheWholeROM) {
    RomImageBus bus(BuildImage({}));
    const auto outcome = Scan(bus, {});

    ASSERT_EQ(outcome.roms.size(), 1U);
    const auto& rom = outcome.roms.front();
    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    EXPECT_FALSE(rom.readIncomplete);
    EXPECT_EQ(rom.vendorName, "ACME");
    EXPECT_EQ(rom.unitDirectories.size(), 1U);
}

TEST(ROMScannerUnchangedROM, UnchangedBusInfoBlockSkipsTheRestOfTheROM) {
    const auto previous = FirstScan({});

    RomImageBus bus(BuildImage({}));
    const auto outcome = Scan(bus, {.gen = Generation{7}, .previous = previous});

    EXPECT_EQ(outcome.completions, 1);
    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    EXPECT_EQ(bus.ROMReadsFrom(0), 4U) << "q0, then q2..q4 (q1 is the fixed bus name)";
    ASSERT_EQ(outcome.roms.size(), 1U);
    const auto& rom = outcome.roms.front();
    EXPECT_EQ(rom.gen, Generation{7});
    EXPECT_EQ(rom.nodeId, 1U);
    EXPECT_EQ(rom.state, ROMState::Fresh);
    EXPECT_EQ(rom.rawQuadlets, previous.front().rawQuadlets);
    EXPECT_EQ(rom.vendorName, "ACME");
    EXPECT_EQ(rom.unitDirectories.size(), 1U);
}

// Node numbers are reassigned by every reset; the match is by content.
TEST(ROMScannerUnchangedROM, DeviceOnANewNodeNumberIsStillMatched) {
    const auto previous = FirstScan({});

    RomImageBus bus(BuildImage({}));
    const auto outcome = Scan(bus, {.deviceNode = 2, .previous = previous});

    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_EQ(outcome.roms.front().nodeId, 2U);
}

TEST(ROMScannerUnchangedROM, AdvancedROMGenerationReadsInFull) {
    const auto previous = FirstScan({});

    RomImageBus bus(BuildImage({.romGeneration = 2, .vendor = "NEWNAME"}));
    const auto outcome = Scan(bus, {.previous = previous});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_EQ(outcome.roms.front().vendorName, "NEWNAME");
}

TEST(ROMScannerUnchangedROM, ChangedHeaderCRCReadsInFull) {
    const auto previous = FirstScan({});

    RomImageBus bus(BuildImage({.crc = 0x4321, .vendor = "NEWNAME"}));
    const auto outcome = Scan(bus, {.previous = previous});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_EQ(outcome.roms.front().vendorName, "NEWNAME");
}

TEST(ROMScannerUnchangedROM, AnotherDeviceReadsInFull) {
    const auto previous = FirstScan({});

    RomImageBus bus(BuildImage({.guidLo = kGuidLo + 1U}));
    const auto outcome = Scan(bus, {.previous = previous});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_EQ(outcome.roms.front().bib.guid, (uint64_t{kGuidHi} << 32) | (kGuidLo + 1U));
}

// A scan that lost a read keeps what it had; reusing that copy would keep the
// gap until the device changes. The next generation reads it again.
TEST(ROMScannerUnchangedROM, CopyWithAFailedReadIsReadAgain) {
    const auto image = BuildImage({});
    const auto leafHeader = static_cast<uint32_t>(image.size()) - 5U;  // 3 + 2 text quadlets
    RomImageBus firstBus(image);
    firstBus.FailReadOfQuadlet(leafHeader);
    const auto first = Scan(firstBus, {.gen = Generation{6}});
    ASSERT_EQ(first.roms.size(), 1U);
    ASSERT_TRUE(first.roms.front().readIncomplete);
    ASSERT_TRUE(first.roms.front().vendorName.empty());

    RomImageBus bus(image);
    const auto outcome = Scan(bus, {.previous = first.roms});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_FALSE(outcome.roms.front().readIncomplete);
    EXPECT_EQ(outcome.roms.front().vendorName, "ACME");
}

// Apple re-reads a generation-0 ROM without units: such devices may publish
// their units late without advancing the ROM generation.
TEST(ROMScannerUnchangedROM, GenerationZeroROMWithoutUnitsIsReadAgain) {
    const DeviceROM device{.romGeneration = 0, .withUnit = false};
    const auto previous = FirstScan(device);

    RomImageBus bus(BuildImage(device));
    (void)Scan(bus, {.previous = previous});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
}

TEST(ROMScannerUnchangedROM, GenerationZeroROMWithUnitsIsReused) {
    const DeviceROM device{.romGeneration = 0};
    const auto previous = FirstScan(device);

    RomImageBus bus(BuildImage(device));
    (void)Scan(bus, {.previous = previous});

    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
}

// Bus generations are 8 bits and wrap (OHCI 1.2 Table 11-2); the match never
// orders them.
TEST(ROMScannerUnchangedROM, ResetAcrossTheGenerationWrapReusesTheROM) {
    const auto previous = FirstScan({}, Generation{255});

    RomImageBus bus(BuildImage({}));
    const auto outcome = Scan(bus, {.gen = Generation{0}, .previous = previous});

    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    ASSERT_EQ(outcome.roms.size(), 1U);
    EXPECT_EQ(outcome.roms.front().gen, Generation{0});
}

// The IRM check is per-generation bus state, not ROM content.
TEST(ROMScannerUnchangedROM, IRMCheckStillRunsForAnUnchangedROM) {
    const DeviceROM device{.irmc = true};
    const auto previous = FirstScan(device);

    RomImageBus bus(BuildImage(device));
    const auto outcome = Scan(bus, {.irmCheck = true, .previous = previous});

    EXPECT_EQ(bus.IRMReads(), 1U);
    EXPECT_EQ(bus.Locks(), 1U);
    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    EXPECT_EQ(outcome.roms.size(), 1U);
}

// The path to a node can change with a reset; the speed probe still runs.
TEST(ROMScannerUnchangedROM, SpeedProbeStillRunsForAnUnchangedROM) {
    const DeviceROM device{.linkSpd = 0};
    const auto previous = FirstScan(device);

    RomImageBus bus(BuildImage(device));
    const auto outcome = Scan(bus, {.pathSpeedCode = 2, .previous = previous});

    bool probedAtS400 = false;
    for (const auto& request : bus.ServedRequests()) {
        probedAtS400 |= request.addressLo == kConfigROMBase && request.speed == FwSpeed::S400;
    }
    EXPECT_TRUE(probedAtS400);
    EXPECT_EQ(outcome.observedSpeed, FwSpeed::S400);
    EXPECT_EQ(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
    EXPECT_EQ(outcome.roms.size(), 1U);
}

// A manual reread passes no previous ROMs.
TEST(ROMScannerUnchangedROM, NoPreviousROMsReadsInFull) {
    RomImageBus bus(BuildImage({}));
    (void)Scan(bus, {});

    EXPECT_GT(bus.ROMReadsFrom(kBusInfoBlockQuadlets), 0U);
}

TEST(UnchangedROMPolicy, CopyHoldingOnlyTheBusInfoBlockIsNotReused) {
    ConfigROM rom{};
    rom.bib.generation = 1;
    rom.rawQuadlets = {1, 2, 3, 4, 5};
    const std::array<uint32_t, 5> busInfoBlock{1, 2, 3, 4, 5};

    EXPECT_FALSE(UnchangedROMPolicy::IsSame(busInfoBlock, rom));
    rom.rawQuadlets.push_back(6);
    EXPECT_TRUE(UnchangedROMPolicy::IsSame(busInfoBlock, rom));
}
