#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2ManagementORB.hpp"
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
using ASFW::Protocols::SBP2::SBP2ManagementORB;
using ASFW::Protocols::SBP2::SBP2SessionRegistry;
namespace SCSI = ASFW::Protocols::SBP2::SCSI;
using ASFW::Protocols::SBP2::Wire::FromBE16;
using ASFW::Protocols::SBP2::Wire::FromBE32;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::NormalORB;
using ASFW::Protocols::SBP2::Wire::ReconnectORB;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
using ASFW::Protocols::SBP2::Wire::TaskManagementORB;
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

uint16_t ReadTaskManagementFunction(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t optionsAndLoginID =
        FromBE32(ReadQuadlet(manager, orbAddress + offsetof(TaskManagementORB, options)));
    return static_cast<uint16_t>((optionsAndLoginID >> 16) & 0x000Fu);
}

uint16_t ReadTaskManagementLoginID(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t optionsAndLoginID =
        FromBE32(ReadQuadlet(manager, orbAddress + offsetof(TaskManagementORB, options)));
    return static_cast<uint16_t>(optionsAndLoginID & 0xFFFFu);
}

uint64_t ReadTaskManagementStatusAddress(AddressSpaceManager& manager, uint64_t orbAddress) {
    return ReadORBAddress(manager,
                          orbAddress,
                          offsetof(TaskManagementORB, statusFIFOAddressHi),
                          offsetof(TaskManagementORB, statusFIFOAddressLo));
}

