#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2ManagementORB.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/SCSICommandSet.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// SessionRegistry host tests — ported from PR #19's SBP2SessionRegistryTests,
// adapted to the decomposed DICE component: `SessionRegistry` (identity/lifecycle)
// delegates the command plane to a per-record CommandExecutor, and LoginSession
// timers run on an injected ISessionScheduler. The two SBP2Handler-dependent tests
// from #19 move to FW-57 (the session-aware user-client handler does not exist on
// DICE yet). Byte-order uses the global OSSwap* intrinsics (DICE dropped the
// Wire::ToBE*/FromBE* helpers).

namespace {

constexpr uint32_t kSBP2UnitSpecId = 0x00609E;
constexpr uint32_t kSBP2UnitSwVersion = 0x010483;

using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceKind;
using ASFW::Discovery::DeviceManager;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::Generation;
using ASFW::Discovery::kInvalidNodeId;
using ASFW::Discovery::LifeState;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::LoginSession;
using ASFW::Protocols::SBP2::SBP2ManagementORB;
using ASFW::Protocols::SBP2::SessionRegistry;
using ASFW::Testing::FakeSessionScheduler;
namespace SCSI = ASFW::Protocols::SBP2::SCSI;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::NormalORB;
using ASFW::Protocols::SBP2::Wire::ReconnectORB;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
using ASFW::Protocols::SBP2::Wire::TaskManagementORB;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;
// OSSwapHostToBigInt*/OSSwapBigToHostInt* are global byte-order intrinsics.

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
    const uint32_t hi = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + hiOffset));
    const uint32_t lo = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + loOffset));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

uint64_t ReadDataBufferAddress(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t hi = OSSwapBigToHostInt32(
        ReadQuadlet(manager, orbAddress + offsetof(NormalORB, dataDescriptorHi)));
    const uint32_t lo = OSSwapBigToHostInt32(
        ReadQuadlet(manager, orbAddress + offsetof(NormalORB, dataDescriptorLo)));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

uint16_t ReadTaskManagementFunction(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t optionsAndLoginID =
        OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + offsetof(TaskManagementORB, options)));
    return static_cast<uint16_t>((optionsAndLoginID >> 16) & 0x000Fu);
}

uint16_t ReadTaskManagementLoginID(AddressSpaceManager& manager, uint64_t orbAddress) {
    const uint32_t optionsAndLoginID =
        OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + offsetof(TaskManagementORB, options)));
    return static_cast<uint16_t>(optionsAndLoginID & 0xFFFFu);
}

uint64_t ReadTaskManagementStatusAddress(AddressSpaceManager& manager, uint64_t orbAddress) {
    return ReadORBAddress(manager, orbAddress,
                          offsetof(TaskManagementORB, statusFIFOAddressHi),
                          offsetof(TaskManagementORB, statusFIFOAddressLo));
}

