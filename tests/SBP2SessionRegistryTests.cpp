#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2SessionRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/SCSICommandSet.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kSBP2UnitSpecId = 0x00609E;
constexpr uint32_t kSBP2UnitSwVersion = 0x010483;

using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceKind;
using ASFW::Discovery::DeviceManager;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::Generation;
using ASFW::Discovery::LifeState;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::SBP2SessionRegistry;
namespace SCSI = ASFW::Protocols::SBP2::SCSI;
using ASFW::Protocols::SBP2::Wire::FromBE16;
using ASFW::Protocols::SBP2::Wire::FromBE32;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::NormalORB;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
using ASFW::Protocols::SBP2::Wire::ToBE16;
using ASFW::Protocols::SBP2::Wire::ToBE32;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;

uint64_t ComposeAddress(uint16_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t DecodeAddressFromWritePayload(std::span<const uint8_t> payload) {
    const uint16_t addressHi =
        static_cast<uint16_t>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    const uint32_t addressLo =
        (static_cast<uint32_t>(payload[4]) << 24) |
        (static_cast<uint32_t>(payload[5]) << 16) |
        (static_cast<uint32_t>(payload[6]) << 8) |
        static_cast<uint32_t>(payload[7]);
    return ComposeAddress(addressHi, addressLo);
}

uint32_t ReadQuadlet(AddressSpaceManager& manager, uint64_t address) {
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete, manager.ReadQuadlet(address, &value));
    return value;
}

uint64_t ReadORBAddress(AddressSpaceManager& manager,
                        uint64_t orbAddress,
                        size_t hiOffset,
                        size_t loOffset) {
    const uint32_t hi = FromBE32(ReadQuadlet(manager, orbAddress + hiOffset));
    const uint32_t lo = FromBE32(ReadQuadlet(manager, orbAddress + loOffset));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

uint64_t ReadDataBufferAddress(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t hi = FromBE32(
        ReadQuadlet(manager, orbAddress + offsetof(NormalORB, dataDescriptorHi)));
    const uint32_t lo = FromBE32(
        ReadQuadlet(manager, orbAddress + offsetof(NormalORB, dataDescriptorLo)));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

class SessionRegistryRig {
public:
    SessionRegistryRig()
        : registry(bus, bus, addressManager, deviceManager, &queue) {
        queue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });

        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);

        DeviceRecord record{};
        record.guid = kGuid;
        record.vendorId = 0x001122;
        record.modelId = 0x334455;
        record.kind = DeviceKind::Unknown;
        record.vendorName = "Scanner Vendor";
        record.modelName = "Scanner Model";
        record.gen = Generation{1};
        record.nodeId = 0x32;
        record.link = LinkPolicy{};
        record.state = LifeState::Ready;

        ConfigROM rom{};
        rom.gen = Generation{1};
        rom.nodeId = record.nodeId;
        rom.vendorName = record.vendorName;
        rom.modelName = record.modelName;
        rom.rootDirMinimal = {
            RomEntry{CfgKey::Unit_Spec_Id, kSBP2UnitSpecId, 0, 0},
            RomEntry{CfgKey::Unit_Sw_Version, kSBP2UnitSwVersion, 0, 0},
            RomEntry{CfgKey::Logical_Unit_Number, 0x000002, 0, 0},
            RomEntry{CfgKey::Management_Agent_Offset, 0x000080, 1, 0},
            RomEntry{CfgKey::Unit_Characteristics, 0x080400, 0, 0},
        };

        auto device = deviceManager.UpsertDevice(record, rom);
        EXPECT_NE(nullptr, device);
        if (!device) {
            return;
        }
        EXPECT_FALSE(device->GetUnits().empty());
    }

    ~SessionRegistryRig() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void AdvanceMs(uint64_t milliseconds) {
        nowNs += milliseconds * 1'000'000ULL;
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    uint64_t CreateSession() {
        auto result = registry.CreateSession(reinterpret_cast<void*>(0xCAFE), kGuid, 0);
        EXPECT_TRUE(result.has_value());
        return result.value_or(0);
    }

    void LoginSuccessfully(uint64_t handle,
                           uint16_t loginId = 0x0042,
                           uint32_t commandBlockAgentLo = 0x0020'0000) {
        ASSERT_TRUE(registry.StartLogin(handle));
        ASSERT_EQ(1u, bus.PendingWriteCount());

        const auto& loginWrite = bus.WriteAt(0);
        const uint64_t loginOrbAddress = DecodeAddressFromWritePayload(loginWrite.data);
        const uint64_t loginResponseAddress =
            ReadORBAddress(addressManager,
                           loginOrbAddress,
                           offsetof(LoginORB, loginResponseAddressHi),
                           offsetof(LoginORB, loginResponseAddressLo));
        sessionStatusAddress =
            ReadORBAddress(addressManager,
                           loginOrbAddress,
                           offsetof(LoginORB, statusFIFOAddressHi),
                           offsetof(LoginORB, statusFIFOAddressLo));

        LoginResponse response{};
        response.length = ToBE16(LoginResponse::kSize);
        response.loginID = ToBE16(loginId);
        response.commandBlockAgentAddressHi = ToBE32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = ToBE32(commandBlockAgentLo);
        response.reconnectHold = ToBE16(1);

        ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        addressManager.ApplyRemoteWrite(
            loginResponseAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&response),
                                     sizeof(response)});

        StatusBlock status{};
        status.details = 0;
        status.sbpStatus = SBPStatus::kNoAdditionalInfo;
        addressManager.ApplyRemoteWrite(
            sessionStatusAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
    }

    static constexpr uint64_t kGuid = 0x0003DB0001DDDDA1ULL;

    ASFW::Async::Testing::DeferredFireWireBus bus;
    AddressSpaceManager addressManager{nullptr};
    DeviceManager deviceManager;
    IODispatchQueue queue;
    SBP2SessionRegistry registry;
    uint64_t nowNs{0};
    uint64_t sessionStatusAddress{0};
};