void CompleteTaskManagementStatus(AddressSpaceManager& manager,
                                  uint64_t statusAddress,
                                  uint64_t orbAddress,
                                  uint8_t sbpStatus = SBPStatus::kNoAdditionalInfo) {
    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = sbpStatus;
    status.orbOffsetHi = ToBE16(static_cast<uint16_t>((orbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = ToBE32(static_cast<uint32_t>(orbAddress & 0xFFFF'FFFFu));
    manager.ApplyRemoteWrite(
        statusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
}

class SessionRegistryRig {
public:
    explicit SessionRegistryRig(uint32_t unitCharacteristics = 0x080400)
        : registry(bus, bus, addressManager, deviceManager, &queue) {
        queue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });

        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);

        UpsertDevice(Generation{1}, 0x32, unitCharacteristics);
    }

    ~SessionRegistryRig() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void UpsertDevice(Generation generation, uint8_t nodeId, uint32_t unitCharacteristics = 0x080400) {
        DeviceRecord record{};
        record.guid = kGuid;
        record.vendorId = 0x001122;
        record.modelId = 0x334455;
        record.kind = DeviceKind::Unknown;
        record.vendorName = "Scanner Vendor";
        record.modelName = "Scanner Model";
        record.gen = generation;
        record.nodeId = nodeId;
        record.link = LinkPolicy{};
        record.state = LifeState::Ready;

        ConfigROM rom{};
        rom.gen = generation;
        rom.nodeId = record.nodeId;
        rom.vendorName = record.vendorName;
        rom.modelName = record.modelName;
        rom.rootDirMinimal = {
            RomEntry{CfgKey::Unit_Spec_Id, kSBP2UnitSpecId, 0, 0},
            RomEntry{CfgKey::Unit_Sw_Version, kSBP2UnitSwVersion, 0, 0},
            RomEntry{CfgKey::Logical_Unit_Number, 0x000002, 0, 0},
            RomEntry{CfgKey::Management_Agent_Offset, 0x000080, 1, 0},
            RomEntry{CfgKey::Unit_Characteristics, unitCharacteristics, 0, 0},
        };

        auto device = deviceManager.UpsertDevice(record, rom);
        EXPECT_NE(nullptr, device);
        if (!device) {
            return;
        }
        EXPECT_FALSE(device->GetUnits().empty());
    }

    void AdvanceMs(uint64_t milliseconds) {
        nowNs += milliseconds * 1'000'000ULL;
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    uint64_t CreateSession() {
        auto result = registry.CreateSession(reinterpret_cast<void*>(0xCAFE), kGuid, 0);
        EXPECT_TRUE(result.has_value());
        const uint64_t handle = result.value_or(0);
        if (auto* session = registry.GetSessionForTesting(handle)) {
            session->SetTimeoutQueue(&queue);
        }
        return handle;
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

TEST(SBP2SessionRegistryTests, ActiveCommandFailsOnceAfterFetchAgentRetryExhaustion) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    session->SetFetchAgentWriteRetriesForTesting(0);
    const auto request = SCSI::BuildTestUnitReadyRequest();
    ASSERT_TRUE(rig.registry.SubmitCommand(handle, request));
    const auto fetchAgentWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8u, fetchAgentWrite.data.size());

    ASSERT_TRUE(rig.bus.CompleteWrite(fetchAgentWrite.handle, ASFW::Async::AsyncStatus::kTimeout));
    rig.AdvanceMs(1000);
    while (rig.queue.DrainReadyForTesting() > 0U) {
    }

    const auto commandResult = rig.registry.GetCommandResult(handle);
    ASSERT_TRUE(commandResult.has_value());
    EXPECT_EQ(-1, commandResult->transportStatus);
    EXPECT_EQ(SBPStatus::kUnspecifiedError, commandResult->sbpStatus);

    const auto stateAfterFailure = rig.registry.GetSessionState(handle);
    ASSERT_TRUE(stateAfterFailure.has_value());
    EXPECT_EQ(-1, stateAfterFailure->lastError);

    ASSERT_GT(rig.bus.WriteCount(), 0u);
    const auto agentResetWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    EXPECT_EQ(4u, agentResetWrite.data.size());

    ASSERT_TRUE(rig.bus.CompleteWrite(agentResetWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    while (rig.queue.DrainReadyForTesting() > 0U) {
    }
}

TEST(SBP2SessionRegistryTests, SubmitTaskManagementWritesLogicalUnitResetORB) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const size_t writesBeforeSubmit = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitTaskManagement(
        handle, SBP2ManagementORB::Function::LogicalUnitReset));

    ASSERT_EQ(writesBeforeSubmit + 1U, rig.bus.WriteCount());
    const auto& write = rig.bus.WriteAt(writesBeforeSubmit);
    const uint64_t managementOrbAddress = DecodeAddressFromWritePayload(write.data);

    EXPECT_EQ(0x0Eu, ReadTaskManagementFunction(rig.addressManager, managementOrbAddress));
    EXPECT_EQ(0x0042u, ReadTaskManagementLoginID(rig.addressManager, managementOrbAddress));
}

TEST(SBP2SessionRegistryTests, TaskManagementSuccessClearsActiveCommandTracking) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const auto request = SCSI::BuildTestUnitReadyRequest();
    ASSERT_TRUE(rig.registry.SubmitCommand(handle, request));
    ASSERT_FALSE(rig.registry.GetCommandResult(handle).has_value());
    ASSERT_GT(rig.bus.WriteCount(), 0U);
    const auto fetchAgentWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8U, fetchAgentWrite.data.size());

    const size_t writesBeforeTaskManagement = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitTaskManagement(
        handle, SBP2ManagementORB::Function::AbortTaskSet));
    const auto& taskWrite = rig.bus.WriteAt(writesBeforeTaskManagement);
    const uint64_t taskOrbAddress = DecodeAddressFromWritePayload(taskWrite.data);
    const uint64_t taskStatusAddress =
        ReadTaskManagementStatusAddress(rig.addressManager, taskOrbAddress);

    ASSERT_TRUE(rig.bus.CompleteWrite(taskWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    CompleteTaskManagementStatus(rig.addressManager, taskStatusAddress, taskOrbAddress);

    EXPECT_FALSE(rig.registry.GetCommandResult(handle).has_value());
    EXPECT_FALSE(rig.bus.CompleteWrite(fetchAgentWrite.handle,
                                       ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_TRUE(rig.registry.SubmitCommand(handle, request));
}

TEST(SBP2SessionRegistryTests, SubmitTaskManagementRejectsInvalidFunction) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    EXPECT_FALSE(rig.registry.SubmitTaskManagement(
        handle, SBP2ManagementORB::Function::QueryLogins));
}

TEST(SBP2SessionRegistryTests, MissingDiscoverySuspendsDeviceAndReconnectWaitsForResume) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    rig.registry.OnBusReset(2);
    const auto suspendedState = rig.registry.GetSessionState(handle);
    ASSERT_TRUE(suspendedState.has_value());
    EXPECT_EQ(ASFW::Protocols::SBP2::LoginState::Suspended, suspendedState->loginState);

    rig.deviceManager.MarkDeviceLost(SessionRegistryRig::kGuid);
    const auto suspendedDevice = rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid);
    ASSERT_NE(nullptr, suspendedDevice);
    EXPECT_TRUE(suspendedDevice->IsSuspended());

    const size_t writesBeforeMissingRefresh = rig.bus.WriteCount();
    rig.registry.RefreshTargets(Generation{2});
    EXPECT_EQ(writesBeforeMissingRefresh, rig.bus.WriteCount());

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.UpsertDevice(Generation{2}, 0x33);
    const auto resumedDevice = rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid);
    ASSERT_NE(nullptr, resumedDevice);
    EXPECT_TRUE(resumedDevice->IsReady());
    EXPECT_EQ(0x33u, resumedDevice->GetNodeID());

    rig.registry.RefreshTargets(Generation{2});
    ASSERT_EQ(writesBeforeMissingRefresh + 1U, rig.bus.WriteCount());
    const auto& reconnectWrite = rig.bus.WriteAt(writesBeforeMissingRefresh);
    EXPECT_EQ(0x33u, reconnectWrite.nodeId.value);
    EXPECT_EQ(8U, reconnectWrite.data.size());

    const uint64_t reconnectOrbAddress = DecodeAddressFromWritePayload(reconnectWrite.data);
    const uint64_t reconnectStatusAddress =
        ReadORBAddress(rig.addressManager,
                       reconnectOrbAddress,
                       offsetof(ReconnectORB, statusFIFOAddressHi),
                       offsetof(ReconnectORB, statusFIFOAddressLo));
    ASSERT_TRUE(rig.bus.CompleteWrite(reconnectWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    StatusBlock reconnectStatus{};
    reconnectStatus.details = 0;
    reconnectStatus.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        reconnectStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&reconnectStatus),
                                 sizeof(reconnectStatus)});

    ASSERT_GE(rig.bus.WriteCount(), writesBeforeMissingRefresh + 2U);
    const auto& busyTimeoutWrite = rig.bus.WriteAt(writesBeforeMissingRefresh + 1U);
    EXPECT_EQ(0x33u, busyTimeoutWrite.nodeId.value);
    EXPECT_EQ(0x33u, busyTimeoutWrite.address.nodeID);

    const size_t writesBeforeCommand = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitCommand(handle, SCSI::BuildTestUnitReadyRequest()));
    ASSERT_EQ(writesBeforeCommand + 1U, rig.bus.WriteCount());
    const auto& fetchAgentWrite = rig.bus.WriteAt(writesBeforeCommand);
    EXPECT_EQ(0x33u, fetchAgentWrite.nodeId.value);
    EXPECT_EQ(0x33u, fetchAgentWrite.address.nodeID);
}