void CompleteTaskManagementStatus(AddressSpaceManager& manager,
                                  uint64_t statusAddress,
                                  uint64_t orbAddress,
                                  uint8_t sbpStatus = SBPStatus::kNoAdditionalInfo,
                                  uint8_t details = 0) {
    StatusBlock status{};
    status.details = details;
    status.sbpStatus = sbpStatus;
    status.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((orbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(orbAddress & 0xFFFF'FFFFu));
    manager.ApplyRemoteWrite(
        statusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
}

class SessionRegistryRig {
public:
    explicit SessionRegistryRig(uint32_t unitCharacteristics = 0x080400)
        : registry(bus, bus, addressManager, deviceRegistry, deviceManager, scheduler, &queue) {
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
        rom.bib.guid = kGuid;
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

        (void)deviceRegistry.UpsertFromROM(rom, record.link);

        auto device = deviceManager.UpsertDevice(record, rom);
        EXPECT_NE(nullptr, device);
        if (!device) {
            return;
        }
        EXPECT_FALSE(device->GetUnits().empty());
    }

    void AdvanceMs(uint64_t milliseconds) {
        nowNs += milliseconds * 1'000'000ULL;
        scheduler.Advance(milliseconds * 1'000'000ULL);
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    uint64_t CreateSession() {
        auto result = registry.CreateSession(Owner(), kGuid, 0);
        EXPECT_TRUE(result.has_value());
        return result.value_or(0);
    }

    void LoginSuccessfully(uint64_t handle,
                           uint16_t loginId = 0x0042,
                           uint32_t commandBlockAgentLo = 0x0020'0000) {
        ASSERT_TRUE(registry.StartLogin(Owner(), handle));
        ASSERT_EQ(1u, bus.PendingWriteCount());

        const auto& loginWrite = bus.WriteAt(0);
        const uint64_t loginOrbAddress = DecodeAddressFromWritePayload(loginWrite.data);
        const uint64_t loginResponseAddress =
            ReadORBAddress(addressManager, loginOrbAddress,
                           offsetof(LoginORB, loginResponseAddressHi),
                           offsetof(LoginORB, loginResponseAddressLo));
        sessionStatusAddress =
            ReadORBAddress(addressManager, loginOrbAddress,
                           offsetof(LoginORB, statusFIFOAddressHi),
                           offsetof(LoginORB, statusFIFOAddressLo));

        LoginResponse response{};
        response.length = OSSwapHostToBigInt16(LoginResponse::kSize);
        response.loginID = OSSwapHostToBigInt16(loginId);
        response.commandBlockAgentAddressHi = OSSwapHostToBigInt32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = OSSwapHostToBigInt32(commandBlockAgentLo);
        response.reconnectHold = OSSwapHostToBigInt16(1);

        ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        addressManager.ApplyRemoteWrite(
            loginResponseAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&response), sizeof(response)});

        StatusBlock status{};
        status.details = 0;
        status.sbpStatus = SBPStatus::kNoAdditionalInfo;
        addressManager.ApplyRemoteWrite(
            sessionStatusAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
    }

    static constexpr uint64_t kGuid = 0x0003DB0001DDDDA1ULL;
    static void* Owner() noexcept { return reinterpret_cast<void*>(0xCAFE); }
    static void* OtherOwner() noexcept { return reinterpret_cast<void*>(0xBEEF); }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    FakeSessionScheduler scheduler;
    AddressSpaceManager addressManager{nullptr};
    DeviceRegistry deviceRegistry;
    DeviceManager deviceManager;
    IODispatchQueue queue;
    SessionRegistry registry;
    uint64_t nowNs{0};
    uint64_t sessionStatusAddress{0};
};

TEST(SessionRegistryTests, BuildStandardCommandHelpersUseExpectedOpCodes) {
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

TEST(SessionRegistryTests, SubmitRequestSenseCapturesPayloadAndSenseData) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const auto request = SCSI::BuildRequestSenseRequest(18);
    const size_t pendingBeforeSubmit = rig.bus.PendingWriteCount();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    ASSERT_EQ(pendingBeforeSubmit + 1U, rig.bus.PendingWriteCount());

    const auto& write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t commandOrbAddress = DecodeAddressFromWritePayload(write.data);
    const uint64_t dataBufferAddress = ReadDataBufferAddress(rig.addressManager, commandOrbAddress);
    // Command data is carried by OHCI physical DMA: the descriptor names a
    // sub-4 GB bus address, not a slot in the software-served 0xFFFF window.
    EXPECT_LT(dataBufferAddress, 1ULL << 32);

    const std::vector<uint8_t> sensePayload{
        0x70, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x3A, 0x01
    };
    rig.addressManager.ApplyRemoteWrite(dataBufferAddress, sensePayload);

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    status.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((commandOrbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(commandOrbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    auto result = rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(0, result->transportStatus);
    EXPECT_EQ(SBPStatus::kNoAdditionalInfo, result->sbpStatus);
    EXPECT_EQ(sensePayload, result->payload);
    EXPECT_EQ(sensePayload, result->senseData);
}

TEST(SessionRegistryTests, CheckConditionSurfacesSCSIStatusAndAutosense) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const auto request = SCSI::BuildTestUnitReadyRequest();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    const auto& write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t orbAddress = DecodeAddressFromWritePayload(write.data);

    // Status block with command-set-dependent bytes (SBP-2 Annex B): SAM status
    // CHECK CONDITION + packed autosense 5/26/00 (INVALID FIELD IN PARAM LIST).
    StatusBlock header{};
    header.details = 0;
    header.sbpStatus = SBPStatus::kNoAdditionalInfo;
    header.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((orbAddress >> 32) & 0xFFFFu));
    header.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(orbAddress & 0xFFFF'FFFFu));
    uint8_t blockBytes[32] = {};
    std::memcpy(blockBytes, &header, 8);
    blockBytes[8] = 0x02;   // sfmt=0, SAM status = CHECK CONDITION
    blockBytes[9] = 0x05;   // sense key ILLEGAL REQUEST
    blockBytes[10] = 0x26;  // ASC
    blockBytes[11] = 0x00;  // ASCQ
    rig.addressManager.ApplyRemoteWrite(rig.sessionStatusAddress,
                                        std::span<const uint8_t>{blockBytes, sizeof(blockBytes)});

    auto result = rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(0, result->transportStatus);
    EXPECT_EQ(SBPStatus::kNoAdditionalInfo, result->sbpStatus);  // SBP-status alone lies
    EXPECT_TRUE(result->scsiStatusValid);
    EXPECT_EQ(0x02, result->scsiStatus);
    ASSERT_GE(result->senseData.size(), 14u);
    EXPECT_EQ(0x70, result->senseData[0]);
    EXPECT_EQ(0x05, result->senseData[2] & 0x0F);   // sense key
    EXPECT_EQ(0x26, result->senseData[12]);         // ASC
    EXPECT_EQ(0x00, result->senseData[13]);         // ASCQ
}

// Locks the SBP-2 -> fixed-format sense bit transform against a byte1 with the
// valid bit and a flag set, which the CheckCondition test (byte1=0x05) cannot
// reach. Guards the Linux sbp2.c:1303-1305 mapping: valid -> sense[0] bit7,
// mark/eom/ili shifted up into sense[2] [7:5], key kept in [3:0].
TEST(SessionRegistryTests, SenseDecodePropagatesValidAndFlagBits) {
    // byte1 = 0xA5: valid(0x80) + eom(0x20) + key MEDIUM ERROR(0x05).
    const std::array<uint8_t, 14> statusData{
        0x02,        // sfmt=0 (current), SAM status CHECK CONDITION
        0xA5,        // valid + eom + key 5
        0x26, 0x00,  // ASC, ASCQ
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto sense = SCSI::ConvertSBP2StatusToSenseData(statusData);
    ASSERT_GE(sense.size(), 14u);
    EXPECT_EQ(0xF0, sense[0]);         // 0x70 | valid bit
    EXPECT_EQ(0x45, sense[2]);         // eom -> bit6, key 5 in [3:0]
    EXPECT_EQ(0x05, sense[2] & 0x0F);  // sense key preserved
    EXPECT_EQ(0x26, sense[12]);        // ASC
    EXPECT_EQ(0x00, sense[13]);        // ASCQ

    const std::array<uint8_t, 14> deferredVendor{0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(SCSI::ConvertSBP2StatusToSenseData(deferredVendor).empty());  // sfmt==2 undecodable
}

TEST(SessionRegistryTests, InquiryFailureResultPreservesSBPStatus) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    ASSERT_TRUE(rig.registry.SubmitInquiry(SessionRegistryRig::Owner(), handle, 36));
    const auto& write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t commandOrbAddress = DecodeAddressFromWritePayload(write.data);

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kRequestAborted;
    status.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((commandOrbAddress >> 32) & 0xFFFFu));
    status.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(commandOrbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    auto result = rig.registry.GetInquiryResult(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(0, result->transportStatus);
    EXPECT_EQ(SBPStatus::kRequestAborted, result->sbpStatus);
    EXPECT_TRUE(result->payload.empty());
    EXPECT_FALSE(rig.registry.GetInquiryResult(SessionRegistryRig::Owner(), handle).has_value());
}

TEST(SessionRegistryTests, ActiveCommandFailsOnceAfterFetchAgentRetryExhaustion) {
    SessionRegistryRig rig;
    int busResets = 0;
    rig.registry.SetBusResetRequester([&]() { ++busResets; });
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    session->SetFetchAgentWriteRetriesForTesting(0);
    const auto request = SCSI::BuildTestUnitReadyRequest();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    const auto fetchAgentWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8u, fetchAgentWrite.data.size());
    const uint64_t commandOrbAddress = DecodeAddressFromWritePayload(fetchAgentWrite.data);

    ASSERT_TRUE(rig.bus.CompleteWrite(fetchAgentWrite.handle, ASFW::Async::AsyncStatus::kTimeout));
    rig.AdvanceMs(1000);

    const auto commandResult = rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(commandResult.has_value());
    EXPECT_EQ(-1, commandResult->transportStatus);
    EXPECT_EQ(SBPStatus::kUnspecifiedError, commandResult->sbpStatus);

    const auto stateAfterFailure = rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(stateAfterFailure.has_value());
    EXPECT_EQ(-1, stateAfterFailure->lastError);

    ASSERT_GT(rig.bus.WriteCount(), 1u);
    const auto agentResetWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    EXPECT_EQ(4u, agentResetWrite.data.size());
    // Apple-style recovery: the transport failure also submitted a
    // LOGICAL_UNIT_RESET management ORB (8-byte agent write) before the
    // fetch-agent reset quadlet.
    const auto lunResetWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 2);
    EXPECT_EQ(8u, lunResetWrite.data.size());

    ASSERT_TRUE(rig.bus.CompleteWrite(agentResetWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    // The LUN-reset write fails: nothing confirmed the target stopped, so the
    // command's ORB and buffer stay mapped and a bus reset is requested. The
    // reset is what ends the target's claim on them.
    ASSERT_TRUE(rig.bus.CompleteWrite(lunResetWrite.handle, ASFW::Async::AsyncStatus::kTimeout));
    EXPECT_EQ(1, busResets);
    uint32_t ignored = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(commandOrbAddress, &ignored));
    EXPECT_FALSE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));

    rig.registry.OnBusReset(2);
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(commandOrbAddress, &ignored));
}

TEST(SessionRegistryTests, RejectsSessionOperationsFromNonOwningClient) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();

    EXPECT_FALSE(rig.registry.StartLogin(SessionRegistryRig::OtherOwner(), handle));
    EXPECT_EQ(0u, rig.bus.PendingWriteCount());

    rig.LoginSuccessfully(handle);

    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::OtherOwner(), handle).has_value());
    EXPECT_FALSE(rig.registry.SubmitCommand(SessionRegistryRig::OtherOwner(), handle,
                                            SCSI::BuildTestUnitReadyRequest()));
    EXPECT_FALSE(rig.registry.SubmitInquiry(SessionRegistryRig::OtherOwner(), handle, 36));
    EXPECT_FALSE(rig.registry.SubmitTaskManagement(
        SessionRegistryRig::OtherOwner(), handle, SBP2ManagementORB::Function::LogicalUnitReset));
    EXPECT_FALSE(rig.registry.ReleaseSession(SessionRegistryRig::OtherOwner(), handle));

    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
                                           SCSI::BuildTestUnitReadyRequest()));
    const auto& commandWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t commandOrbAddress = DecodeAddressFromWritePayload(commandWrite.data);
    StatusBlock commandStatus{};
    commandStatus.details = 0;
    commandStatus.sbpStatus = SBPStatus::kNoAdditionalInfo;
    commandStatus.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((commandOrbAddress >> 32) & 0xFFFFu));
    commandStatus.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(commandOrbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&commandStatus), sizeof(commandStatus)});

    EXPECT_FALSE(rig.registry.GetCommandResult(SessionRegistryRig::OtherOwner(), handle).has_value());
    EXPECT_TRUE(rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle).has_value());

    // Drain the first command's pending ORB_POINTER write so the fetch agent
    // is free before the next submit.
    for (int i = 0; i < 8 && rig.bus.PendingWriteCount() > 0; ++i) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    ASSERT_TRUE(rig.registry.SubmitInquiry(SessionRegistryRig::Owner(), handle, 36));
    // Immediate model: the 2nd command is announced with its own ORB_POINTER
    // write — its address is in the last bus write's payload.
    const uint64_t inquiryOrbAddress =
        DecodeAddressFromWritePayload(rig.bus.WriteAt(rig.bus.WriteCount() - 1).data);
    StatusBlock inquiryStatus{};
    inquiryStatus.details = 0;
    inquiryStatus.sbpStatus = SBPStatus::kRequestAborted;
    inquiryStatus.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((inquiryOrbAddress >> 32) & 0xFFFFu));
    inquiryStatus.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(inquiryOrbAddress & 0xFFFF'FFFFu));
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&inquiryStatus), sizeof(inquiryStatus)});

    EXPECT_FALSE(rig.registry.GetInquiryResult(SessionRegistryRig::OtherOwner(), handle).has_value());
    EXPECT_TRUE(rig.registry.GetInquiryResult(SessionRegistryRig::Owner(), handle).has_value());
    EXPECT_TRUE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());
}

