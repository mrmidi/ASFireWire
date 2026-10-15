// SPDX-License-Identifier: Apache-2.0
// The TCD22xx receiver dump through the TCAT firmware-loader PEEK
// (FFADO dice_firmware_loader.cpp:300-330), against a simulated mailbox.
#include "DICEDuplexTestSupport.hpp"
#include "Audio/Diagnostics/DICE/DiceReceiverDump.hpp"

#include <map>

namespace {
using namespace ASFW::Testing::DICE;
using ASFW::Audio::DICE::Diagnostics::BuildTcd22xxDumpPlan;
using ASFW::Audio::DICE::Diagnostics::DiceDumpPhase;
using ASFW::Audio::DICE::Diagnostics::DiceDumpRefusal;
using ASFW::Audio::DICE::Diagnostics::DiceDumpStage;
using ASFW::Audio::DICE::Diagnostics::DiceDumpTrigger;
using ASFW::Audio::DICE::Diagnostics::DiceLockSnapshot;
using ASFW::Audio::DICE::Diagnostics::DiceReceiverDump;

constexpr uint32_t kExtension = 0xE0200000U;
constexpr uint32_t kCapsGeneral = kExtension + 0x28 + 0x08; // caps section at quadlet 0x0A
constexpr uint32_t kOpcode = 0xE0100004U;
constexpr uint32_t kStatus = 0xE0100008U;
constexpr uint32_t kParameter = 0xE010002CU;
constexpr uint32_t kExecutePeek = 0x80000006U;

bool InLoader(uint32_t addressLo) { return (addressLo & 0xFFF00000U) == 0xE0100000U; }

// One TCAT loader mailbox: parameters, an opcode whose bit 31 stays set for
// `busyPolls` reads after execute, a return status, and a register file.
struct LoaderRig {
    ASFW::Testing::RecordingFireWireBus bus;
    RouteState route;
    ProtocolRegisterIO io{bus, bus, route.registry, route.route};
    ASFW::Testing::FakeTimerScheduler timer;
    uint32_t asic{1}; // TCD2210
    uint32_t opcode{0};
    uint32_t returnStatus{0};
    uint32_t busyPolls{1};
    bool busyForever{false};
    bool corruptParams{false};
    uint32_t pollsLeft{0};
    std::array<uint8_t, 8> params{};
    uint32_t result{0};
    std::vector<uint32_t> peeked;
    std::map<uint32_t, uint32_t> registers;

    LoaderRig() {
        bus.SetReadResponder([this](FWAddress address, uint32_t length) -> std::optional<std::vector<uint8_t>> {
            std::vector<uint8_t> bytes(length, 0);
            const uint32_t lo = address.addressLo;
            if (lo == kExtension) {
                ASFW::FW::WriteBE32(bytes.data(), 0x0A); // caps offset (quadlets)
                ASFW::FW::WriteBE32(bytes.data() + 4, 0x04); // caps size (quadlets)
            } else if (lo == kCapsGeneral) {
                ASFW::FW::WriteBE32(bytes.data(), asic << 16);
            } else if (lo == kOpcode) {
                if ((opcode & 0x80000000U) && !busyForever) {
                    if (pollsLeft == 0) Finish();
                    else --pollsLeft;
                }
                ASFW::FW::WriteBE32(bytes.data(), opcode);
            } else if (lo == kStatus) {
                ASFW::FW::WriteBE32(bytes.data(), returnStatus);
            } else if (lo == kParameter && length == 8) {
                std::copy(params.begin(), params.end(), bytes.begin());
                if (corruptParams) bytes[7] ^= 0x01;
            } else if (lo == kParameter) {
                ASFW::FW::WriteBE32(bytes.data(), result);
            }
            return bytes;
        });
        bus.SetWriteResponder([this](FWAddress address, std::span<const uint8_t> data) {
            if (address.addressLo == kParameter && data.size() == 8) {
                std::copy(data.begin(), data.end(), params.begin());
            } else if (address.addressLo == kOpcode && data.size() == 4) {
                opcode = ASFW::FW::ReadBE32(data.data());
                pollsLeft = busyPolls;
            }
        });
    }
    void Finish() {
        EXPECT_EQ(ASFW::FW::ReadBE32(params.data()), 2U) << "only sub-command 2 (PEEK) may execute";
        const uint32_t address = ASFW::FW::ReadBE32(params.data() + 4);
        peeked.push_back(address);
        const auto it = registers.find(address);
        result = it != registers.end() ? it->second : (0xA5000000U | (address & 0xFFFFU));
        opcode &= ~0x80000000U;
    }
    [[nodiscard]] std::vector<ASFW::Testing::RecordedOp> LoaderOps() const {
        std::vector<ASFW::Testing::RecordedOp> ops;
        for (const auto& op : bus.Operations()) if (InLoader(op.addressLo)) ops.push_back(op);
        return ops;
    }
    [[nodiscard]] size_t Writes() const {
        return static_cast<size_t>(std::ranges::count_if(bus.Operations(), [](const auto& op) {
            return op.kind == OpKind::Write;
        }));
    }
    void Run(uint64_t ms = 2000) { timer.Advance(ms * 1'000'000); }
};

TEST(DiceReceiverDumpTests, UnverifiedFirmwareIsRefusedWithoutAnyBusRequest) {
    LoaderRig rig;
    DiceReceiverDump dump(false);
    EXPECT_EQ(dump.Request(rig.io, &rig.timer, false), kIOReturnSuccess);
    const auto result = dump.Copy();
    EXPECT_EQ(result.phase, DiceDumpPhase::Refused);
    EXPECT_EQ(result.refusal, DiceDumpRefusal::NotVerifiedFirmware);
    EXPECT_TRUE(rig.bus.Operations().empty());
}

TEST(DiceReceiverDumpTests, NoExtensionIsRefusedBeforeTheMailbox) {
    LoaderRig rig;
    rig.bus.FailNext(OpKind::Read, kExtension, AsyncStatus::kTimeout);
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, false);
    rig.Run();
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Refused);
    EXPECT_EQ(dump.Copy().refusal, DiceDumpRefusal::NoExtension);
    EXPECT_TRUE(rig.LoaderOps().empty());
}

