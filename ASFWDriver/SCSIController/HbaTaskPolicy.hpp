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

[[nodiscard]] constexpr Disposition Classify(uint64_t targetID, uint8_t opcode,
                                             bool sessionReady) {
    // A target other than 0 answers nothing, not even a synthetic GOOD:
    // forwarding the scan's INQUIRY to ID 1 published the same scanner twice
    // (HW-observed). Failing it like an absent device makes the scan drop it.
    if (targetID != kBridgedTargetID) {
        return Disposition::NotPresent;
    }
    // RESERVE/RELEASE are forwarded like any CDB, as Apple's SBP-2 transport
    // does. The Minolta FS-V2 needs RELEASE after each SCAN: answering it
    // locally hung the next SCAN (#139).
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

// ---------------------------------------------------------------------------
// Short transfers
// ---------------------------------------------------------------------------

// Short transfers go through a driver-owned buffer instead of the family's
// task buffer:
//  * data-out under 512 bytes whose length is not a multiple of four, as
//    Apple's SBP-2 transport double-buffers them (IOFireWireSerialBusProtocol-
//    Transport.cpp SetCommandBuffers, kDoubleBufferCommandSizeCheckThreshold);
//    SBP-2 leaves the bytes of a partial last quadlet undefined;
//  * every data-in under 512 bytes. A target may write its whole response
//    whatever the allocation length and data_size say, and a small task
//    buffer from the family shares its page — the unit the DMA mapping
//    exposes — with other kernel memory. HW 2026-11-07 (Leaf Valeo 11): the
//    target wrote its full 36-byte INQUIRY data into every INQUIRY, standard
//    or VPD, with data_size 4, 5 and 6 alike (4 is quadlet-aligned, so the
//    alignment rule alone would not cover it); vendor reads of 64 B and
//    61,440 B stopped exactly at data_size. Before this, the 6-byte INQUIRY's
//    overrun landed in a neighbouring kalloc.16 element and panicked.
inline constexpr uint64_t kShortTransferBounceLimit = 512;

[[nodiscard]] constexpr bool ShouldBounceTransfer(uint64_t length, bool dataIn) {
    if (length == 0 || length >= kShortTransferBounceLimit) {
        return false;
    }
    return dataIn || (length & 3u) != 0;
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
