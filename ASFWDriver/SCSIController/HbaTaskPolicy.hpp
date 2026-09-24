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

} // namespace ASFW::Protocols::SBP2::HbaTaskPolicy
