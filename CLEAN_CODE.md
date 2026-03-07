# Clean Code Refactor Ledger

This file tracks the architectural cleanup of `ASFWDriver` from the old "make it work"
state toward a maintainable, test-backed, modern C++23 codebase.

It is not a marketing document. It is a working ledger of what was improved, what is still
messy, and what should happen next.

## Goals

- Reduce hidden coupling between subsystems.
- Replace concrete implementation dependencies with narrow ports/interfaces.
- Split large files and "god objects" into smaller, reasoned modules.
- Make async behavior explicit: generation guard, cancellation, callback context, no accidental
  re-entrancy.
- Prefer strong types, `std::span`, `std::expected`, and local ownership over raw pointer-heavy
  patterns.
- Keep low-level unsafe operations isolated instead of spreading them through business logic.
- Keep `clang-format`, `clang-tidy`, host tests, and static analysis active during refactors.

## Definition Of "Clean" For This Repo

For ASFW, "clean code" does not mean abstracting everything.

It means:

- hardware-facing code stays explicit and debuggable;
- protocol/discovery/business logic depends on ports, not on DMA/interrupt internals;
- generation, endianness, and address-space semantics are visible in types or API names;
- callback-driven code is organized as small state machines, not nested lambda webs;
- unsafe casts and DriverKit-specific edge cases are isolated behind wrappers;
- every meaningful refactor ends in a shippable state with tests still passing.

## Completed Refactor Work

### 1. ConfigROM rewrite foundation

Completed on `codex/configrom-rewrite`.

- Rebuilt `ASFWDriver/ConfigROM/` into subfolders by concern instead of one dense blob.
- Split responsibilities into `Parse/`, `Store/`, `Remote/`, `Local/`, and `Common/`.
- Introduced `ConfigROMParser`, `ROMReader`, `ROMScanner`, `ConfigROMStore`,
  `ConfigROMBuilder`, and `ConfigROMStager` as clearer units.
- Added `MemoryMapView` so Config ROM memory-map address-to-pointer conversion is isolated in one
  place instead of leaking `void*` and integer-to-pointer casts through core logic.
- Replaced old poll-style discovery with callback-based scan completion.
- Modularized `ROMScanSession` across multiple translation units.
- Reworked parser APIs toward safer, more explicit behavior and added detail-discovery tests.

### 2. Tooling from day 1

- Added repo-level `.clang-format`.
- Added repo-level `.clang-tidy` with `clang-analyzer-*`, `bugprone-*`, `performance-*`,
  `modernize-*`, and `readability-*` checks.
- Added `tools/quality/format.sh`.
- Added `tools/quality/tidy.sh`.
- Added `tools/quality/analyze.sh`.
- Added `tools/quality/all.sh`.

This changed the workflow from "clean up later" to "refactor with a quality floor in place".

### 3. Async contract reality fix

Checkpoint commit: `f786385`.

- Enforced generation gating at the async bus adapter boundary.
- Implemented real cancel semantics instead of stub behavior.
- Locked down "callback exactly once" and "no inline completion on submit/cancel path" as the
  actual contract.
- Added host tests for stale-generation behavior and cancellation guarantees.

This was a necessary prerequisite for any future async cleanup or coroutine layer.

### 4. Dependency inversion in protocol code

Checkpoint commit: `3da0b51`.

- Protocol code was moved off direct `AsyncSubsystem` dependency.
- `Protocols` now use `IFireWireBusOps` / `IFireWireBusInfo` style ports instead of reaching into
  async engine internals.
- This separated protocol logic from transport engine implementation details.

### 5. Discovery generation type cleanup

Checkpoint commit: `7a78c38`.

- Unified discovery generation handling onto `ASFW::FW::Generation`.
- Reduced raw `uint16_t` generation leakage.
- Made generation semantics more explicit across discovery/controller paths.

### 6. Mechanical god-file splits

Checkpoint commit: `21e2355`.

- Split `AsyncSubsystem.cpp` into lifecycle, interrupts, bus-reset, command-queue, and diagnostics
  translation units.
- Split `ControllerCore.cpp` into lifecycle, interrupts, discovery, and facade translation units.
- Split `BusResetCoordinator.cpp` into FSM/actions-style translation units.

This did not solve all design debt, but it made the system easier to inspect and change safely.

### 7. Async Rx decoupling for FCP response routing

Checkpoint commit: `ef3f165`.

- `Protocols/AVC/FCPResponseRouter.hpp` no longer depends on `PacketRouter`, `ARPacketView`,
  `PacketHelpers`, or async response-code internals.
- Introduced a small protocol-facing receive port type and kept adaptation in the composition
  root.

This is the pattern to repeat elsewhere: infrastructure adapts, higher layers stay clean.

## Current WIP On `codex/stack-despaghetti`

The working tree currently contains the next step of the same cleanup:

- added `ASFWDriver/Async/Interfaces/IAsyncSubsystemPort.hpp`;
- made `AsyncSubsystem` implement that narrow port;
- refactored `UserClient` transaction/status/bus-reset handlers to depend on the port instead of
  `AsyncSubsystem.hpp`;
