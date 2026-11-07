#pragma once

// HbaTaskPolicy — the SCSI HBA's target-ID and per-task decisions, split out of
// ASFWSCSIController (IIG-only) so they run on the host. The controller maps a
// Disposition onto its SCSIUserParallelResponse; this header knows no DriverKit.

#include <cstdint>

namespace ASFW::Protocols::SBP2::HbaTaskPolicy {

// The one target the SBP-2 login maps to.
inline constexpr uint64_t kBridgedTargetID = 0;

// Only target 0 is ever bridged, but report 1. The family's willTerminate
// flushes and destroys targets with `index < fHighestSupportedDeviceID`
// (strict <, unlike start's `<=`; OSS IOSCSIParallelInterfaceController.cpp
// willTerminate, same in the 26.x/27 kernelcache). With 0 it skipped target 0:
// its outstanding tasks stayed live, stop() freed fWorkLoop, and our late
// ParallelTaskCompletion NULL-dereferenced it in CompleteParallelTask (#139).
// Should Apple make that loop `<=`, 1 still covers target 0. The bring-up scan
// does instantiate ID 1 (presence false does not stop it); Classify fails
// every task to it, so it never probes.
inline constexpr uint64_t kReportedHighestTargetID = 1;

// Largest data transfer one task may carry: Apple's SBP-2 transport ceiling,
// kFWSBP2DefaultPageTableEntriesCount (512) page-table entries of
// kFWSBP2MaxPageClusterSize (0xF000) bytes (IOFireWireSerialBusProtocolTransport.cpp:193,
// :411-415; IOFireWireSBP2ORB.h:61). The earlier 1 MiB cap, sized for scanner
// reads (~510 KB), was silently clipped: a Hasselblad H4D calibration READ of
// 1,966,080 bytes went out as a 1 MiB page table and came back all zeros.
inline constexpr uint64_t kMaxPageTableEntries = 512;
inline constexpr uint64_t kMaxPageClusterBytes = 0xF000;
inline constexpr uint64_t kMaxTransferBytes = kMaxPageTableEntries * kMaxPageClusterBytes;

enum class Disposition {
    NotPresent,     // SERVICE_DELIVERY_OR_TARGET_FAILURE / DeviceNotPresent
    SyntheticGood,  // GOOD without touching the wire
    Busy,           // BUSY: the initiator retries
    Forward,        // bridge to the SBP-2 command plane
};

inline constexpr uint8_t kOpTestUnitReady = 0x00;
inline constexpr uint8_t kOpRequestSense  = 0x03;
inline constexpr uint8_t kOpReserve6      = 0x16;
inline constexpr uint8_t kOpRelease6      = 0x17;
inline constexpr uint8_t kOpReserve10     = 0x56;
inline constexpr uint8_t kOpRelease10     = 0x57;

[[nodiscard]] constexpr bool IsReserveRelease(uint8_t opcode) {
    return opcode == kOpReserve6 || opcode == kOpRelease6 ||
           opcode == kOpReserve10 || opcode == kOpRelease10;
}

[[nodiscard]] constexpr Disposition Classify(uint64_t targetID, uint8_t opcode,
                                             bool sessionReady) {
    // A target other than 0 answers nothing, not even a synthetic GOOD:
    // forwarding the scan's INQUIRY to ID 1 published the same scanner twice
    // (HW-observed). Failing it like an absent device makes the scan drop it.
    if (targetID != kBridgedTargetID) {
        return Disposition::NotPresent;
    }
    // RESERVE/RELEASE never reach the wire. This HBA owns the only initiator on
    // the bus, so a reservation is uncontended and trivially GOOD for any
    // single-initiator SBP-2 target. (The working Sequoia stack, VueScan via
    // IOFireWireSBP2Lib, never sent them, and LS-9000 firmware wedges on a
    // RESERVE(6) retry after UNIT ATTENTION.)
    if (IsReserveRelease(opcode)) {
        return Disposition::SyntheticGood;
    }
    if (!sessionReady) {
        // Suspended window: every data-carrying command, INQUIRY included,
        // answers BUSY so the initiator's bounded retries land after the
        // reconnect or fail cleanly. Nothing is held without a deadline (a held
        // INQUIRY was the #54 strand). TUR/REQUEST SENSE stay GOOD so probes move.
        return (opcode == kOpTestUnitReady || opcode == kOpRequestSense)
                   ? Disposition::SyntheticGood
                   : Disposition::Busy;
    }
    return Disposition::Forward;
}

// ---------------------------------------------------------------------------
// The SCSI family's attach probe
// ---------------------------------------------------------------------------

// The family probes a new target inside UserCreateTargetForID(0)
// (TargetLifecycle::IsCreatingTarget): TEST UNIT READY, INQUIRY, VPD, REPORT
// LUNS, then the same for the logical unit it creates. Its TEST UNIT READYs
// are answered from what the readiness gate learned on this login a moment
// before (SBP2TargetBridge::AttachProbeAnswer) instead of being sent again;
// every later TEST UNIT READY goes to the target. The family needs a completed
// TUR to accept the target (IOSCSITargetDevice VerifyLogicalUnitPresence takes
// only TASK_COMPLETE); a target that does not implement TUR and stays silent
// would otherwise fail the attach after the family's 10 s timeout and a LUN
// reset per TUR.
[[nodiscard]] constexpr bool IsAttachProbeTUR(uint64_t targetID, uint8_t opcode,
                                              bool creatingTarget) {
    return creatingTarget && targetID == kBridgedTargetID && opcode == kOpTestUnitReady;
}

// ---------------------------------------------------------------------------
// REPORT LUNS
// ---------------------------------------------------------------------------

inline constexpr uint8_t kOpReportLuns = 0xA0;
inline constexpr uint8_t kReportLunsSelectWellKnownOnly = 0x01;
inline constexpr uint32_t kReportLunsSingleLunBytes = 16;  // 8-byte header + one 8-byte entry

// SBP-2 binds the logical unit at login (the login ORB names it; command ORBs
// carry none), and the bridge maps that one login to SCSI LUN 0. The HBA
// therefore answers REPORT LUNS itself for every SBP-2 target: LUN LIST LENGTH
// 8 and one all-zero entry (peripheral device addressing, LUN 0); an empty
// list for SELECT REPORT 01h (well-known LUNs only). Apple's SBP-2 stack takes
// LUNs from the Config ROM and never sends REPORT LUNS. The caller copies at
// most the allocation length.
struct ReportLunsData {
    uint8_t bytes[kReportLunsSingleLunBytes]{};
    uint32_t length{0};
};

[[nodiscard]] constexpr ReportLunsData BuildReportLunsData(uint8_t selectReport) {
    ReportLunsData data{};
    data.length = kReportLunsSingleLunBytes;
    if (selectReport == kReportLunsSelectWellKnownOnly) {
        data.length = 8;  // header only, LUN LIST LENGTH 0
        return data;
    }
    data.bytes[3] = 8;  // LUN LIST LENGTH, big-endian: one 8-byte entry
    return data;
}

// Bytes to copy into the task buffer (REPORT LUNS data, a bounced data-in):
// never more than the task asked for, the source holds, or the task buffer
// maps.
[[nodiscard]] constexpr uint64_t CopyBackLength(uint64_t requested,
                                                  uint64_t delivered,
                                                  uint64_t taskBufferLength) {
    uint64_t n = requested;
    if (delivered < n) n = delivered;
    if (taskBufferLength < n) n = taskBufferLength;
    return n;
}

} // namespace ASFW::Protocols::SBP2::HbaTaskPolicy
