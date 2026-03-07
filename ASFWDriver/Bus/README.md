# Bus Management Subsystem

## Purpose

The Bus subsystem turns OHCI reset interrupts and Self-ID DMA data into a
validated `TopologySnapshot` that the rest of the driver can trust.

It owns four correctness-critical responsibilities:

- stage bus-reset recovery off the IRQ path;
- validate Self-ID DMA data before topology is rebuilt;
- prevent stale topology reuse after malformed or raced resets;
- issue corrective software resets conservatively when the bus is not yet stable.

## Main Components

### `BusResetCoordinator`

`BusResetCoordinator` is the orchestrator. It does not parse Self-ID data or
compute topology itself; it sequences the steps that must happen in the correct
order.

Current live states:

```cpp
enum class State : uint8_t {
    Idle,
    Detecting,
    WaitingSelfID,
    QuiescingAT,
    RestoringConfigROM,
    ClearingBusReset,
    Rearming,
    Complete,
};
```

There is no `Error` terminal state anymore. Invalid Self-ID or topology data is
handled through explicit recovery reset requests.

### `SelfIDCapture`

`SelfIDCapture` owns the OHCI Self-ID DMA buffer and decodes the captured
quadlets into validated Self-ID sequences.

It now returns typed errors:

```cpp
std::expected<SelfIDCapture::Result, SelfIDCapture::DecodeError>
```

Important failure modes:

- controller error bit set in `SelfIDCount`;
- zero-length or overflowed capture;
- generation mismatch across header / initial register read / double-read;
- invalid quadlet / inverse-quadlet pairs;
- malformed Self-ID sequence structure.

### `TopologyManager`

`TopologyManager` converts validated Self-ID sequences into immutable
`TopologySnapshot` values and returns:

```cpp
std::expected<TopologySnapshot, TopologyBuildError>
```

Invalid input never falls back to `latest_`. A bus reset invalidates the cached
snapshot before recovery starts.

### `BusManager`

`BusManager` computes optional PHY configuration follow-ups:

- cycle-master delegation;
- gap-count correction / optimization.

Those follow-ups are expressed as deferred software reset requests. The
coordinator decides when they are allowed to execute.

## Interrupt Ownership

The subsystem intentionally splits interrupt responsibilities:

- `ControllerCore`
  - logs and diagnoses fault interrupts such as `postedWriteErr`,
    `unrecoverableError`, and `regAccessFail`;
  - dispatches async Tx/Rx interrupts to the async engine;
  - does **not** generically ack bus-reset or Self-ID completion bits.
- `BusResetCoordinator`
  - owns `busReset` mask / unmask sequencing;
  - owns `busReset` clear timing;
  - owns `selfIDComplete` / `selfIDComplete2` consume-and-clear policy.

This keeps the spec-sensitive reset ordering in one place.

## `selfIDComplete2` Semantics

`selfIDComplete2` is not a duplicate bit; it is the durable completion latch.

- OHCI 1.1 §6.1 / Table 6-1 defines `selfIDComplete2` as the secondary
  indication of Self-ID completion.
- OHCI 1.1 §11.5 says both completion bits are set after the controller updates
  the first quadlet of the Self-ID buffer.
- OHCI 1.1 §11.5 also states that `selfIDComplete2` is cleared only through
  `IntEventClear`.
- OHCI 1.1 Annex C.6 explains why it exists: it prevents missed completion
  indications during fast back-to-back resets.

ASFW policy:

- `selfIDComplete` is the transient companion bit;
- `selfIDComplete2` is the sticky, software-cleared-only completion latch;
- decode may begin when `NodeID.iDValid == 1` and either completion bit has been
  latched;
- on a fresh reset edge, stale `selfIDComplete2` state is cleared before the new
  wait cycle begins.

## Reset Flow

High-level recovery order:

1. `OnIrq()` latches `busReset`, `selfIDComplete`, `selfIDComplete2`, and the
   interrupt timestamp.
2. `Detecting`
   - invalidate previous reset-local state;
   - mask `busReset`;
   - clear stale sticky `selfIDComplete2`.
3. `WaitingSelfID`
   - wait for `NodeID.iDValid` and at least one completion indication;
   - decode Self-ID DMA data through `SelfIDCapture`;
   - recover via software reset if capture is invalid or times out.
4. `QuiescingAT`
   - notify async about bus-reset begin;
   - stop / flush AT contexts.
5. `RestoringConfigROM`
   - restore staged Config ROM header / bus options;
   - build validated topology;
   - evaluate delegation / gap-correction requests.
6. `ClearingBusReset`
   - clear `IntEvent.busReset` only after AT contexts are inactive.
7. `Rearming`
   - restore filters;
   - re-arm AT contexts;
   - notify async that bus-reset recovery is complete.
8. `Complete`
   - log metrics;
   - either dispatch a deferred software reset or publish topology for
     discovery.

## Repeated Software Reset Holdoff

ASFW distinguishes two different delays:

- **discovery delay** for slow-booting remote devices;
- **software reset holdoff** after Self-ID completion.

The second rule is normative:

- IEEE 1394-2008 §8.2.1 says applications, the bus manager, and the node
  controller should not issue `SB_CONTROL.request(Reset)` until 2 seconds have
  elapsed after completion of the Self-ID process that follows a bus reset.
- IEEE 1394-2008 §8.4.5.2 describes the gap-count optimization flow that sends a
  PHY configuration packet and then confirms it with a reset.

The spec allows a gap-count confirmation exception. ASFW intentionally does not
exploit that exception yet. It follows the more conservative Linux / Apple
policy and still rate-limits the follow-up software reset to avoid reset storms
on unstable hardware.

Implementation rule:

- `lastSelfIdCompletionNs` marks the point where a stable Self-ID capture has
  been accepted and the generation/topology path is trusted;
- software-triggered follow-up resets are deferred until
  `lastSelfIdCompletionNs + 2 s`.

## Self-ID and Topology Validation Rules

`SelfIDCapture` rejects:

- invalid quadlet / inverse-quadlet pairs;
- generation races;
- malformed sequence enumeration.

`TopologyManager` rejects:

- invalid or missing contiguous node coverage;
- non-empty captures with no root node;
- internal tree validation failures when explicit parent/child information is
  present.

If any of these fail:

- no topology is published;
- no stale snapshot is reused;
- the coordinator requests a corrective software reset.

## Clean-Code Boundaries

This subsystem follows the refactor rules used elsewhere in the driver:

- orchestration in the coordinator;
- parsing in `SelfIDCapture`;
- topology assembly in `TopologyManager`;
- policy in `BusManager`;
- typed internal errors with `std::expected`;
- comments only for invariants, hardware ordering, or normative spec rationale.

The goal is not a clever FSM. The goal is a reset path that is explicit,
defensible, and hostile to stale state.