TEST(SessionRegistryTests, SubmitTaskManagementWritesLogicalUnitResetORB) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const size_t writesBeforeSubmit = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitTaskManagement(
        SessionRegistryRig::Owner(), handle, SBP2ManagementORB::Function::LogicalUnitReset));

    ASSERT_EQ(writesBeforeSubmit + 1U, rig.bus.WriteCount());
    const auto& write = rig.bus.WriteAt(writesBeforeSubmit);
    const uint64_t managementOrbAddress = DecodeAddressFromWritePayload(write.data);

    EXPECT_EQ(0x0Eu, ReadTaskManagementFunction(rig.addressManager, managementOrbAddress));
    EXPECT_EQ(0x0042u, ReadTaskManagementLoginID(rig.addressManager, managementOrbAddress));
}

TEST(SessionRegistryTests, TaskManagementSuccessClearsActiveCommandTracking) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const auto request = SCSI::BuildTestUnitReadyRequest();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    ASSERT_FALSE(rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle).has_value());
    ASSERT_GT(rig.bus.WriteCount(), 0U);
    const auto fetchAgentWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8U, fetchAgentWrite.data.size());

    const size_t writesBeforeTaskManagement = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitTaskManagement(
        SessionRegistryRig::Owner(), handle, SBP2ManagementORB::Function::AbortTaskSet));
    const auto& taskWrite = rig.bus.WriteAt(writesBeforeTaskManagement);
    const uint64_t taskOrbAddress = DecodeAddressFromWritePayload(taskWrite.data);
    const uint64_t taskStatusAddress =
        ReadTaskManagementStatusAddress(rig.addressManager, taskOrbAddress);

    ASSERT_TRUE(rig.bus.CompleteWrite(taskWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    CompleteTaskManagementStatus(rig.addressManager, taskStatusAddress, taskOrbAddress);

    EXPECT_FALSE(rig.registry.GetCommandResult(SessionRegistryRig::Owner(), handle).has_value());
    EXPECT_FALSE(rig.bus.CompleteWrite(fetchAgentWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
}

TEST(SessionRegistryTests, SubmitTaskManagementRejectsInvalidFunction) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    EXPECT_FALSE(rig.registry.SubmitTaskManagement(
        SessionRegistryRig::Owner(), handle, SBP2ManagementORB::Function::QueryLogins));
}

TEST(SessionRegistryTests, MissingDiscoverySuspendsDeviceAndReconnectWaitsForResume) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    rig.registry.OnBusReset(2);
    const auto suspendedState = rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle);
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
        ReadORBAddress(rig.addressManager, reconnectOrbAddress,
                       offsetof(ReconnectORB, statusFIFOAddressHi),
                       offsetof(ReconnectORB, statusFIFOAddressLo));
    ASSERT_TRUE(rig.bus.CompleteWrite(reconnectWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    StatusBlock reconnectStatus{};
    reconnectStatus.details = 0;
    reconnectStatus.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        reconnectStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&reconnectStatus), sizeof(reconnectStatus)});

    ASSERT_GE(rig.bus.WriteCount(), writesBeforeMissingRefresh + 2U);
    const auto& busyTimeoutWrite = rig.bus.WriteAt(writesBeforeMissingRefresh + 1U);
    EXPECT_EQ(0x33u, busyTimeoutWrite.nodeId.value);
    EXPECT_EQ(0x33u, busyTimeoutWrite.address.nodeID);

    const size_t writesBeforeCommand = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
                                           SCSI::BuildTestUnitReadyRequest()));
    ASSERT_EQ(writesBeforeCommand + 1U, rig.bus.WriteCount());
    const auto& fetchAgentWrite = rig.bus.WriteAt(writesBeforeCommand);
    EXPECT_EQ(0x33u, fetchAgentWrite.nodeId.value);
    EXPECT_EQ(0x33u, fetchAgentWrite.address.nodeID);
}

TEST(SessionRegistryTests, RepeatedMissingDiscoveryTerminatesSuspendedSBP2Device) {
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

TEST(SessionRegistryTests, ReleaseOwnerRetainsSessionUntilLogoutStatusArrives) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());

    EXPECT_FALSE(weakSession.expired());
    ASSERT_EQ(writesBeforeRelease + 1U, rig.bus.WriteCount());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());

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
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());
}

TEST(SessionRegistryTests, ReleaseOwnerRetainsSessionUntilLogoutTimeout) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());

    EXPECT_FALSE(weakSession.expired());
    ASSERT_EQ(writesBeforeRelease + 1U, rig.bus.WriteCount());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    EXPECT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_FALSE(weakSession.expired());

    rig.AdvanceMs(2'000);

    EXPECT_TRUE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());
}

