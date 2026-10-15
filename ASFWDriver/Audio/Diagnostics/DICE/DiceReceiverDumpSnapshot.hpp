// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DICE::Diagnostics {

enum class DiceDumpPhase : uint32_t { Idle, Running, Complete, Refused, Failed, Cancelled };

// Why a dump never touched the loader mailbox. Every refusal is decided
// before the first mailbox write.
enum class DiceDumpRefusal : uint32_t {
    None,
    NotVerifiedFirmware, // the protocol has no verified PEEK firmware
    NoExtension,         // no TCAT extension: the chip cannot be identified
    AsicNotTcd22xx,      // extension ASIC field is not TCD2210/TCD2220
    LoaderAbsent,        // no firmware-loader mailbox at 0xFFFFE0100000
    LoaderBusy,          // mailbox already executing a command
    NoScheduler,
};

// The step that failed once the mailbox exchange had begun.
enum class DiceDumpStage : uint32_t {
    None, Sections, Caps, Probe, WriteParams, VerifyParams, Execute, Poll, Status, Value,
};

enum class DiceDumpTrigger : uint32_t { Request, LockVerdict };

// Family-specific telemetry (selectors 1016/1017). Native little-endian
// scalar layout, copied under a lock; never a buffer view.
struct DiceReceiverDumpSnapshot {
    struct Entry {
        uint32_t address{0};
        uint32_t value{0};
    };
    static constexpr uint32_t kMaxEntries = 24;

    uint32_t version{1};
    uint32_t bytes{248};
    uint64_t guid{0};
    uint32_t generation{0};
    DiceDumpPhase phase{DiceDumpPhase::Idle};
    DiceDumpRefusal refusal{DiceDumpRefusal::None};
    DiceDumpStage failedStage{DiceDumpStage::None};
    uint32_t lastError{0};
    uint32_t asicType{0xFFFFFFFFU};
    DiceDumpTrigger trigger{DiceDumpTrigger::Request};
    uint32_t count{0};      // planned entries
    uint32_t validMask{0};  // bit i: entries[i].value was read
    uint32_t flags{0};      // bit 0: FIFO events (read-to-clear) were requested
    std::array<Entry, kMaxEntries> entries{};
};
static_assert(sizeof(DiceReceiverDumpSnapshot) == 248);
#define ASFW_DICE_DUMP_OFFSET(field, offset) \
    static_assert(offsetof(DiceReceiverDumpSnapshot, field) == offset)
ASFW_DICE_DUMP_OFFSET(version, 0);
ASFW_DICE_DUMP_OFFSET(bytes, 4);
ASFW_DICE_DUMP_OFFSET(guid, 8);
ASFW_DICE_DUMP_OFFSET(generation, 16);
ASFW_DICE_DUMP_OFFSET(phase, 20);
ASFW_DICE_DUMP_OFFSET(refusal, 24);
ASFW_DICE_DUMP_OFFSET(failedStage, 28);
ASFW_DICE_DUMP_OFFSET(lastError, 32);
ASFW_DICE_DUMP_OFFSET(asicType, 36);
ASFW_DICE_DUMP_OFFSET(trigger, 40);
ASFW_DICE_DUMP_OFFSET(count, 44);
ASFW_DICE_DUMP_OFFSET(validMask, 48);
ASFW_DICE_DUMP_OFFSET(flags, 52);
ASFW_DICE_DUMP_OFFSET(entries, 56);
#undef ASFW_DICE_DUMP_OFFSET

} // namespace ASFW::Audio::DICE::Diagnostics
