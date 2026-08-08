# ASFW Runtime Lifecycle Contract

## Purpose

This document is the authority for the ASFWDriver root runtime lifecycle.
Implementation code may refine resource details, but it must not introduce a
second state machine, teardown path, or hardware-legality authority.

See also [TOKEN_BASED_LIFECYCLE.md](../../../documentation/TOKEN_BASED_LIFECYCLE.md) for the token-based routing architecture and device route validity contract.

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
               |          |              `-> Suspended -> Starting
               `-> Failed -> Quiescing

Starting | Running | Failed | Quiescing | Suspended -> Revoked -> Stopped
```

The following transitions are legal:

- `Stopped -> Starting`
- `Starting -> Running`
- `Starting -> Failed`
- `Starting -> Revoked`
- `Running -> Quiescing`
- `Running -> Revoked`
- `Failed -> Quiescing`
- `Failed -> Revoked`
- `Quiescing -> Stopped`
- `Quiescing -> Suspended`
- `Quiescing -> Revoked`
- `Suspended -> Starting`, `Quiescing`, or `Revoked`
- `Revoked -> Stopped`

A request for the current state is idempotent and does not create a new
transition record. Every other transition is rejected without changing state.

`Revoked` dominates all live-state teardown. If provider removal races a
planned quiesce, the coordinator transitions `Quiescing -> Revoked` and skips
every remaining hardware cleanup step.

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
3. Cancel backend/control-plane timers and stop audio/protocol producers.
4. Stop and drain isoch/async contexts while their contract still permits the
   interrupt path to complete required shutdown work.
5. Mask controller interrupts, then cancel and drain the DriverKit interrupt
   source.
6. Perform final register cleanup within an admitted hardware scope.
7. Revoke and drain local MMIO access.
8. Close/release the PCI provider.
9. Release software resources and enter `Stopped`.

The exact context-specific point at which controller interrupts are masked is
validated against the local OHCI and Apple reference implementations; generic
lifecycle code must not assume DMA can always be stopped without interrupts.

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

The coordinator owns a private `StartStage` resource ledger (`None`,
`DependenciesReady`, `QueueReady`, `ProviderOpened`, `InterruptSourceReady`,
`AsyncReady`, `ControllerReady`, `Running`). It records completed bring-up
stages solely to bound failed-start unwind work; it is not a second lifecycle
state machine and cannot admit work.

### Wake rebuild

1. Begin in `Suspended`.
2. Enter `Starting`.
3. Reopen the provider and MMIO gate only after attach succeeds.
4. Rebuild the same runtime pipeline used for cold start.
5. Enter `Running` on success.
6. On failure, use the normal `Starting -> Failed -> Quiescing -> Stopped` path.

## DriverKit completion barriers

Cancellation is not complete merely because cancellation was requested. Final
teardown uses the terminal `Cancel` completion of interrupt, timer, and
notification sources. The source and its OSAction retain their final references
until that completion executes, after all pending handlers return; only then are
they released.

Suspend is different: it uses `SetEnableWithCompletion(false, ...)` and keeps
the source and action alive for wake. The lifecycle state closes work admission
before disable, so a queued callback cannot enter torn-down runtime state.

The coordinator never blocks a dispatch queue waiting for its own completion
work. Completion handlers provide the lifetime barrier asynchronously.

## MMIO rule

No subsystem calls `HardwareInterface::Detach()`.

Only `RuntimeLifecycleCoordinator` may revoke the access gate and close the PCI
provider. Hardware users operate through one admitted `HardwareAccessScope` per
logical batch, not one lock/lease per register operation.

Audio real-time code never performs MMIO and never waits for lifecycle locks. It
consumes published immutable snapshots or atomic timing anchors.

`HardwareAccessScope` is move-only, stack-bound, and synchronous: it must not
be stored, captured by a callback, cross a dispatch boundary, or be held while
waiting. This prevents `RevokeAndDrain()` from waiting on a continuation that
cannot run until revocation has completed.

Route-token validation through `DeviceRegistry` is control-plane work only.
The audio packet path consumes an already-published atomic epoch or immutable
stream binding and never takes the registry lock.

Once `DeviceRegistry` becomes cross-queue synchronized, it exposes snapshots
or controlled access rather than unlocked mutable `DeviceRecord*` values. It
never calls listeners, backend code, nub publication, cancellation, or protocol
shutdown while holding its lock.

## Strict cutover rule

A phase is mergeable only when the superseded authority is deleted in the same
change set. The final branch must not contain both:

- old and new root teardown paths;
- direct and scope-gated public MMIO APIs;
- duplicate runtime admission flags;
- route tokens plus equivalent ad-hoc route-validity authorities.