TEST(SessionRegistryTests, ReleaseOwnerDuringPendingLoginCancelsWriteAndDropsSession) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();

    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    ASSERT_TRUE(rig.registry.StartLogin(SessionRegistryRig::Owner(), handle));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());
    const auto loginWrite = rig.bus.WriteAt(0);

    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());

    EXPECT_TRUE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());
    EXPECT_EQ(0u, rig.bus.PendingWriteCount());
    EXPECT_FALSE(rig.bus.CompleteWrite(loginWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
}

TEST(SessionRegistryTests, ReleaseSessionDuringPendingLogoutRetainsUntilStatusArrives) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    ASSERT_FALSE(weakSession.expired());

    const size_t writesBeforeLogout = rig.bus.WriteCount();
    const size_t pendingBeforeLogout = rig.bus.PendingWriteCount();
    ASSERT_TRUE(session->Logout());
    ASSERT_EQ(writesBeforeLogout + 1U, rig.bus.WriteCount());
    ASSERT_EQ(pendingBeforeLogout + 1U, rig.bus.PendingWriteCount());

    EXPECT_TRUE(rig.registry.ReleaseSession(SessionRegistryRig::Owner(), handle));
    EXPECT_FALSE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeLogout);
    EXPECT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_FALSE(weakSession.expired());

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    EXPECT_TRUE(weakSession.expired());
    EXPECT_FALSE(rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle).has_value());
}

TEST(SessionRegistryTests, CreateSessionRejectsDuplicateTargetAcrossOwners) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    ASSERT_NE(0u, handle);

    auto duplicate = rig.registry.CreateSession(SessionRegistryRig::OtherOwner(),
                                                SessionRegistryRig::kGuid, 0);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(kIOReturnExclusiveAccess, duplicate.error());
}

TEST(SessionRegistryTests, CreateSessionRejectsDuplicateTargetUntilLogoutCompletes) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());

    auto duplicateWhileLoggingOut = rig.registry.CreateSession(SessionRegistryRig::OtherOwner(),
                                                               SessionRegistryRig::kGuid, 0);
    ASSERT_FALSE(duplicateWhileLoggingOut.has_value());
    EXPECT_EQ(kIOReturnExclusiveAccess, duplicateWhileLoggingOut.error());

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    ASSERT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});

    auto replacement = rig.registry.CreateSession(SessionRegistryRig::OtherOwner(),
                                                  SessionRegistryRig::kGuid, 0);
    ASSERT_TRUE(replacement.has_value());
}

TEST(SessionRegistryTests, MissingDiscoveryStillTerminatesNonSBP2Device) {
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

TEST(SessionRegistryTests, BusResetInvalidatesDeviceMappingsBeforeDiscoveryRebinds) {
    SessionRegistryRig rig;
    const auto original = rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid);
    ASSERT_NE(nullptr, original);
    ASSERT_TRUE(original->IsReady());
    ASSERT_NE(nullptr, rig.deviceManager.GetDeviceByNode(Generation{1}, 0x32));

    rig.deviceManager.SuspendAllForBusReset();

    EXPECT_TRUE(original->IsSuspended());
    EXPECT_EQ(kInvalidNodeId, original->GetNodeID());
    EXPECT_TRUE(rig.deviceManager.GetReadyDevices().empty());
    EXPECT_EQ(nullptr, rig.deviceManager.GetDeviceByNode(Generation{1}, 0x32));

    rig.UpsertDevice(Generation{2}, 0x21);
    const auto rebound = rig.deviceManager.GetDeviceByGUID(SessionRegistryRig::kGuid);
    ASSERT_NE(nullptr, rebound);
    EXPECT_EQ(original.get(), rebound.get());
    EXPECT_TRUE(rebound->IsReady());
    EXPECT_EQ(Generation{2}, rebound->GetGeneration());
    EXPECT_EQ(0x21, rebound->GetNodeID());
    EXPECT_EQ(rebound, rig.deviceManager.GetDeviceByNode(Generation{2}, 0x21));
}

TEST(SessionRegistryTests, BusResetInvalidatesRegistryMappingUntilNewRom) {
    DeviceRegistry registry;
    ConfigROM rom{};
    rom.bib.guid = SessionRegistryRig::kGuid;
    rom.gen = Generation{1};
    rom.nodeId = 0x32;

    const auto record = registry.UpsertFromROM(rom, LinkPolicy{});
    const auto byNode = registry.SnapshotByNode(Generation{1}, 0x32);
    ASSERT_TRUE(byNode.has_value());
    EXPECT_EQ(record.guid, byNode->guid);
    ASSERT_EQ(1u, registry.LiveDevices(Generation{1}).size());

    registry.InvalidateLiveMappingsForBusReset();

    EXPECT_FALSE(registry.SnapshotByNode(Generation{1}, 0x32).has_value());
    const auto byGuid = registry.SnapshotByGuid(SessionRegistryRig::kGuid);
    ASSERT_TRUE(byGuid.has_value());
    EXPECT_EQ(kInvalidNodeId, byGuid->nodeId);
    EXPECT_TRUE(registry.LiveDevices(Generation{1}).empty());
}

TEST(SessionRegistryTests, RouteTokensRejectResetAndSameGuidReplugCallbacks) {
    DeviceRegistry registry;
    ConfigROM rom{};
    rom.bib.guid = SessionRegistryRig::kGuid;
    rom.gen = Generation{1};
    rom.nodeId = 0x32;

    const auto firstRecord = registry.UpsertFromROM(rom, LinkPolicy{});
    const auto firstRoute = registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(firstRoute.has_value());
    EXPECT_TRUE(registry.IsCurrent(*firstRoute));

    registry.InvalidateLiveMappingsForBusReset();
    EXPECT_FALSE(registry.IsCurrent(*firstRoute));

    rom.gen = Generation{2};
    rom.nodeId = 0x21;
    const auto reboundRecord = registry.UpsertFromROM(rom, LinkPolicy{});
    const auto reboundRoute = registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(reboundRoute.has_value());
    EXPECT_TRUE(registry.IsCurrent(*reboundRoute));
    EXPECT_EQ(firstRecord.deviceIncarnation, reboundRecord.deviceIncarnation);
    EXPECT_NE(firstRoute->routeEpoch, reboundRoute->routeEpoch);

    registry.RetireDevice(rom.bib.guid);
    EXPECT_FALSE(registry.IsCurrent(*reboundRoute));

    rom.gen = Generation{3};
    rom.nodeId = 0x12;
    const auto replugRecord = registry.UpsertFromROM(rom, LinkPolicy{});
    const auto replugRoute = registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(replugRoute.has_value());
    EXPECT_TRUE(registry.IsCurrent(*replugRoute));
    EXPECT_GT(replugRecord.deviceIncarnation, reboundRecord.deviceIncarnation);
    EXPECT_NE(replugRoute->routeEpoch, reboundRoute->routeEpoch);
}

TEST(SessionRegistryTests, SessionAdmissionRejectsAnInvalidatedRoute) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();

    rig.deviceRegistry.InvalidateLiveMappingsForBusReset();
    EXPECT_FALSE(rig.registry.StartLogin(SessionRegistryRig::Owner(), handle));
}