- updated host test stubs to satisfy the expanded virtual surface.

Validation status for this WIP:

- `./build.sh --test-only --no-bump` passes.
- `tools/quality/analyze.sh` succeeds.
- Analyzer still reports the pre-existing retain-count warnings in `ASFWDriver.cpp` around the
  isoch queue memory handoff paths.

This WIP has not yet been checkpoint-committed at the time of writing.

## What Was Actually Improved

The refactor already removed real technical debt, not just moved files around.

- ConfigROM is no longer a single opaque subsystem with mixed parsing, storage, remote I/O, and
  local staging concerns.
- Async now has a defined contract instead of implied behavior.
- Protocol code is less coupled to transport-engine details.
- Some previously dense callback logic is now localized into explicit state-machine code.
- Generation handling is less error-prone.
- Quality tooling is part of the workflow instead of an afterthought.

## What Is Still Not Clean

The major remaining debt is architectural coupling, not formatting.

### 1. Ports cleanup is incomplete

- `UserClient` is being decoupled now, but other consumers still reach for concrete async
  implementation details.
- `DriverContext`, controller facades, and some service-layer glue still know too much about
  infrastructure objects.

### 2. Shared boundaries are still blurry

`ASFWDriver/Shared/` is useful, but currently too broad.

- Some contents are truly cross-cutting DMA/ring primitives.
- Some contents are infrastructure conveniences that probably belong closer to `Async/` or
  `Isoch/`.
- "Shared" is acceptable only for low-level reusable building blocks with clear ownership and
  stable semantics.
- "Shared" is not acceptable as a dumping ground for code with unclear subsystem identity.

### 3. Some classes are still too powerful

Large classes/files were split, but responsibility concentration still exists.

- `AsyncSubsystem` is still the largest conceptual object in the driver.
- `ControllerCore` still acts as a major orchestration hub.
- `BusResetCoordinator` still carries a lot of timing/FSM policy.

These are better than before, but not yet minimal.

### 4. Async API surface is still mixed-level

- `IFireWireBusOps` is a good transport-facing port.
- `AsyncSubsystem` still exposes lower-level engine concerns alongside higher-level operations.
- Some code paths still operate on raw async parameter structs instead of stronger intention-level
  commands.

### 5. Strong typing is incomplete

- Generation typing improved.
- ConfigROM offset/count typing improved.
- There is still raw integer traffic for addresses, offsets, node IDs, status codes, and some
  wire values.

### 6. A few legacy smells remain

- Some user-client flows still mirror older low-level transaction APIs too directly.
- Some comments explain history instead of current invariants.
- A few analyzer warnings remain outside the immediate cleanup scope.

## Refactor Strategy Going Forward

Do not do a full rewrite of the whole driver again.

The current codebase is already working hardware-backed code. The right move is bounded
checkpoint refactoring:

1. Finish port completion.
   Keep removing direct `AsyncSubsystem.hpp` dependencies from higher layers.

2. Tighten composition-root boundaries.
   `ASFWDriver.cpp` and `DriverContext` should wire objects together, not leak transport details
   outward.

3. Audit `Shared/`.
   Move files that are not genuinely cross-subsystem primitives into clearer homes.

4. Continue splitting conceptual god objects.
   Prefer small helper types and translation units over adding more methods to central classes.

5. Strengthen types where mistakes are expensive.
   Focus on generation, node identity, offsets, counts, endian-tagged values, and status/result
   transport.

6. Keep coroutine work optional and late.
   Coroutines are only worth adding after contracts and ports are stable.

## Concrete TODO

- Finish the `UserClient` port decoupling checkpoint and commit it.
- Continue removing `AsyncSubsystem.hpp` from non-infrastructure consumers.
- Introduce ports for async diagnostics / capture access where direct concrete access still leaks.
- Review `ASFWDriver/Shared/` and classify each file as:
  `truly shared`, `belongs to Async`, `belongs to Isoch`, or `should be deleted`.
- Reduce direct use of raw async parameter structs in upper layers where stronger wrappers make
  intent clearer.
- Fix or explicitly isolate the remaining analyzer warnings in `ASFWDriver.cpp`.
- Continue documenting architectural decisions while the refactor is fresh, instead of after the
  fact.

## Anti-Goals

These are explicitly not the current plan:

- rewriting the driver from scratch again;
- introducing coroutines deep in interrupt or DMA hot paths;
- abstracting hardware semantics until the code becomes unreadable;
- doing repo-wide type redesign in one jump;
- moving fast and breaking hardware behavior.

## Exit Criteria For This Cleanup Campaign

This campaign can be considered structurally successful when:

- protocol/discovery/user-client code depend on ports, not async implementation internals;
- async semantics are test-backed and stable;
- ConfigROM stays clean under ongoing changes;
- `Shared/` contains only truly shared low-level building blocks;
- major orchestration classes are smaller and easier to reason about;
- new features can be added without reopening the same architectural knots.