TEST(SBP2SessionRegistryTests, BuildStandardCommandHelpersUseExpectedOpCodes) {
    const auto inquiry = SCSI::BuildInquiryRequest(64);
    ASSERT_EQ(6u, inquiry.cdb.size());
    EXPECT_EQ(0x12, inquiry.cdb[0]);
    EXPECT_EQ(64u, inquiry.transferLength);

    const auto tur = SCSI::BuildTestUnitReadyRequest();
    ASSERT_EQ(6u, tur.cdb.size());
    EXPECT_EQ(0x00, tur.cdb[0]);
    EXPECT_EQ(SCSI::DataDirection::None, tur.direction);

    const auto sense = SCSI::BuildRequestSenseRequest(18);
    ASSERT_EQ(6u, sense.cdb.size());
    EXPECT_EQ(0x03, sense.cdb[0]);
    EXPECT_EQ(SCSI::DataDirection::FromTarget, sense.direction);
}

TEST(SBP2SessionRegistryTests, SubmitRequestSenseCapturesPayloadAndSenseData) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const auto request = SCSI::BuildRequestSenseRequest(18);
    const size_t pendingBeforeSubmit = rig.bus.PendingWriteCount();
    ASSERT_TRUE(rig.registry.SubmitCommand(handle, request));
    ASSERT_EQ(pendingBeforeSubmit + 1U, rig.bus.PendingWriteCount());

    const auto& write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t commandOrbAddress = DecodeAddressFromWritePayload(write.data);
    const uint64_t dataBufferAddress =
        ReadDataBufferAddress(rig.addressManager, commandOrbAddress);

    const std::vector<uint8_t> sensePayload{
        0x70, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x3A, 0x01
    };
    rig.addressManager.ApplyRemoteWrite(dataBufferAddress, sensePayload);

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    status.orbOffsetHi = ToBE16(static_cast<uint16_t>((commandOrbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = ToBE32(static_cast<uint32_t>(commandOrbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    auto result = rig.registry.GetCommandResult(handle);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(0, result->transportStatus);
    EXPECT_EQ(SBPStatus::kNoAdditionalInfo, result->sbpStatus);
    EXPECT_EQ(sensePayload, result->payload);
    EXPECT_EQ(sensePayload, result->senseData);
}

TEST(SBP2SessionRegistryTests, CreateSessionAcceptsRealSBP2SpecAndVersion) {
    SessionRegistryRig rig;
    auto result = rig.registry.CreateSession(reinterpret_cast<void*>(0xCAFE),
                                             SessionRegistryRig::kGuid,
                                             0);
    ASSERT_TRUE(result.has_value());
}

} // namespace