TEST(SessionRegistryTests, SubmitCommandRejectsCDBLargerThanORBPayloadBudget) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    // maxCommandBlockSize is maxORBSize(32) - NormalORB::kHeaderSize(20) = 12, so a
    // 13-byte CDB exceeds the budget and must be rejected.
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(13, 0x12);
    request.direction = SCSI::DataDirection::None;
    request.transferLength = 0;
    request.timeoutMs = 100;

    EXPECT_FALSE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
}

TEST(SessionRegistryTests, CreateSessionAcceptsRealSBP2SpecAndVersion) {
    SessionRegistryRig rig;
    auto result = rig.registry.CreateSession(SessionRegistryRig::Owner(),
                                             SessionRegistryRig::kGuid, 0);
    ASSERT_TRUE(result.has_value());
}

TEST(SessionRegistryTests, UnitCharacteristicsDecodeMinimumORBSize) {
    SessionRegistryRig rig(0x000408);

    const uint64_t handle = rig.CreateSession();
    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    const auto& targetInfo = session->TargetInfo();
    EXPECT_EQ(2000u, targetInfo.managementTimeoutMs);
    EXPECT_EQ(32u, targetInfo.maxORBSize);
    // NormalORB header is 20 bytes (next 8 + data_descriptor 8 + misc quadlet 4);
    // command block starts at offset 20 (Linux sbp2.c struct sbp2_command_orb).
    EXPECT_EQ(32u - NormalORB::kHeaderSize, targetInfo.maxCommandBlockSize);
}

TEST(SessionRegistryTests, UnitCharacteristicsDecodeTimeoutAndORBSizeFromLowBytes) {
    SessionRegistryRig rig(0x000410);

    const uint64_t handle = rig.CreateSession();
    auto* session = rig.registry.GetSessionForTesting(handle);
    ASSERT_NE(nullptr, session);

    const auto& targetInfo = session->TargetInfo();
    EXPECT_EQ(2000u, targetInfo.managementTimeoutMs);
    EXPECT_EQ(64u, targetInfo.maxORBSize);
    EXPECT_EQ(64u - NormalORB::kHeaderSize, targetInfo.maxCommandBlockSize);
}

TEST(SessionRegistryTests, DeviceDiscoveryParsesNikonStyleManagementAgentCSRKey) {
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
        OSSwapHostToBigInt32(0x04045343u),
        OSSwapHostToBigInt32(0x31333934u),
        OSSwapHostToBigInt32(0x00FF5012u),
        OSSwapHostToBigInt32(0x0090B540u),
        OSSwapHostToBigInt32(0x01FFFFFFu),
        OSSwapHostToBigInt32(0x0001B344u),
        OSSwapHostToBigInt32(0x0004CAEEu),
        OSSwapHostToBigInt32(0x1200609Eu),
        OSSwapHostToBigInt32(0x13010483u),
        OSSwapHostToBigInt32(0x5400C000u),
        OSSwapHostToBigInt32(0x14060000u),
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

// Reproduces the v45 HW sequence: push-mode command → ORB timeout → LUN-reset
// management ORB → mgmt timeout (target never posts mgmt status). The task must
// still be delivered to the initiator (otherwise the SCSI task leaks and the
// host hangs), and the failed LUN reset must escalate to a bus reset.
TEST(SessionRegistryTests, OrbTimeoutThenLunResetTimeoutEscalatesAndDeliversResult) {
    SessionRegistryRig rig;
    int busResets = 0;
    rig.registry.SetBusResetRequester([&busResets]() { ++busResets; });

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    // Login leaves the BUSY_TIMEOUT CSR write pending — flush it so later
    // FIFO completions hit the writes this test issues.
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }

    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>{0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 36;
    request.timeoutMs = 1000;

    int callbacks = 0;
    SCSI::CommandResult lastResult{};
    ASSERT_TRUE(rig.registry.SubmitCommand(
        SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult& result) {
            ++callbacks;
            lastResult = result;
        }));

    // ORB_POINTER write ack'd (arms the ORB timeout); the target fetches the
    // ORB but never posts status.
    const auto orbPointerWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8u, orbPointerWrite.data.size());
    ASSERT_TRUE(rig.bus.CompleteWrite(orbPointerWrite.handle,
                                      ASFW::Async::AsyncStatus::kSuccess));

    rig.AdvanceMs(1001);  // ORB timeout → AGENT_RESET (no-wait) + LUN-reset mgmt write
    EXPECT_EQ(0, callbacks);  // delivery deferred until the LUN reset resolves
    EXPECT_EQ(2u, rig.bus.PendingWriteCount());  // AGENT_RESET + mgmt-agent write
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    EXPECT_EQ(0, callbacks);

    // Mgmt status never arrives → mgmt timeout (2000 ms advertised + grace).
    rig.AdvanceMs(3001);

    EXPECT_EQ(1, callbacks);  // task delivered despite the failed LUN reset
    EXPECT_NE(0, lastResult.transportStatus);
    EXPECT_EQ(1, busResets);  // failed LUN reset escalates to a bus reset
}

// Hasselblad H-camera HW trace (2026-10-15): the target answers the timeout's
// LUN reset with the dead bit set, and a dead fetch agent ignores ORB_POINTER.
// Without an AGENT_RESET after the LUN reset every later command times out.
// Apple resets the fetch agent from the LUN-reset completion and only then
// completes the task (IOFireWireSerialBusProtocolTransport.cpp LunResetComplete).
class LunResetAgentResetTest : public ::testing::TestWithParam<bool> {};

