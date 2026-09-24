#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"

#include <string>
#include <vector>

namespace {
using namespace ASFW::Discovery;

DeviceRecord Record(uint32_t model, Generation generation, uint16_t node)
{
    DeviceRecord record{};
    record.guid = 0x0011223344556677ULL;
    record.vendorId = 0x00000D6C;
    record.modelId = model;
    record.kind = DeviceKind::AV_C;
    record.gen = generation;
    record.nodeId = node;
    record.identity.observedGuid = record.guid;
    record.identity.rootVendorId = record.vendorId;
    record.identity.rootModelId = model;
    return record;
}

ConfigROM ROM(Generation generation, uint16_t node)
{
    ConfigROM rom{};
    rom.bib.guid = 0x0011223344556677ULL;
    rom.gen = generation;
    rom.nodeId = node;
    return rom;
}

class Events final : public IDeviceObserver {
public:
    void OnDeviceAdded(std::shared_ptr<FWDevice> device) override {
        events.push_back("add:" + std::to_string(device->GetModelID()));
    }
    void OnDeviceResumed(std::shared_ptr<FWDevice> device) override {
        events.push_back("resume:" + std::to_string(device->GetModelID()));
    }
    void OnDeviceSuspended(std::shared_ptr<FWDevice>) override {}
    void OnDeviceRemoved(Guid64) override { events.push_back("remove"); }
    std::vector<std::string> events;
};

TEST(DeviceManagerIdentityReplacement, SameGuidWithNewROMPersonaReplacesImmutableDevice)
{
    DeviceManager manager;
    Events events;
    manager.RegisterDeviceObserver(&events);
    auto bootloader = manager.UpsertDevice(Record(0x00010070, Generation{1}, 0x21),
                                          ROM(Generation{1}, 0x21));
    ASSERT_NE(nullptr, bootloader);
    EXPECT_EQ(0x00010070U, bootloader->GetModelID());

    auto operational = manager.UpsertDevice(Record(0x00010071, Generation{2}, 0x22),
                                             ROM(Generation{2}, 0x22));
    ASSERT_NE(nullptr, operational);
    EXPECT_NE(bootloader.get(), operational.get());
    EXPECT_TRUE(bootloader->IsTerminated());
    EXPECT_EQ(0x00010071U, operational->GetModelID());
    EXPECT_EQ(Generation{2}, operational->GetGeneration());
    EXPECT_EQ(0x22U, operational->GetNodeID());
    EXPECT_EQ((std::vector<std::string>{"add:65648", "remove", "add:65649"}),
              events.events);
    manager.UnregisterDeviceObserver(&events);
}

TEST(DeviceManagerIdentityReplacement, SameIdentityAcrossResetResumesExistingDevice)
{
    DeviceManager manager;
    Events events;
    manager.RegisterDeviceObserver(&events);
    auto first = manager.UpsertDevice(Record(0x00010071, Generation{1}, 0x21),
                                      ROM(Generation{1}, 0x21));
    ASSERT_NE(nullptr, first);
    first->Suspend();

    auto rebound = manager.UpsertDevice(Record(0x00010071, Generation{2}, 0x22),
                                        ROM(Generation{2}, 0x22));
    EXPECT_EQ(first.get(), rebound.get());
    EXPECT_EQ(Generation{2}, rebound->GetGeneration());
    EXPECT_EQ(0x22U, rebound->GetNodeID());
    EXPECT_EQ((std::vector<std::string>{"add:65649", "resume:65649"}), events.events);
    manager.UnregisterDeviceObserver(&events);
}
} // namespace
