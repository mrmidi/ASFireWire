#pragma once

// RoleCoordinator.hpp — Layer 2 of the RoleCoordinator design (FW-6).
//
// Thin, stateful role-policy actor. It owns ONLY:
//   - evidence accumulated for the current bus generation, and
//   - the ping-pong guard (topology fingerprint + same-topology reset count).
// It is NOT a state machine: on each event it rebuilds an immutable RoleInputs
// snapshot, calls the pure EvaluateRolePolicy, and dispatches the resulting
// RoleAction to injected executors. A new bus reset is a clean slate by
// construction. See the FW-6 design comment in Linear.
//
// SKELETON (FW-6): executors default to nullptr (no-op) and the coordinator is
// not yet wired into the live BusResetCoordinator FSM — it is exercised only by
// host tests. The live notification call sites and executor adapters land with
// FW-7/FW-8/FW-9.

#include <cstdint>

#include "RolePolicy.hpp"

namespace ASFW::Driver::Role {

// Layer 3 boundary: executors are injected interfaces so tests use fakes and the
// live driver supplies adapters over BusManager (PHY/reset/contender) and the
// async layer (remote CSR STATE_SET CMSTR). The coordinator performs no hardware
// access itself.
struct IPhyConfigReset {
    virtual ~IPhyConfigReset() = default;
    virtual void ForceRootAndReset(uint8_t targetRoot, RoleResetFlavor flavor, uint8_t gapCount,
                                   uint32_t generation) = 0;
};

struct IRemoteCsrWriter {
    virtual ~IRemoteCsrWriter() = default;
    virtual void EnableRemoteCycleMaster(uint8_t rootNodeId, uint32_t generation) = 0;
};

struct IContenderControl {
    virtual ~IContenderControl() = default;
    virtual void EnableLocalCycleMaster(uint32_t generation) = 0;
    virtual void ClearLocalContenderAndDelegate(uint8_t targetRoot, uint32_t generation) = 0;
};

// Injected executor set. Defined at namespace scope (not nested) so its default
// member initializers are usable as a default argument / member of RoleCoordinator.
struct RoleExecutors {
    IPhyConfigReset* reset{nullptr};
    IRemoteCsrWriter* csr{nullptr};
    IContenderControl* contender{nullptr};
};

class RoleCoordinator {
  public:
    using Executors = RoleExecutors;

    // Linux bm_retries: stop issuing role-policy resets after this many on one
    // physical topology, then log a stable failure instead of looping forever.
    static constexpr uint8_t kMaxSameTopologyResets = 5;

    explicit RoleCoordinator(Executors executors = {}, PolicyFn policy = &EvaluateRolePolicy)
        : executors_(executors), policy_(policy) {}

    // ---- Events (the boundary BusResetCoordinator/FW-8/FW-7 will drive) -------
    // OnTopologyChanged starts a fresh generation: evidence is cleared, and the
    // ping-pong retry counter resets only when the physical topology changed.
    void OnTopologyChanged(uint32_t generation, const TopologySnapshot& topo);
    void OnRootCapability(uint32_t generation, RootCapability verdict);
    void OnCycleStartEvidence(uint32_t generation, CycleObservation obs);
    // Reserved for FW-9 to correlate an issued reset with its completion.
    void OnResetComplete(uint32_t generation);

    // ---- Test / diagnostic accessors -----------------------------------------
    [[nodiscard]] uint32_t Generation() const noexcept { return generation_; }
    [[nodiscard]] RoleAction LastAction() const noexcept { return lastAction_; }
    [[nodiscard]] uint8_t ResetRetriesThisTopology() const noexcept { return resetRetries_; }
    [[nodiscard]] bool HaveTopology() const noexcept { return haveTopology_; }

  private:
    void Reevaluate();
    void Dispatch();
    [[nodiscard]] static uint64_t TopologyFingerprint(const TopologySnapshot& topo) noexcept;

    Executors executors_{};
    PolicyFn policy_{&EvaluateRolePolicy};

    // Evidence for the current generation.
    uint32_t generation_{0};
    bool haveTopology_{false};
    TopologySnapshot topo_{};
    RootCapability rootCap_{RootCapability::Unknown};
    CycleObservation cycles_{};
    bool localCmcCapable_{false};

    // Ping-pong guard.
    uint64_t topologyFingerprint_{0};
    bool haveFingerprint_{false};
    uint8_t resetRetries_{0};

    RoleAction lastAction_{};
};

} // namespace ASFW::Driver::Role
