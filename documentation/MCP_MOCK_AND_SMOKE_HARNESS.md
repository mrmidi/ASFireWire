# ASFW MCP Mock and Smoke Harness

Linear: [FW-90](https://linear.app/asfirewire/issue/FW-90/build-mcp-mock-transport-and-hardware-smoke-test-harness)

Status: Initial executable harness.

## 1. Goal

Provide a hardware-free MCP test harness for the ASFW MCP control plane, plus a
safe plan shape for future opt-in hardware smoke tests.

This is not the real HTTP/SSE MCP server. It is an in-process harness that lets
the Swift test suite exercise:

- runtime-mode-based tool discovery
- hidden write-capable tools in read-only mode
- stable telemetry resource envelopes
- mock node and transaction resources
- default hardware smoke plans with no mutating operations

## 2. Files

Production-side pure Swift harness:

- `ASFW/MCP/ASFWMCPModels.swift`
- `ASFW/MCP/ASFWMCPDriverControl.swift`
- `ASFW/MCP/ASFWMCPCore.swift`
- `ASFW/MCP/ASFWMCPMockTransport.swift`

Tests:

- `ASFWTests/MCP/MCPMockHarnessTests.swift`

These files intentionally avoid importing the MCP Swift SDK. They model the core
behavior that `ASFWMCPHost` will later expose over app-hosted local HTTP/SSE.

## 3. Current Harness Shape

```text
ASFWMCPMockTransport
      ↓
ASFWMCPCore
      ↓
ASFWDriverControlling
      ↓
MockASFWDriverControl
```

The mock driver returns deterministic fixtures:

- two nodes: one AV/C/CMP-like node and one DICE/TCAT-like node
- one recent async transaction event
- a compact telemetry snapshot

No IOKit, DriverKit, FireWire hardware, or live MCP networking is used.

## 4. Runtime Discovery Covered

The current tests verify:

- mock mode lists always-visible and read tools
- read-only developer mode hides write-capable tools
- developer-write mode lists writes only after write policy and Swift test gates
  are marked available
- raw OHCI write tools stay hidden unless raw developer tier is enabled
- telemetry and nodes resources use stable envelopes and mock data
- the mock path records no unexpected write attempt

## 5. Hardware Smoke Plan

`ASFWMCPHardwareSmokeHarness.defaultPlan()` returns a read-only smoke plan by
default. It includes only:

- read telemetry snapshot
- read controller state
- list nodes
- read recent transactions

The default plan must never mutate hardware.

Mutating steps are included only when explicitly requested:

```swift
let plan = ASFWMCPHardwareSmokeHarness.defaultPlan(includeMutatingOperations: true)
```

Any mutating step must have:

- `mutatesHardware == true`
- `requiresExplicitEnablement == true`

Future hardware-backed tests must stay opt-in and must not run mutating
operations unless the developer explicitly enables them.

## 6. Running The Tests

```bash
./build.sh --swift-test-only --no-bump
```

The initial harness is covered by `MCPMockHarnessTests` and does not require
FireWire hardware.

## 7. Boundaries

This harness does not implement:

- real MCP SDK server wiring
- app-hosted HTTP/SSE
- live `ASFWDriverConnector` access
- write policy execution
- hardware-backed smoke tests

Those belong to later slices. FW-90 only makes the MCP design executable in
mock form and establishes the safe default shape for future hardware tests.