TEST(DiceReceiverDumpTests, OnlyATcd22xxAsicMayBeRead) {
    for (const uint32_t asic : {0U, 3U, 0xFFFFU}) {
        LoaderRig rig;
        rig.asic = asic;
        DiceReceiverDump dump(true);
        (void)dump.Request(rig.io, &rig.timer, false);
        rig.Run();
        EXPECT_EQ(dump.Copy().refusal, DiceDumpRefusal::AsicNotTcd22xx) << asic;
        EXPECT_EQ(dump.Copy().asicType, asic);
        EXPECT_TRUE(rig.LoaderOps().empty()) << asic;
    }
}

TEST(DiceReceiverDumpTests, AbsentOrBusyLoaderIsRefusedWithoutAnyWrite) {
    {
        LoaderRig rig;
        rig.bus.FailNext(OpKind::Read, kOpcode, AsyncStatus::kTimeout);
        DiceReceiverDump dump(true);
        (void)dump.Request(rig.io, &rig.timer, false);
        rig.Run();
        EXPECT_EQ(dump.Copy().refusal, DiceDumpRefusal::LoaderAbsent);
        EXPECT_EQ(rig.Writes(), 0U);
    }
    {
        LoaderRig rig;
        rig.opcode = 0x80000003U; // an upload in progress
        rig.busyForever = true;
        DiceReceiverDump dump(true);
        (void)dump.Request(rig.io, &rig.timer, false);
        rig.Run();
        EXPECT_EQ(dump.Copy().refusal, DiceDumpRefusal::LoaderBusy);
        EXPECT_EQ(rig.Writes(), 0U);
    }
}

TEST(DiceReceiverDumpTests, CompleteDumpPeeksExactlyTheAllowlistInOrder) {
    LoaderRig rig;
    rig.registers[0xCF000028U] = 0x90000010U; // ARX1 ADO_CFG: unmuted, locked
    DiceReceiverDump dump(true);
    ASSERT_EQ(dump.Request(rig.io, &rig.timer, false), kIOReturnSuccess);
    rig.Run();
    const auto result = dump.Copy();
    const auto plan = BuildTcd22xxDumpPlan(false);
    ASSERT_EQ(result.phase, DiceDumpPhase::Complete);
    EXPECT_EQ(result.asicType, 1U);
    EXPECT_EQ(result.count, plan.count);
    EXPECT_EQ(result.count, 20U);
    EXPECT_EQ(result.validMask, (1U << plan.count) - 1U);
    ASSERT_EQ(rig.peeked.size(), plan.count);
    for (uint32_t i = 0; i < plan.count; ++i) {
        EXPECT_EQ(rig.peeked[i], plan.addresses[i]);
        EXPECT_EQ(result.entries[i].address, plan.addresses[i]);
    }
    EXPECT_EQ(result.entries[5].value, 0x90000010U); // RX_EVENTS, then ARX1 CFG0..CIP1, ADO_CFG
    // Every write is a PEEK parameter block or the PEEK opcode, nothing else.
    for (const auto& op : rig.bus.Operations()) {
        if (op.kind != OpKind::Write) continue;
        const bool params = op.addressLo == kParameter && op.payload.size() == 8 &&
                            ASFW::FW::ReadBE32(op.payload.data()) == 2U;
        const bool execute = op.addressLo == kOpcode && op.payload.size() == 4 &&
                             ASFW::FW::ReadBE32(op.payload.data()) == kExecutePeek;
        EXPECT_TRUE(params || execute) << std::hex << op.addressLo;
    }
    // FLUSH words and the read-to-clear FIFO flags are never addressed.
    for (const uint32_t address : rig.peeked) {
        if (address >= 0xCF000184U && address < 0xCF000184U + 4 * 12)
            EXPECT_NE((address - 0xCF000184U) % 12, 8U) << std::hex << address;
        EXPECT_NE(address, 0xCF0001FCU);
    }
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceReceiverDumpTests, ReadToClearFifoEventsOnlyOnRequest) {
    LoaderRig rig;
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, true);
    rig.Run();
    ASSERT_EQ(dump.Copy().phase, DiceDumpPhase::Complete);
    EXPECT_EQ(dump.Copy().count, 21U);
    EXPECT_EQ(rig.peeked.back(), 0xCF0001FCU);
    EXPECT_EQ(dump.Copy().flags, 1U);
}

