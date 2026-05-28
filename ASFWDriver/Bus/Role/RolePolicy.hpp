#pragma once

// RolePolicy.hpp — Layer 1 of the RoleCoordinator design (FW-6).
//
// Pure, deterministic root/cycle-master policy: value-in, value-out. No
// DriverKit, no hardware, no state, no allocation. This is the part that the
// whole FW-12 scenario matrix is tested against — build a RoleInputs, assert a
// RoleAction. See the FW-6 design comment in Linear for the three-layer split.
//
// SKELETON (FW-6): EvaluateRolePolicy implements only conservative defaults
// (None / DeferForEvidence) — it never issues a reset or cycle-master change.
// The real decision matrix (mirroring Linux bm_work, firewire/core-card.c:432)
// lands in FW-9; the branch structure here is laid out so FW-9 fills each case
// in place without reshaping the boundary.

#include <cstdint>

#include "../../Controller/ControllerTypes.hpp" // TopologySnapshot

namespace ASFW::Driver::Role {

// Root cycle-master capability verdict. Populated by FW-8 (BIB CMC + Config ROM
// read outcome, combined with cycle-start observation). Skeleton default Unknown.
enum class RootCapability : uint8_t {
    Unknown,                 // not yet probed
    CapableByBIB,            // BIB read OK, CMC=1
    IncapableByBIB,          // BIB read OK, CMC=0
    FunctioningByCycleStart, // BIB read failed, but cycle starts observed
    BadOrNonResponsive,      // BIB read failed and no cycle starts observed
};

// Cycle-start observation since the latest reset. Populated by FW-7.
struct CycleObservation {
    bool cycleStartObserved{false};
    bool cycleLostObserved{false};
};

// Reset flavor for a role action. Deliberately local to the pure layer (with a
// None case) so Layer 1 does not depend on BusResetCoordinator. The executor
// maps None→no-op and Short/Long→BusResetCoordinator::ResetFlavor.
enum class RoleResetFlavor : uint8_t { None, Short, Long };

// Immutable inputs for ONE evaluation. Carries the generation so the coordinator
// and executors can drop stale results by construction (see RoleCoordinator).
struct RoleInputs {
    uint32_t generation{0};
    const TopologySnapshot* topo{nullptr}; // borrowed; valid only for the call
    RootCapability rootCap{RootCapability::Unknown};
    CycleObservation cycles{};
    bool localCmcCapable{false};         // FW-11: is local OHCI cycle-master-capable
    uint8_t resetRetriesThisTopology{0}; // ping-pong guard input
};

// Value-type decision. The ONLY thing Layer-1 tests assert.
struct RoleAction {
    enum class Kind : uint8_t {
        None,                      // stable / nothing to do
        DeferForEvidence,          // wait for BIB/cycle evidence, then re-evaluate
        EnableLocalCycleMaster,    // local == root and accepted capable (FW-7 path)
        EnableRemoteCycleMaster,   // remote CMC root → CSR STATE_SET CMSTR (FW-10)
        ForceRootAndReset,         // PHY CONFIG force-root + reset (FW-9)
        ClearContenderAndDelegate, // Apple-style delegation (FW-9)
        MarkRootBadOrUnknown,      // record bad/unknown root (no side effect)
    };

    Kind kind{Kind::None};
    uint8_t targetRoot{0};                       // ForceRoot / Delegate / EnableRemote
    RoleResetFlavor reset{RoleResetFlavor::None}; // reset to request, if any
    uint8_t gapCount{0};                          // gap-correction resets
    const char* reason{""};                       // straight into the log line
};

// Pure decision. noexcept, no allocation, no hardware.
[[nodiscard]] RoleAction EvaluateRolePolicy(const RoleInputs& in) noexcept;

// Policy function pointer type so RoleCoordinator can be tested with a synthetic
// policy (a captureless lambda) before FW-9 fills in EvaluateRolePolicy.
using PolicyFn = RoleAction (*)(const RoleInputs&) noexcept;

} // namespace ASFW::Driver::Role
