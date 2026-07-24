# ASFW Runtime Lifecycle Contract

## Purpose

This document is the authority for the ASFWDriver root runtime lifecycle.
Implementation code may refine resource details, but it must not introduce a
second state machine, teardown path, or hardware-legality authority.

## Ownership

| Concern | Sole authority | Notes |
|---|---|---|
| DriverKit service incarnation | `IOService` / I/O Registry | `Start`, `Stop`, `Terminate`, provider and child lifetime |
| ASFW root runtime state | `ControllerStateMachine` | All admission decisions derive from this state |
| Runtime transition and teardown ordering | `RuntimeLifecycleCoordinator` | The only entry point for start, stop, suspend, revoke, failed start, and wake rebuild |
| Local OHCI MMIO legality | `HardwareAccessGate` | No MMIO outside an admitted batch scope |
| FireWire bus generation | `GenerationTracker` | Authoritative generation from controller/Self-ID flow |
| Remote device/route validity | `DeviceRegistry` | Issues and validates `DeviceRouteToken` values |
| Backend logical operation | Owning backend | Operation serial, exactly-once completion, rollback, restart |

## Legal state graph

```text
Stopped -> Starting -> Running -> Quiescing -> Stopped
                       |             `------> Suspended
                       `-> Revoked -> Stopped
Starting -> Failed -> Quiescing -> Stopped
Suspended -> Starting
```

The following transitions are legal:

- `Stopped -> Starting`
- `Starting -> Running`
- `Starting -> Failed`
- `Running -> Quiescing`
- `Running -> Revoked`
- `Failed -> Quiescing`
- `Quiescing -> Stopped`
- `Quiescing -> Suspended`
- `Suspended -> Starting`
- `Revoked -> Stopped`

A request for the current state is idempotent and does not create a new
transition record. Every other transition is rejected without changing state.

`Reset()` is reserved for construction/test reuse when no runtime resources are
live. Runtime code must reach `Stopped` through legal transitions.

## Admission rules

- Only `Running` admits new normal control, async, isoch, discovery, or backend work.
- `Starting` admits only coordinator-owned bring-up work.
- `Quiescing`, `Suspended`, `Revoked`, `Failed`, and `Stopped` reject new work.
- Resource booleans may describe allocation state, but they must not override
  or compete with `ControllerStateMachine` for admission decisions.

## Single coordinator rule

All of these DriverKit paths delegate to `RuntimeLifecycleCoordinator`:

- driver `Start` / runtime start;
- driver `Stop`;
- power suspend and resume;
- provider termination notification;
- failed-start cleanup;
- wake verification and rebuild.

Duplicate stop, revoke, or failed-start requests are idempotent coordinator
requests. They must not execute independent teardown bodies.

`ASFWDriver::QuiesceRuntime`, `DriverWiring::CleanupStartFailure`, and
`ControllerCore::Stop` must not remain competing hardware teardown authorities
after the root cutover.

## Quiesce phase order

### Common admission closure

1. Request the legal state transition.
2. Close all new producer admission.
3. Invalidate remote route tokens.
4. Reject new backend operations.

### Planned stop

1. Enter `Quiescing` from `Running`.
2. Close producers.
3. Cancel and drain DriverKit callback sources.
4. Stop audio control activity.
5. Stop isoch DMA.
6. Stop async DMA.
7. Perform final register cleanup within an admitted hardware scope.
8. Revoke and drain local MMIO access.
9. Close/release the PCI provider.
10. Enter `Stopped`.

### Suspend

Use the planned-stop ordering while the provider is still valid, but preserve
only the DriverKit objects explicitly required for a safe resume. Finish in
`Suspended`, never in an independent `runtimeSuspended` authority.

### Provider revocation

1. Enter `Revoked` from `Running`.
2. Close producers.
3. Revoke and drain local MMIO immediately.
4. Cancel and drain DriverKit callback sources.
5. Tear down software state without final register cleanup.
6. Release provider resources.
7. Enter `Stopped`.

No operation after step 3 may assume that OHCI registers respond.

### Start failure

1. Enter `Failed` from `Starting`.
2. Enter `Quiescing` through the coordinator.
3. Cancel/drain only resources successfully created so far.
4. If hardware is still valid, perform the applicable bounded cleanup.
5. Revoke MMIO and release the provider.
6. Enter `Stopped`.

There is no independent `CleanupStartFailure` teardown implementation.

### Wake rebuild

1. Begin in `Suspended`.
2. Enter `Starting`.
3. Reopen the provider and MMIO gate only after attach succeeds.
4. Rebuild the same runtime pipeline used for cold start.
5. Enter `Running` on success.
6. On failure, use the normal `Starting -> Failed -> Quiescing -> Stopped` path.

## DriverKit completion barriers

Cancellation is not complete merely because cancellation was requested. The
coordinator composes completion-bearing teardown operations for interrupt,
timer, notification, and action sources.

`super::Stop()` may run only after the final barrier proves that no callback can
still target or retain the service.

The coordinator must never block a dispatch queue while waiting for completion
work scheduled to that same queue. Barrier composition must be asynchronous or
executed from a context that can safely wait.

## MMIO rule

No subsystem calls `HardwareInterface::Detach()`.

Only `RuntimeLifecycleCoordinator` may revoke the access gate and close the PCI
provider. Hardware users operate through one admitted `HardwareAccessScope` per
logical batch, not one lock/lease per register operation.

Audio real-time code never performs MMIO and never waits for lifecycle locks. It
consumes published immutable snapshots or atomic timing anchors.

## Strict cutover rule

A phase is mergeable only when the superseded authority is deleted in the same
change set. The final branch must not contain both:

- old and new root teardown paths;
- direct and scope-gated public MMIO APIs;
- duplicate runtime admission flags;
- route tokens plus equivalent ad-hoc route-validity authorities.
