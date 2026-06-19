#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

// The host stub's IOUserClientMethodArguments::structureInput is a void*, which
// dynamic_cast (the host OSDynamicCast) cannot accept. Override to static_cast for
// the handler include — the real DriverKit build keeps the RTTI-based macro.
#undef OSDynamicCast
#define OSDynamicCast(type, object) static_cast<type*>(object)
#include "ASFWDriver/UserClient/Handlers/SBP2Handler.hpp"
#include "ASFWDriver/UserClient/WireFormats/SBP2CommandWireFormats.hpp"
#undef OSDynamicCast

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

// SBP2Handler host tests — the two user-client ABI tests deferred from PR #19's
// SBP2SessionRegistryTests (FW-56 handoff). They exercise the session-aware handler
// re-threaded onto DICE's SessionRegistry: scalar-output sizing for GetSBP2SessionState
// and the SubmitSBP2Command structure-input ABI hardening.

namespace {

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
using ASFW::Protocols::SBP2::SessionRegistry;
using ASFW::Testing::FakeSessionScheduler;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;
namespace UCWire = ASFW::UserClient::Wire;

constexpr uint32_t kSBP2UnitSpecId = 0x00609E;
constexpr uint32_t kSBP2UnitSwVersion = 0x010483;

uint64_t ComposeAddress(uint16_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t DecodeAddressFromWritePayload(std::span<const uint8_t> payload) {
    const uint16_t addressHi =
        static_cast<uint16_t>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    const uint32_t addressLo =
        (static_cast<uint32_t>(payload[4]) << 24) | (static_cast<uint32_t>(payload[5]) << 16) |
        (static_cast<uint32_t>(payload[6]) << 8) | static_cast<uint32_t>(payload[7]);
    return ComposeAddress(addressHi, addressLo);
}

uint32_t ReadQuadlet(AddressSpaceManager& manager, uint64_t address) {
    uint32_t value = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete, manager.ReadQuadlet(address, &value));
    return value;
}

uint64_t ReadORBAddress(AddressSpaceManager& manager, uint64_t orbAddress,
                        size_t hiOffset, size_t loOffset) {
    const uint32_t hi = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + hiOffset));
    const uint32_t lo = OSSwapBigToHostInt32(ReadQuadlet(manager, orbAddress + loOffset));
    return ComposeAddress(static_cast<uint16_t>(hi & 0xFFFFu), lo);
}

std::vector<uint8_t> BuildCommandRequestWire(std::vector<uint8_t> cdb,
                                             uint32_t transferLength = 0,
                                             uint8_t direction = 0,
                                             std::vector<uint8_t> outgoingPayload = {},
                                             uint8_t captureSenseData = 0,
                                             uint8_t reserved0 = 0,
                                             uint8_t reserved1 = 0) {
    UCWire::SBP2CommandRequestWire header{};
    header.cdbLength = static_cast<uint32_t>(cdb.size());
    header.transferLength = transferLength;
    header.outgoingLength = static_cast<uint32_t>(outgoingPayload.size());
    header.direction = direction;
    header.captureSenseData = captureSenseData;
    header._reserved[0] = reserved0;
    header._reserved[1] = reserved1;

    std::vector<uint8_t> serialized(sizeof(header) + cdb.size() + outgoingPayload.size());
    std::memcpy(serialized.data(), &header, sizeof(header));
    size_t offset = sizeof(header);
    if (!cdb.empty()) {
        std::memcpy(serialized.data() + offset, cdb.data(), cdb.size());
        offset += cdb.size();
    }
    if (!outgoingPayload.empty()) {
        std::memcpy(serialized.data() + offset, outgoingPayload.data(), outgoingPayload.size());
    }
    return serialized;
}

class HandlerRig {
public:
    HandlerRig() : registry(bus, bus, addressManager, deviceManager, scheduler, &queue) {
        queue.SetManualDispatchForTesting(true);
        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);
        UpsertDevice();
    }

    void UpsertDevice() {
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
        ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
    }

    uint64_t CreateSession() {
        auto result = registry.CreateSession(Owner(), kGuid, 0);
        EXPECT_TRUE(result.has_value());
        return result.value_or(0);
    }

    void LoginSuccessfully(uint64_t handle) {
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
        response.loginID = OSSwapHostToBigInt16(0x0042);
        response.commandBlockAgentAddressHi = OSSwapHostToBigInt32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = OSSwapHostToBigInt32(0x0020'0000u);
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

    ASFW::Async::Testing::DeferredFireWireBus bus;
    FakeSessionScheduler scheduler;
    AddressSpaceManager addressManager{nullptr};
    DeviceManager deviceManager;
    IODispatchQueue queue;
    SessionRegistry registry;
    uint64_t sessionStatusAddress{0};
};

TEST(SBP2HandlerTests, HandlerRejectsMissingOrShortSessionStateOutput) {
    HandlerRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    ASFW::UserClient::SBP2Handler handler(nullptr, &rig.registry);
    uint64_t scalarInput[] = {handle};

    IOUserClientMethodArguments args{};
    args.scalarInput = scalarInput;
    args.scalarInputCount = 1;
    EXPECT_EQ(kIOReturnBadArgument, handler.GetSBP2SessionState(&args, HandlerRig::Owner()));

    uint64_t shortOutput[4]{};
    args.scalarOutput = shortOutput;
    args.scalarOutputCount = 4;
    EXPECT_EQ(kIOReturnBadArgument, handler.GetSBP2SessionState(&args, HandlerRig::Owner()));

    uint64_t output[5]{};
    args.scalarOutput = output;
    args.scalarOutputCount = 5;
    EXPECT_EQ(kIOReturnSuccess, handler.GetSBP2SessionState(&args, HandlerRig::Owner()));
    EXPECT_EQ(5u, args.scalarOutputCount);
}

TEST(SBP2HandlerTests, HandlerHardensCommandABIInputsBeforeSubmission) {
    HandlerRig rig;
    const uint64_t handle = rig.CreateSession();
    rig.LoginSuccessfully(handle);

    ASFW::UserClient::SBP2Handler handler(nullptr, &rig.registry);
    uint64_t scalarInput[] = {handle};

    auto submit = [&](const std::vector<uint8_t>& serialized) {
        std::unique_ptr<OSData> input(
            OSData::withBytes(serialized.data(), static_cast<uint32_t>(serialized.size())));
        IOUserClientMethodArguments args{};
        args.scalarInput = scalarInput;
        args.scalarInputCount = 1;
        args.structureInput = input.get();
        return handler.SubmitSBP2Command(&args, HandlerRig::Owner());
    };

    EXPECT_EQ(kIOReturnBadArgument, submit(BuildCommandRequestWire(std::vector<uint8_t>(17, 0x00))));
    EXPECT_EQ(kIOReturnBadArgument, submit(BuildCommandRequestWire({0x00}, 0, 0, {}, 2)));
    EXPECT_EQ(kIOReturnBadArgument, submit(BuildCommandRequestWire({0x00}, 0, 0, {}, 0, 1)));
    EXPECT_EQ(kIOReturnBadArgument,
              submit(BuildCommandRequestWire({0x2A}, UCWire::kSBP2CommandMaxTransferLength + 1, 1)));
    EXPECT_EQ(kIOReturnBadArgument, submit(BuildCommandRequestWire({0x2A}, 4, 2, {0x01, 0x02})));
    EXPECT_EQ(kIOReturnBadArgument, submit(BuildCommandRequestWire({0x2A}, 4, 1, {0x01})));
    EXPECT_EQ(kIOReturnSuccess, submit(BuildCommandRequestWire({0x00})));
}

} // namespace