TEST_P(LunResetAgentResetTest, OrbTimeoutResetsFetchAgentAfterLunResetBeforeDelivering) {
    SessionRegistryRig rig;
    int busResets = 0;
    rig.registry.SetBusResetRequester([&busResets]() { ++busResets; });

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }

    auto request = SCSI::BuildInquiryRequest(36);
    request.timeoutMs = 1000;
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(
        SessionRegistryRig::Owner(), handle, request,
        [&callbacks](const SCSI::CommandResult&) { ++callbacks; }));

    const auto orbPointerWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_TRUE(rig.bus.CompleteWrite(orbPointerWrite.handle,
                                      ASFW::Async::AsyncStatus::kSuccess));
    const uint64_t bufferAddress = ReadDataBufferAddress(
        rig.addressManager, DecodeAddressFromWritePayload(orbPointerWrite.data));
    uint32_t retained = 0;

    rig.AdvanceMs(1001);  // ORB timeout → AGENT_RESET (no-wait) + LUN-reset mgmt write
    ASSERT_EQ(2u, rig.bus.PendingWriteCount());
    // The target may still be writing the buffer: it stays mapped and the
    // session takes no new command until the LUN reset says it stopped.
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(bufferAddress, &retained));
    EXPECT_FALSE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    auto mgmtWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    if (mgmtWrite.data.size() != 8u) {
        mgmtWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 2);
    }
    ASSERT_EQ(8u, mgmtWrite.data.size());
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }

    const uint64_t mgmtOrbAddress = DecodeAddressFromWritePayload(mgmtWrite.data);
    const size_t writesBeforeStatus = rig.bus.WriteCount();
    constexpr uint8_t kSrc1RespCompleteDeadLen1 = 0x49;  // as posted by the camera
    CompleteTaskManagementStatus(rig.addressManager,
                                 ReadTaskManagementStatusAddress(rig.addressManager, mgmtOrbAddress),
                                 mgmtOrbAddress, SBPStatus::kNoAdditionalInfo,
                                 kSrc1RespCompleteDeadLen1);
    rig.AdvanceMs(0);

    ASSERT_EQ(writesBeforeStatus + 1u, rig.bus.WriteCount())
        << "LUN reset must be followed by an AGENT_RESET";
    const auto agentResetWrite = rig.bus.WriteAt(writesBeforeStatus);
    EXPECT_EQ(4u, agentResetWrite.data.size());
    EXPECT_EQ(0x0020'0004u, agentResetWrite.address.addressLo);
    EXPECT_EQ(0, callbacks) << "task completes only once the fetch agent is reset";
    // LUN-reset status: the task set is gone, the buffer is ours (§10.4.3).
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(bufferAddress, &retained));

    if (GetParam()) {
        // A bus reset while the AGENT_RESET is in flight flushes the result;
        // the late write completion must not deliver it a second time.
        rig.registry.OnBusReset(2);
        EXPECT_EQ(1, callbacks);
    }
    ASSERT_TRUE(rig.bus.CompleteWrite(agentResetWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    rig.AdvanceMs(0);
    EXPECT_EQ(1, callbacks);
    EXPECT_EQ(0, busResets);
}

INSTANTIATE_TEST_SUITE_P(BusResetDuringAgentReset, LunResetAgentResetTest, ::testing::Bool());

// FW-review H1: a command result deferred behind the timeout→LUN-reset ORB
// clears commandInFlight_/commandORB_ before deferring delivery to that ORB's
// onComplete. If a bus reset (or Stop/unplug) drops the management ORB in that
// window, its onComplete never runs — the executor must still complete the SCSI
// task exactly once, or SBP2TargetBridge's command pump wedges and the task
// leaks (VueScan hangs until the device is power-cycled).
TEST(SessionRegistryTests, DeferredResultFlushedWhenBusResetDropsLunResetORB) {
    SessionRegistryRig rig;
    rig.registry.SetBusResetRequester([]() {});

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }

    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>{0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 36;
    request.timeoutMs = 1000;

    int callbacks = 0;
    SCSI::CommandResult lastResult{};
    ASSERT_TRUE(rig.registry.SubmitCommand(
        SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult& result) {
            ++callbacks;
            lastResult = result;
        }));

    const auto orbPointerWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_TRUE(rig.bus.CompleteWrite(orbPointerWrite.handle,
                                      ASFW::Async::AsyncStatus::kSuccess));

    rig.AdvanceMs(1001);  // ORB timeout → result deferred behind the LUN-reset ORB
    ASSERT_EQ(0, callbacks);

    // A bus reset lands while the LUN-reset management ORB is still in flight;
    // it is dropped (onComplete never fires). The deferred result must still be
    // flushed, not stranded.
    rig.registry.OnBusReset(2);
    EXPECT_EQ(1, callbacks) << "deferred result must be flushed on bus reset";
    EXPECT_NE(0, lastResult.transportStatus);

    // The dropped management path must not deliver a second time.
    rig.AdvanceMs(4000);
    EXPECT_EQ(1, callbacks);
}

// FW-review B1: when the fetch-agent write fails synchronously (AT ring-full /
// label-pool exhaustion / bus-reset generation edge), FetchAgent::AppendImmediate
// fires the ORB completion inline. That completion nulls commandORB_ and defers
// delivery behind a LUN-reset ORB while leaving resultCallback_ armed, yet
// SubmitORB returns false. SubmitCommand must report the task ACCEPTED (true) so
// the bridge does NOT fire its own fallback completion — a false return here is
// what double-completes the SCSI task in production (double ParallelTaskCompletion
// + OSAction/controller over-release).
TEST(SessionRegistryTests, SynchronousWriteFailureDoesNotDoubleCompleteTask) {
    SessionRegistryRig rig;
    rig.registry.SetBusResetRequester([]() {});

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }

    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>{0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 36;
    request.timeoutMs = 1000;

    int callbacks = 0;
    rig.bus.FailNextWriteBlock();  // the ORB_POINTER write fails synchronously
    const bool accepted = rig.registry.SubmitCommand(
        SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult&) { ++callbacks; });

    // The completion ran inline (FailORB) and deferred delivery, so the executor
    // owns the task — SubmitCommand must report acceptance, not rejection.
    EXPECT_TRUE(accepted);

    // Model SBP2TargetBridge::Pump's contract: it fires its own fallback
    // completion iff SubmitCommand returned false. A false return is precisely
    // what produces the second completion in production.
    if (!accepted) {
        ++callbacks;
    }

    // Drive the deferred LUN-reset to completion; the result is delivered once.
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    rig.AdvanceMs(4000);
    EXPECT_EQ(1, callbacks) << "SCSI task must complete exactly once";
}

// --- WI-1: login-state push channel (SetLoginStateObserver) -----------------

TEST(SessionRegistryTests, LoginStateObserverFiresUpOnSuccessfulLogin) {
    SessionRegistryRig rig;
    std::vector<std::pair<uint64_t, bool>> events;
    rig.registry.SetLoginStateObserver(
        [&events](uint64_t guid, bool loggedIn) { events.push_back({guid, loggedIn}); });

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    rig.AdvanceMs(0);  // drain the dispatched observer call

    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(SessionRegistryRig::kGuid, events[0].first);
    EXPECT_TRUE(events[0].second);
}

TEST(SessionRegistryTests, LoginStateObserverFiresDownOnLogout) {
    SessionRegistryRig rig;
    std::vector<std::pair<uint64_t, bool>> events;
    rig.registry.SetLoginStateObserver(
        [&events](uint64_t guid, bool loggedIn) { events.push_back({guid, loggedIn}); });

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    rig.AdvanceMs(0);
    events.clear();  // drop the login-up event

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());
    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    ASSERT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    StatusBlock status{};
    status.details = 0;
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
    rig.AdvanceMs(0);  // drain the dispatched observer call

    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(SessionRegistryRig::kGuid, events[0].first);
    EXPECT_FALSE(events[0].second);
}

TEST(SessionRegistryTests, LoginStateObserverStaysSilentOnBusResetSuspend) {
    SessionRegistryRig rig;
    std::vector<std::pair<uint64_t, bool>> events;
    rig.registry.SetLoginStateObserver(
        [&events](uint64_t guid, bool loggedIn) { events.push_back({guid, loggedIn}); });

    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    rig.AdvanceMs(0);
    events.clear();  // drop the login-up event

    // Bus reset suspends the session (LoggedIn -> Suspended). A hot-plug target
    // must NOT be torn down here (reconnect restores it), so no down is emitted.
    rig.registry.OnBusReset(2);
    const auto suspended = rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(suspended.has_value());
    ASSERT_EQ(ASFW::Protocols::SBP2::LoginState::Suspended, suspended->loginState);
    rig.AdvanceMs(0);

    EXPECT_TRUE(events.empty());
}