TEST(SBP2SessionRegistryTests, RepeatedMissingDiscoveryTerminatesSuspendedSBP2Device) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    rig.registry.OnBusReset(2);
    rig.deviceManager.MarkDeviceLost(SessionRegistryRig::kGuid);
    const auto suspendedDevice = rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid);
    ASSERT_NE(nullptr, suspendedDevice);
    ASSERT_TRUE(suspendedDevice->IsSuspended());

    rig.deviceManager.MarkDeviceLost(SessionRegistryRig::kGuid);
    EXPECT_EQ(nullptr, rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid));

    const size_t writesBeforeRefresh = rig.bus.WriteCount();
    rig.registry.RefreshTargets(Generation{2});
    EXPECT_EQ(writesBeforeRefresh, rig.bus.WriteCount());
}

TEST(SBP2SessionRegistryTests, ReleaseOwnerRetainsSessionUntilLogoutStatusArrives) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    std::weak_ptr<ASFW::Protocols::SBP2::SBP2LoginSession> weakSession =
        rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(reinterpret_cast<void*>(0xCAFE));

    EXPECT_FALSE(weakSession.expired());
    ASSERT_EQ(writesBeforeRelease + 1U, rig.bus.WriteCount());
    EXPECT_FALSE(rig.registry.GetSessionState(handle).has_value());

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    EXPECT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_FALSE(weakSession.expired());

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    EXPECT_TRUE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(handle).has_value());
}

TEST(SBP2SessionRegistryTests, ReleaseOwnerRetainsSessionUntilLogoutTimeout) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    std::weak_ptr<ASFW::Protocols::SBP2::SBP2LoginSession> weakSession =
        rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(reinterpret_cast<void*>(0xCAFE));

    EXPECT_FALSE(weakSession.expired());
    ASSERT_EQ(writesBeforeRelease + 1U, rig.bus.WriteCount());
    EXPECT_FALSE(rig.registry.GetSessionState(handle).has_value());

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    EXPECT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_FALSE(weakSession.expired());

    rig.AdvanceMs(2'000);

    EXPECT_TRUE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(handle).has_value());
}

