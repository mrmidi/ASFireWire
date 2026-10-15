// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::DICE::Diagnostics::Registers {

// Diagnostic address catalog, separate from stream programming and transport.
// A register address alone never authorizes a read: a dump also needs a
// positively identified chip and a verified firmware read path.
struct ReceiverRegister {
    uint32_t offset;
    const char* name;
};

namespace Tcd22xx {
// TCD2210 (DICE Mini) / TCD2220 (DICE Jr), TCD2210/2220 User's Guide §6.1.
// Verified through the Pro24 DSP firmware PEEK (frmwload_cmd_test_action
// 0x6A384); a plain DICE II (ASIC type 0) is not verified.
inline constexpr std::array<uint32_t, 2> kReceiverBases{0xCF000000, 0xCF000030};
inline constexpr std::array<ReceiverRegister, 5> kReceiverRegisters{{
    {0x00, "CFG0"}, {0x04, "CFG1"}, {0x20, "CIP0"}, {0x24, "CIP1"}, {0x28, "ADO_CFG"},
}};
inline constexpr uint32_t kChannelMask = 0x3F;
inline constexpr uint32_t kChannelShift = 0;
inline constexpr uint32_t kReceiverLocked = 1U << 28; // ADO_CFG
inline constexpr uint32_t kReceiverEvents = 0xCF00013C;
inline constexpr uint32_t kTransmitterEvents = 0xCF000144;
// Media FIFO partitions (§6.7): inclusive RAM address ranges, BASE then LIMIT,
// 12-byte stride. The third word of each triple is FLUSH: never read or write it.
inline constexpr uint32_t kPartitionBase0 = 0xCF000184;
inline constexpr uint32_t kPartitionStride = 12;
inline constexpr uint32_t kPartitionLimitOffset = 4;
inline constexpr uint32_t kPartitionCount = 4;
// Read-to-clear (User's Guide §6.7.5): explicit opt-in only.
inline constexpr uint32_t kFifoEventsReadToClear = 0xCF0001FC;
}

namespace DiceIII {
// Decode-only: no verified read path exists. The Pro40 firmware's DCP window
// (0xFFFFE0400000) has no address read, and its block reads advance the DCP
// transport (tools/firmware_analysis/Pro40d3-DCP.md). Never address this map.
// Pro40d3Firmware.elf.i64: setup 0xEB00; CFG0 writer 0xE9AC;
// delay writer 0xEA20; CFG3 writer 0xEA6C; lock reader 0xFACC.
// Poller 0xFBD0 reads STATUS then writes it back to clear event latches;
// the clock task 0x12C54 invokes it every 20 ms.
inline constexpr std::array<uint32_t, 2> kReceiverBases{0x60001000, 0x60001020};
inline constexpr std::array<ReceiverRegister, 4> kReceiverRegisters{{
    {0x00, "CFG0"}, {0x08, "DELAY"}, {0x0C, "CFG3"}, {0x1C, "STATUS"},
}};
inline constexpr uint32_t kChannelMask = 0x003F0000;
inline constexpr uint32_t kChannelShift = 16;
inline constexpr uint32_t kReceiverLocked = 1U;
inline constexpr uint32_t kLatchedEventsMask = 0x3FFF0000;
struct ReceiverEvent {
    uint32_t mask;
    const char* name;
};
// Bit order is preserved by the firmware's 14-counter loop at 0xFC30–0xFC50
// and the saved avs_RxPollStatusEvents register annotation.
inline constexpr std::array<ReceiverEvent, 14> kReceiverEvents{{
    {1U << 16, "UNLOCK"}, {1U << 17, "LOCK"}, {1U << 18, "SYT_OFL"},
    {1U << 19, "FIFO_OFL"}, {1U << 20, "NO_SYNC"}, {1U << 21, "SLIP"},
    {1U << 22, "REPEAT"}, {1U << 23, "INT_E"}, {1U << 24, "CRC_E"},
    {1U << 25, "DBC_E"}, {1U << 26, "CIP_E"}, {1U << 27, "CFG_E"},
    {1U << 28, "STALE_E"}, {1U << 29, "LENGTH_E"},
}};
}

static_assert(Tcd22xx::kReceiverBases[1] - Tcd22xx::kReceiverBases[0] == 0x30);
static_assert(DiceIII::kReceiverBases[1] - DiceIII::kReceiverBases[0] == 0x20);
static_assert((DiceIII::kChannelMask >> DiceIII::kChannelShift) == 63);

} // namespace ASFW::Audio::DICE::Diagnostics::Registers
