// SBP2NubPublisherTests — the per-unit nub publisher over the real DeviceManager,
// with ASFWDriver replaced by a fake IOService that hands out mock nubs.
//
// Regression (2026-09-22..24): Shutdown() ran only while ASFWDriver itself was
// stopping, when the kernel had already terminated the driver's client services
// and released their dispatch queues. Calling Terminate() on a nub there aborted
// the dext on every teardown: "Assertion failed: (priv->queueArray), function
// QueueForObject, file uioserver.cpp, line 1056".
#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"
#include "ASFWDriver/SCSIController/SBP2NubPublisher.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"


#include <net.asfw.driver/ASFWSBP2Nub.h>

#include <memory>
#include <vector>

namespace {

using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceKind;
using ASFW::Discovery::DeviceManager;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::Generation;
using ASFW::Discovery::LifeState;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Protocols::SBP2::SBP2NubPublisher;

constexpr uint64_t kGuid = 0x0090B54003FFFFFFULL;

// Stands in for ASFWDriver: Create() returns a mock nub, retained once for the
// registry as the kernel would.
class FakeDriver : public IOService {
public:
    kern_return_t Create(IOService*, const char*, IOService** out) override {
        auto* nub = new ASFWSBP2Nub();
        nub->retain();
        nubs.push_back(nub);
        *out = nub;
        return kIOReturnSuccess;
    }

    std::vector<ASFWSBP2Nub*> nubs;
};

class SBP2NubPublisherTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue.SetManualDispatchForTesting(true);
        publisher = std::make_shared<SBP2NubPublisher>(&driver, deviceManager, &queue);
        publisher->Start();
    }

    void TearDown() override {
        publisher.reset();
        for (auto* nub : driver.nubs) {
            nub->release();
        }
    }

    void PublishScanner() {
        DeviceRecord record{};
        record.guid = kGuid;
        record.vendorId = 0x0090B5;
        record.modelId = 0x000001;
        record.kind = DeviceKind::Unknown;
        record.vendorName = "Nikon";
        record.modelName = "LS-9000";
        record.gen = Generation{1};
        record.nodeId = 0;
        record.link = LinkPolicy{};
        record.state = LifeState::Ready;

        ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = Generation{1};
        rom.nodeId = 0;
        rom.vendorName = record.vendorName;
        rom.modelName = record.modelName;
        rom.rootDirMinimal = {
            RomEntry{CfgKey::Unit_Spec_Id, ASFW::Protocols::SBP2::kSBP2UnitSpecId, 0, 0},
            RomEntry{CfgKey::Unit_Sw_Version, ASFW::Protocols::SBP2::kSBP2UnitSwVersion, 0, 0},
            RomEntry{CfgKey::Logical_Unit_Number, 0x000000, 0, 0},
            RomEntry{CfgKey::Management_Agent_Offset, 0x000080, 1, 0},
        };
        (void)deviceRegistry.UpsertFromROM(rom, record.link);
        ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
        Drain();
        ASSERT_EQ(1u, driver.nubs.size());
    }

    void Drain() {
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    FakeDriver driver;
    DeviceRegistry deviceRegistry;
    DeviceManager deviceManager;
    IODispatchQueue queue;
    std::shared_ptr<SBP2NubPublisher> publisher;
};

TEST_F(SBP2NubPublisherTest, ShutdownDuringDriverStopLeavesNubTerminationToTheKernel) {
    PublishScanner();

    publisher->Shutdown();

    EXPECT_EQ(0, driver.nubs[0]->terminateCalls);
}

TEST_F(SBP2NubPublisherTest, UnitRemovalWhileDriverRunsTerminatesTheNub) {
    PublishScanner();

    deviceManager.TerminateDevice(kGuid);
    Drain();

    EXPECT_EQ(1, driver.nubs[0]->terminateCalls);
}

// A Leaf Valeo 11 style unit directory: the nub carries the unit identity under
// the names Apple's IOFireWireSBP2LUN publishes, plus the bus generation, which
// is refreshed when the unit resumes in a later generation.
TEST_F(SBP2NubPublisherTest, NubPublishesUnitIdentityAndRefreshesBusGeneration) {
    DeviceRecord record{};
    record.guid = kGuid;
    record.vendorId = 0x0006CD;
    record.modelId = 0x000000;
    record.kind = DeviceKind::Unknown;
    record.vendorName = "Leaf";
    record.modelName = "Valeo";
    record.gen = Generation{1};
    record.nodeId = 0;
    record.link = LinkPolicy{};
    record.state = LifeState::Ready;

    ConfigROM rom{};
    rom.bib.guid = kGuid;
    rom.gen = Generation{1};
    rom.nodeId = 0;
    rom.vendorName = record.vendorName;
    rom.modelName = record.modelName;
    rom.rootDirMinimal = {
        RomEntry{CfgKey::Unit_Spec_Id, ASFW::Protocols::SBP2::kSBP2UnitSpecId, 0, 0},
        RomEntry{CfgKey::Unit_Sw_Version, ASFW::Protocols::SBP2::kSBP2UnitSwVersion, 0, 0},
        RomEntry{CfgKey::Command_Set_Spec_Id, 0x00609E, 0, 0},
        RomEntry{CfgKey::Command_Set, 0x0104D8, 0, 0},
        RomEntry{CfgKey::Unit_Characteristics, 0x00A008, 0, 0},
        RomEntry{CfgKey::Command_Set_Revision, 0x000001, 0, 0},
        RomEntry{CfgKey::Firmware_Revision, 0x000000, 0, 0},
        RomEntry{CfgKey::Management_Agent_Offset, 0x004000, 1, 0},
        RomEntry{CfgKey::Logical_Unit_Number, 0x020000, 0, 0},
    };
    (void)deviceRegistry.UpsertFromROM(rom, record.link);
    ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
    Drain();
    ASSERT_EQ(1u, driver.nubs.size());

    const auto& n = driver.nubs[0]->numbers;
    EXPECT_EQ(0x0006CDu, n.at("Vendor_ID"));
    EXPECT_EQ(kGuid, n.at("GUID"));
    EXPECT_EQ(0x00609Eu, n.at("Unit_Spec_ID"));
    EXPECT_EQ(0x00609Eu, n.at("Command_Set_Spec_ID"));
    EXPECT_EQ(0x0104D8u, n.at("Command_Set"));
    EXPECT_EQ(0x00A008u, n.at("Unit_Characteristics"));
    EXPECT_EQ(1u, n.at("Command_Set_Revision"));
    EXPECT_EQ(0u, n.at("Firmware_Revision"));
    EXPECT_EQ(0x004000u, n.at("Management_Agent_Offset"));
    EXPECT_EQ(2u, n.at("Device_Type"));
    EXPECT_EQ(1u, n.at("ASFWBusGeneration"));

    // Bus reset: the unit is suspended, then rediscovered in generation 2.
    deviceManager.MarkDeviceLost(kGuid);
    Drain();
    record.gen = Generation{2};
    rom.gen = Generation{2};
    ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
    Drain();
    ASSERT_EQ(1u, driver.nubs.size()) << "resume must not republish the nub";
    EXPECT_EQ(2u, driver.nubs[0]->numbers.at("ASFWBusGeneration"));
    EXPECT_EQ(1u, driver.nubs[0]->lastPropertyCount) << "the refresh sets only ASFWBusGeneration";
}

} // namespace