TEST(SBP2SessionRegistryTests, MissingDiscoveryStillTerminatesNonSBP2Device) {
    DeviceManager deviceManager;

    DeviceRecord record{};
    record.guid = SessionRegistryRig::kGuid + 2U;
    record.vendorId = 0x001122;
    record.modelId = 0x334455;
    record.kind = DeviceKind::Unknown;
    record.vendorName = "Other Vendor";
    record.modelName = "Other Device";
    record.gen = Generation{1};
    record.nodeId = 0x10;
    record.link = LinkPolicy{};
    record.state = LifeState::Ready;

    ConfigROM rom{};
    rom.gen = Generation{1};
    rom.nodeId = record.nodeId;
    rom.rootDirMinimal = {
        RomEntry{CfgKey::Unit_Spec_Id, 0x00A02D, 0, 0},
        RomEntry{CfgKey::Unit_Sw_Version, 0x010001, 0, 0},
    };

    ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
    deviceManager.MarkDeviceLost(record.guid);
    EXPECT_EQ(nullptr, deviceManager.GetDeviceByGUID(record.guid));
}

TEST(SBP2SessionRegistryTests, SubmitCommandRejectsCDBLargerThanORBPayloadBudget) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(16, 0x12);
    request.direction = SCSI::DataDirection::None;
    request.transferLength = 0;
    request.timeoutMs = 100;

    EXPECT_FALSE(rig.registry.SubmitCommand(handle, request));
}

TEST(SBP2SessionRegistryTests, CreateSessionAcceptsRealSBP2SpecAndVersion) {
    SessionRegistryRig rig;
    auto result = rig.registry.CreateSession(reinterpret_cast<void*>(0xCAFE),
                                             SessionRegistryRig::kGuid,
                                             0);
    ASSERT_TRUE(result.has_value());
}

TEST(SBP2SessionRegistryTests, UnitCharacteristicsDecodeTimeoutAndORBSizeFromLowBytes) {
    SessionRegistryRig rig(0x000410);

    const uint64_t handle = rig.CreateSession();
    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    const auto& targetInfo = session->TargetInfo();
    EXPECT_EQ(2000u, targetInfo.managementTimeoutMs);
    EXPECT_EQ(64u, targetInfo.maxORBSize);
    EXPECT_EQ(64u - NormalORB::kHeaderSize, targetInfo.maxCommandBlockSize);
}

TEST(SBP2SessionRegistryTests, DeviceDiscoveryParsesNikonStyleManagementAgentCSRKey) {
    DeviceManager deviceManager;

    DeviceRecord record{};
    record.guid = SessionRegistryRig::kGuid + 1U;
    record.vendorId = 0x0090B5;
    record.modelId = 0x004001;
    record.kind = DeviceKind::Unknown;
    record.vendorName = "Nikon";
    record.modelName = "LS-4000 ED";
    record.gen = Generation{1};
    record.nodeId = 0x00;
    record.link = LinkPolicy{};
    record.state = LifeState::Ready;

    ConfigROM rom{};
    rom.gen = Generation{1};
    rom.nodeId = record.nodeId;
    rom.bib.busInfoLength = 4;
    rom.rootDirMinimal = {
        RomEntry{CfgKey::Unit_Directory, 0x000001, 3, 1},
    };
    rom.rawQuadlets = {
        ToBE32(0x04045343u),
        ToBE32(0x31333934u),
        ToBE32(0x00FF5012u),
        ToBE32(0x0090B540u),
        ToBE32(0x01FFFFFFu),
        ToBE32(0x0001B344u),
        ToBE32(0x0004CAEEu),
        ToBE32(0x1200609Eu),
        ToBE32(0x13010483u),
        ToBE32(0x5400C000u),
        ToBE32(0x14060000u),
    };

    auto device = deviceManager.UpsertDevice(record, rom);
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->GetUnits().size(), 1u);

    const auto& unit = device->GetUnits().front();
    ASSERT_NE(unit, nullptr);
    EXPECT_TRUE(unit->Matches(kSBP2UnitSpecId, kSBP2UnitSwVersion));
    ASSERT_TRUE(unit->GetManagementAgentOffset().has_value());
    EXPECT_EQ(*unit->GetManagementAgentOffset(), 0x00C000u);
    ASSERT_TRUE(unit->GetLUN().has_value());
    EXPECT_EQ(*unit->GetLUN(), 0x060000u);
}

} // namespace
