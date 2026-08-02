# AV/C Control-Plane Contract

Status: FW-99 design and TDD baseline. This is the implementation contract
for FW-100 through FW-104 and the BeBoB backend epic. It describes observable
behavior and ownership, not a source-code port.

## Scope and layer boundary

ASFW is an AV/C **initiator** for remote FireWire audio interfaces. The
control plane owns remote AV/C commands over FCP, remote CMP/PCR connection
leases, and device-family profiles. Local AV/C target/responder support, a
local FCP command region, and local PCR CSR target space are out of scope.

```
Audio profile / AV-C command model
      -> FCP engine + remote CMP connection leases
      -> neutral IRM reservation + fully framed isoch packets
      -> payload-opaque FireWire transport
```

FCP/CMP code must not know CoreAudio buffer ownership or OHCI descriptor
mechanics. Audio code must not mutate remote PCRs directly. IRM owns global
channel/bandwidth allocation and passes an immutable reservation to CMP.

## Clean-room policy and behavioral references

Linux, FFADO, ALSA userspace protocol code, and Apple's IOFireWireAVC tree are
behavioral references only. No implementation may be copied or mechanically
translated. Every wire-visible decision records a concise `path:line`
provenance entry in its test or design note, plus a fresh explanation.

| Concern | Behavioral reference |
| --- | --- |
| FCP response registration, matching, interim, reset | `references/linux-sound-firewire-stack/firewire/fcp.c:186-398` |
| Remote CMP connection ownership and PCR mutation | `references/linux-sound-firewire-stack/firewire/cmp.c:73-367` |
| Write-complete timeout arming and async cancellation | `references/IOFireWireAVC/IOFireWireAVC/IOFireWireAVCCommand.cpp:67-95`, `IOFireWireAVCUnit.cpp:1462-1525` |
| BeBoB formation, ordering, and readiness | `references/linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:377-683` |

## Session ownership and lifecycle

`RemoteAvcSession` is the future logical owner for one remote GUID. It may own
FCP and CMP children, but never spans different GUIDs. Node ID and generation
are an epoch-bound route, not device identity.

| Event | Required behavior |
| --- | --- |
| Discovery / resume | Resolve GUID to current node and generation before issuing work. |
| Command write success | Capture the route used for that command, then arm its response timeout. |
| Bus suspend/reset | Reject new old-epoch work; resolve every outstanding operation exactly once; invalidate CMP leases and isoch reservations. An explicitly idempotent FCP request may remain pending only until discovery revalidates its GUID/node/generation route; it never replays directly from the reset edge. |
| Resume | Re-discover capabilities and reconstruct reservations/connections through the profile. Never reuse a prior-generation PCR lease. |
| Removal / shutdown | Quiesce host stream, cancel FCP timers/operations, drop routing leases, then release profile/buffer state. |

Response routing holds a strong session/transport lease through dispatch. A
node-ID map may accelerate lookup but cannot return an unprotected raw pointer
across a lock boundary.

## FCP transaction contract

An FCP request has immutable command bytes, an explicit response predicate,
GUID/node/generation route snapshot, command replay class, cancellation token,
single completion gate, and final/interim timeout policy.

```
Queued -> WritePending -> AwaitingResponse -> Interim -> AwaitingResponse
   |           |                 |                |
   +-----------+-----------------+----------------+--> Completed / Cancelled / Reset
```

Rules:

1. Start response timing only after a successful write completion.
2. Match response route and a command-defined predicate; subunit/opcode alone
   is not a general identity.
3. Interim never completes a command. A profile chooses bounded caller-facing
   waiting policy without permitting duplicate completion.
4. Reset never silently replays state-changing work. Replay requires an
   explicit idempotent class and a revalidated route.
5. Timers are cancellable scheduler work, never sleeping queue workers.
6. The FCP handler accepts the valid response range; transport configuration
   and routing configuration must agree.

The engine may serialize requests initially. Adding concurrency requires an
independently discriminating predicate per outstanding request; a constant
transaction handle or implicit single pending slot is not acceptable.

## Remote CMP/PCR lease contract

`RemoteCmpConnection` owns one `(GUID, direction, PCR index)` remote lease and
is the sole mutator of its PCR. Before establish it reads MPR/PCR, validates
index/capability, and rejects offline, broadcast, or externally used plugs. It
re-reads and revalidates after CAS contention.

On establish, the lease records its epoch, channel, reserved bandwidth,
effective speed, PCR snapshot, and connected state. oPCR programming includes
speed/overhead derived from the reservation. Only an owned current-epoch lease
may break a connection. Reset invalidates a lease; it never proves a later PCR
is ASFW-owned.

There is one canonical PCR/MPR wire representation. The incompatible legacy
`PCRSpace`/`AVCDefs` layouts cannot be used for remote CMP mutation.

## Profile boundary

Profiles own behavior that is not generic: response-code quirks, plug indexes,
format/clock/settle/re-query recipe, formation/channel-map interpretation,
start order, dual-CMP requirement, readiness/DBC rules, and vendor controls.
The FCP/CMP engines expose typed outcomes and never special-case a family.

## TDD conformance matrix

The following test suites are required before their production behavior is
enabled. Host fakes cover deterministic behavior; hardware supplements them.

| Work | Target / fixture | Required cases |
| --- | --- | --- |
| FW-100 FCP | `FCPTransportTests` with fake bus/scheduler | write failure; timer after write; final/interim; cancellation; reset; source/generation mismatch; late response; teardown; response range; queue policy |
| FW-101 CMP | `CMPConnectionTests` with scripted MPR/PCR/CAS bus | canonical bit vectors; invalid plug; offline/broadcast/foreign use; CAS contention; owned break; reset invalidation; two GUIDs |
| FW-102 model | codec/parser fixtures | malformed frames; plug/format vectors; rate/channel formation; no default stereo fallback |
| FW-103 OXFW | profile state-machine fixtures | format change, settle/re-query, rollback, start/stop/recovery |
| FW-105+ BeBoB | BridgeCo codec/model/profile fixtures | address encoding, formation/channel map, clock/timing quirks, Phase 88 evidence vectors |
| FW-104 / FW-111 | hardware matrix | cold discovery, one-way/duplex, rate/clock change, active reset, teardown, repeated restart |

Each test asserts one externally observable invariant. Test names describe the
event and expected result, not a private implementation method.

## Hardware evidence

Batch hardware runs after the relevant fixture suite passes. Capture config-ROM
identity, command/response bytes, generation, and anomaly-only driver logs.
For TerraTec PHASE 88 Rack FW, reported stream formation determines published
channel counts; reference rates, clock sources, and routing remain hypotheses
until observed on the actual unit.