// A reset during login invalidates the node ID the login was addressed to.
// Apple and Linux both re-login after the device is re-enumerated
// (IOFireWireSBP2Login::resumeNotify, sbp2_update); so does RefreshTargets.
TEST(SessionRegistryTests, BusResetDuringLoginLogsInAgainAfterRediscovery) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    ASSERT_TRUE(rig.registry.StartLogin(SessionRegistryRig::Owner(), handle));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());

    rig.registry.OnBusReset(2);
    rig.deviceManager.SuspendAllForBusReset();
    rig.AdvanceMs(1000);
    EXPECT_EQ(1u, rig.bus.WriteCount());
    auto state = rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(ASFW::Protocols::SBP2::LoginState::Suspended, state->loginState);

    rig.bus.SetGeneration(ASFW::FW::Generation{2});
    rig.UpsertDevice(Generation{2}, 0x33);
    rig.registry.RefreshTargets(Generation{2});

    ASSERT_EQ(2u, rig.bus.WriteCount());
    EXPECT_EQ(0x33u, rig.bus.WriteAt(1).nodeId.value);
    state = rig.registry.GetSessionState(SessionRegistryRig::Owner(), handle);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(ASFW::Protocols::SBP2::LoginState::LoggingIn, state->loginState);
}

class ReleaseParksCommandTest : public ::testing::TestWithParam<bool> {};

// Quitting a client mid-transfer completes the task at once, but the target
// still owns the buffer until its logout status (§8.4 aborts the task set).
TEST_P(ReleaseParksCommandTest, ReleaseKeepsCommandBufferUntilLogoutStatus) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
        SCSI::BuildInquiryRequest(36), [&](const SCSI::CommandResult&) { ++callbacks; }));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t dataAddress =
        ReadDataBufferAddress(rig.addressManager, DecodeAddressFromWritePayload(write.data));

    const size_t writesBeforeRelease = rig.bus.WriteCount();
    if (GetParam()) {
        rig.registry.ReleaseOwner(SessionRegistryRig::Owner());
    } else {
        ASSERT_TRUE(rig.registry.ReleaseSession(SessionRegistryRig::Owner(), handle));
    }
    // The task completes at once (our buffer is parked, not the caller's); the
    // buffer itself waits for the target to let go.
    EXPECT_EQ(1, callbacks);
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(dataAddress, &value));

    const auto& logoutWrite = rig.bus.WriteAt(writesBeforeRelease);
    ASSERT_TRUE(rig.bus.CompleteWrite(logoutWrite.handle, ASFW::Async::AsyncStatus::kSuccess));
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
    StatusBlock status{};
    status.sbpStatus = SBPStatus::kNoAdditionalInfo;
    rig.addressManager.ApplyRemoteWrite(
        rig.sessionStatusAddress,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
    EXPECT_EQ(1, callbacks);
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
}

INSTANTIATE_TEST_SUITE_P(SessionAndOwner, ReleaseParksCommandTest, ::testing::Bool());

TEST(SessionRegistryTests, BusResetFreesParkedCommand) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
        SCSI::BuildInquiryRequest(36), [&](const SCSI::CommandResult&) { ++callbacks; }));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t dataAddress =
        ReadDataBufferAddress(rig.addressManager, DecodeAddressFromWritePayload(write.data));
    ASSERT_TRUE(rig.registry.ReleaseSession(SessionRegistryRig::Owner(), handle));
    EXPECT_EQ(1, callbacks);
    uint32_t value = 0;
    ASSERT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
    rig.registry.OnBusReset(2);
    EXPECT_EQ(1, callbacks);
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
}

// A logout the target never answers leaves its claim on the buffer open;
// only a bus reset ends it (your item 1 in #213: reset only when logout fails
// or times out).
class LogoutFailureTest : public ::testing::TestWithParam<bool> {};

TEST_P(LogoutFailureTest, FailedLogoutKeepsParkedCommandAndRequestsBusReset) {
    SessionRegistryRig rig;
    int resets = 0;
    rig.registry.SetBusResetRequester([&]() { ++resets; });
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
        SCSI::BuildInquiryRequest(36), [&](const SCSI::CommandResult&) { ++callbacks; }));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t dataAddress =
        ReadDataBufferAddress(rig.addressManager, DecodeAddressFromWritePayload(write.data));

    const bool rejected = GetParam();
    if (rejected) {
        rig.bus.FailNextWriteBlock();
    }
    const size_t writesBeforeRelease = rig.bus.WriteCount();
    ASSERT_TRUE(rig.registry.ReleaseSession(SessionRegistryRig::Owner(), handle));
    if (!rejected) {
        ASSERT_TRUE(rig.bus.CompleteWrite(rig.bus.WriteAt(writesBeforeRelease).handle,
                                          ASFW::Async::AsyncStatus::kSuccess));
        rig.AdvanceMs(3'000);  // logout timeout
    }
    EXPECT_EQ(1, resets);
    EXPECT_EQ(1, callbacks);
    EXPECT_FALSE(weakSession.expired());
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(dataAddress, &value));

    rig.registry.OnBusReset(2);
    EXPECT_EQ(1, callbacks);
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
    EXPECT_TRUE(weakSession.expired());
}

INSTANTIATE_TEST_SUITE_P(RejectedAndTimedOut, LogoutFailureTest, ::testing::Bool());

TEST(SessionRegistryTests, TeardownReleaseWithRejectedLogoutRequestsNoBusReset) {
    SessionRegistryRig rig;
    int resets = 0;
    rig.registry.SetBusResetRequester([&]() { ++resets; });
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    std::weak_ptr<LoginSession> weakSession = rig.registry.GetSessionWeakForTesting(handle);
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle,
        SCSI::BuildInquiryRequest(36), [&](const SCSI::CommandResult&) { ++callbacks; }));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t dataAddress =
        ReadDataBufferAddress(rig.addressManager, DecodeAddressFromWritePayload(write.data));

    rig.registry.PrepareForTeardown();
    rig.bus.FailNextWriteBlock();
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());
    // No reset on a bus that is going down; the command stays parked and goes
    // with the registry (the teardown shut the physical filter before this).
    EXPECT_EQ(0, resets);
    EXPECT_EQ(1, callbacks);
    EXPECT_FALSE(weakSession.expired());
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(dataAddress, &value));
}

class PhysicalDmaSizeTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(PhysicalDmaSizeTest, DescribesTheWholeBufferBelow4GB) {
    SessionRegistryRig rig;
    const auto handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x28;
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = GetParam();
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t orbAddress = DecodeAddressFromWritePayload(write.data);
    const uint64_t descriptor = ReadDataBufferAddress(rig.addressManager, orbAddress);
    const uint32_t options = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, orbAddress + offsetof(NormalORB, options)));
    if (GetParam() <= 0xF000) {
        EXPECT_EQ(GetParam(), options & 0xFFFFu);
        EXPECT_LT(descriptor, uint64_t{1} << 32);
    } else {
        const uint32_t entries = options & 0xFFFFu;
        EXPECT_EQ((GetParam() + 0xEFFFu) / 0xF000u, entries);
        uint64_t expectedAddress = 0;
        uint32_t total = 0;
        for (uint32_t index = 0; index < entries; ++index) {
            const uint32_t hi = OSSwapBigToHostInt32(
                ReadQuadlet(rig.addressManager, descriptor + index * 8));
            const uint32_t lo = OSSwapBigToHostInt32(
                ReadQuadlet(rig.addressManager, descriptor + index * 8 + 4));
            const uint64_t address = ComposeAddress(hi & 0xFFFFu, lo);
            const uint32_t length = hi >> 16;
            if (index != 0) EXPECT_EQ(expectedAddress, address);
            EXPECT_GT(length, 0u);
            EXPECT_LE(length, 0xF000u);
            EXPECT_LE(address + length, uint64_t{1} << 32);
            expectedAddress = address + length;
            total += length;
        }
        EXPECT_EQ(GetParam(), total);
    }
    rig.registry.ReleaseOwner(SessionRegistryRig::Owner());
}