TEST(DiceReceiverDumpTests, ParametersChangedByAnotherMasterAreNeverExecuted) {
    LoaderRig rig;
    rig.corruptParams = true;
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, false);
    rig.Run();
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Failed);
    EXPECT_EQ(dump.Copy().failedStage, DiceDumpStage::VerifyParams);
    EXPECT_FALSE(std::ranges::any_of(rig.bus.Operations(), [](const auto& op) {
        return op.kind == OpKind::Write && op.addressLo == kOpcode;
    }));
}

TEST(DiceReceiverDumpTests, FirmwareRejectionStopsTheDump) {
    LoaderRig rig;
    rig.returnStatus = 0xC3000003U;
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, false);
    rig.Run();
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Failed);
    EXPECT_EQ(dump.Copy().failedStage, DiceDumpStage::Status);
    EXPECT_EQ(dump.Copy().lastError, 0xC3000003U);
    EXPECT_EQ(rig.peeked.size(), 1U);
}

TEST(DiceReceiverDumpTests, TimeoutLeavesTheMailboxAlone) {
    LoaderRig rig;
    rig.busyForever = true;
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, false);
    rig.Run(1999);
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Running);
    rig.Run(100);
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Failed);
    EXPECT_EQ(dump.Copy().failedStage, DiceDumpStage::Poll);
    EXPECT_EQ(dump.Copy().lastError, static_cast<uint32_t>(kIOReturnTimeout));
    const size_t writes = rig.Writes();
    EXPECT_EQ(writes, 2U); // one parameter block, one execute
    rig.Run(10000);
    EXPECT_EQ(rig.Writes(), writes);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceReceiverDumpTests, CancelStopsBusTrafficAndTimers) {
    LoaderRig rig;
    rig.busyPolls = 1000;
    DiceReceiverDump dump(true);
    (void)dump.Request(rig.io, &rig.timer, false);
    rig.Run(100);
    dump.Cancel();
    const size_t ops = rig.bus.Operations().size();
    EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Cancelled);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
    rig.Run(10000);
    EXPECT_EQ(rig.bus.Operations().size(), ops);
}

TEST(DiceReceiverDumpTests, SecondRequestWhileRunningIsBusy) {
    LoaderRig rig;
    rig.busyPolls = 1000;
    DiceReceiverDump dump(true);
    EXPECT_EQ(dump.Request(rig.io, &rig.timer, false), kIOReturnSuccess);
    EXPECT_EQ(dump.Request(rig.io, &rig.timer, false), kIOReturnBusy);
    dump.Cancel();
}

TEST(DiceReceiverDumpTests, LockVerdictDumpsOnlyWhenAReceiverNeverLockedOrLostLock) {
    DiceLockSnapshot locked{};
    locked.expectedMask = 1;
    locked.observedMask = 1;
    DiceLockSnapshot never = locked;
    never.observedMask = 0;
    DiceLockSnapshot lost = locked;
    lost.lostMask = 1;
    {
        LoaderRig rig;
        DiceReceiverDump dump(true);
        dump.VerdictTrigger(rig.io, &rig.timer)(locked);
        EXPECT_TRUE(rig.bus.Operations().empty());
        EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Idle);
    }
    for (const auto& verdict : {never, lost}) {
        LoaderRig rig;
        DiceReceiverDump dump(true);
        dump.VerdictTrigger(rig.io, &rig.timer)(verdict);
        rig.Run();
        EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Complete);
        EXPECT_EQ(dump.Copy().trigger, DiceDumpTrigger::LockVerdict);
    }
    {
        LoaderRig rig;
        DiceReceiverDump dump(false);
        dump.VerdictTrigger(rig.io, &rig.timer)(never);
        EXPECT_TRUE(rig.bus.Operations().empty());
        EXPECT_EQ(dump.Copy().phase, DiceDumpPhase::Idle); // no refusal noise per start
    }
}

TEST(DiceReceiverDumpTests, ATriggerOutlivingItsOwnerDoesNothing) {
    LoaderRig rig;
    std::function<void(const DiceLockSnapshot&)> trigger;
    {
        DiceReceiverDump dump(true);
        trigger = dump.VerdictTrigger(rig.io, &rig.timer);
    }
    DiceLockSnapshot never{};
    never.expectedMask = 1;
    trigger(never);
    rig.Run();
    EXPECT_TRUE(rig.bus.Operations().empty());
}
} // namespace