INSTANTIATE_TEST_SUITE_P(DescriptorBoundaries, PhysicalDmaSizeTest,
    ::testing::Values(0xF000u, 0xF001u, 1024u * 1024u, 30u * 1024u * 1024u));

// An HBA task names its own DMA-mapped buffer: SBP-2 points the target at it
// and allocates nothing of its own; the result reports the bytes moved there.
TEST(SessionRegistryTests, InPlaceCommandUsesTheCallersBufferAndAllocatesNone) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    constexpr uint64_t kIOVA = 0x8010'0000ULL;
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x28;
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 0x1000;
    request.dataIOVA = kIOVA;
    std::optional<SCSI::CommandResult> delivered;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult& result) { delivered = result; }));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t orbAddress = DecodeAddressFromWritePayload(write.data);
    EXPECT_EQ(kIOVA, ReadDataBufferAddress(rig.addressManager, orbAddress));
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(kIOVA, &value));
    ASSERT_TRUE(rig.bus.CompleteWrite(write.handle, ASFW::Async::AsyncStatus::kSuccess));

    CompleteTaskManagementStatus(rig.addressManager, rig.sessionStatusAddress, orbAddress);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_EQ(0, delivered->transportStatus);
    EXPECT_EQ(0x1000u, delivered->inPlaceBytes);
    EXPECT_TRUE(delivered->payload.empty());
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(orbAddress, &value));
}

// With the caller's own buffer, a result after an unanswered LUN reset must not
// go out before the bus reset that ends the target's claim on that buffer.
TEST(SessionRegistryTests, InPlaceResultAfterFailedLunResetWaitsForTheBusReset) {
    SessionRegistryRig rig;
    int resets = 0;
    rig.registry.SetBusResetRequester([&]() { ++resets; });
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x28;
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 0x1000;
    request.dataIOVA = 0x8040'0000ULL;
    request.timeoutMs = 1000;
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult&) { ++callbacks; }));
    ASSERT_TRUE(rig.bus.CompleteWrite(rig.bus.WriteAt(rig.bus.WriteCount() - 1).handle,
                                      ASFW::Async::AsyncStatus::kSuccess));
    rig.AdvanceMs(1001);  // ORB timeout → AGENT_RESET + LUN-reset management write
    auto mgmtWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    if (mgmtWrite.data.size() != 8u) {
        mgmtWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 2);
    }
    ASSERT_EQ(8u, mgmtWrite.data.size());
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    rig.AdvanceMs(3001);  // LUN reset never answered
    EXPECT_EQ(1, resets);
    EXPECT_EQ(0, callbacks);
    rig.registry.OnBusReset(2);
    EXPECT_EQ(1, callbacks);
}

// Timeout while a client ABORT TASK SET is already in flight: no LUN reset can
// be issued, and the caller's buffer is handed back only with that status.
TEST(SessionRegistryTests, InPlaceResultWithoutLunResetWaitsForTheClientTaskManagement) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x28;
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = 0x1000;
    request.dataIOVA = 0x8050'0000ULL;
    request.timeoutMs = 1000;
    int callbacks = 0;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request,
        [&](const SCSI::CommandResult&) { ++callbacks; }));
    const auto orbWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t orbAddress = DecodeAddressFromWritePayload(orbWrite.data);
    ASSERT_TRUE(rig.bus.CompleteWrite(orbWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    ASSERT_TRUE(rig.registry.SubmitTaskManagement(SessionRegistryRig::Owner(), handle,
                                                  SBP2ManagementORB::Function::AbortTaskSet));
    const auto mgmtWrite = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    ASSERT_EQ(8u, mgmtWrite.data.size());
    const uint64_t mgmtOrbAddress = DecodeAddressFromWritePayload(mgmtWrite.data);
    ASSERT_TRUE(rig.bus.CompleteWrite(mgmtWrite.handle, ASFW::Async::AsyncStatus::kSuccess));

    rig.AdvanceMs(1001);  // ORB timeout; the LUN reset cannot be submitted
    while (rig.bus.PendingWriteCount() > 0) {
        ASSERT_TRUE(rig.bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
    }
    EXPECT_EQ(0, callbacks);
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              rig.addressManager.ReadQuadlet(orbAddress, &value));

    CompleteTaskManagementStatus(rig.addressManager,
                                 ReadTaskManagementStatusAddress(rig.addressManager, mgmtOrbAddress),
                                 mgmtOrbAddress);
    EXPECT_EQ(1, callbacks);
    EXPECT_EQ(ASFW::Async::ResponseCode::AddressError,
              rig.addressManager.ReadQuadlet(orbAddress, &value));
}

TEST(SessionRegistryTests, InPlaceDataOutCarriesNoPayload) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x2a;
    request.direction = SCSI::DataDirection::ToTarget;
    request.transferLength = 512;
    request.dataIOVA = 0x8020'0000ULL;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    EXPECT_EQ(0x8020'0000ULL,
              ReadDataBufferAddress(rig.addressManager, DecodeAddressFromWritePayload(write.data)));
}

TEST(SessionRegistryTests, InPlacePageTableCoversTheCallersRange) {
    SessionRegistryRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);
    constexpr uint64_t kIOVA = 0x8030'0000ULL;
    constexpr uint32_t kLength = 1024u * 1024u;
    SCSI::CommandRequest request{};
    request.cdb = std::vector<uint8_t>(10, 0);
    request.cdb[0] = 0x28;
    request.direction = SCSI::DataDirection::FromTarget;
    request.transferLength = kLength;
    request.dataIOVA = kIOVA;
    ASSERT_TRUE(rig.registry.SubmitCommand(SessionRegistryRig::Owner(), handle, request));
    const auto write = rig.bus.WriteAt(rig.bus.WriteCount() - 1);
    const uint64_t orbAddress = DecodeAddressFromWritePayload(write.data);
    const uint64_t descriptor = ReadDataBufferAddress(rig.addressManager, orbAddress);
    const uint32_t entries = OSSwapBigToHostInt32(
        ReadQuadlet(rig.addressManager, orbAddress + offsetof(NormalORB, options))) & 0xFFFFu;
    EXPECT_EQ((kLength + 0xEFFFu) / 0xF000u, entries);
    uint64_t expected = kIOVA;
    uint32_t total = 0;
    for (uint32_t index = 0; index < entries; ++index) {
        const uint32_t hi = OSSwapBigToHostInt32(ReadQuadlet(rig.addressManager, descriptor + index * 8));
        const uint32_t lo = OSSwapBigToHostInt32(ReadQuadlet(rig.addressManager, descriptor + index * 8 + 4));
        EXPECT_EQ(expected, ComposeAddress(hi & 0xFFFFu, lo));
        expected += hi >> 16;
        total += hi >> 16;
    }
    EXPECT_EQ(kLength, total);
}

} // namespace
