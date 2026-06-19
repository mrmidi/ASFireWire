# ASFW MCP Test Gate

Linear: [FW-89](https://linear.app/asfirewire/issue/FW-89/add-swift-mcp-test-gate-before-agent-enablement)

Status: Initial fail-closed Swift test gate.

## 1. Goal

MCP agent access to real ASFW driver/hardware must remain disabled unless the
Swift MCP test gate passes. The gate exists so the MCP control plane cannot
accidentally expose write-capable or hardware-backed behavior before discovery,
resource envelopes, and mutation guards are test-covered.

## 2. Implementation

The initial gate is implemented by:

- `ASFW/MCP/ASFWMCPTestGate.swift`
- `ASFWTests/MCP/MCPTestGateTests.swift`

It evaluates an `ASFWMCPCore` instance and returns an
`ASFWMCPTestGateResult` with named checks. The app/host layer should treat
`ASFWMCPTestGate.allowsRealAgentHardwareAccess(_:)` as the mandatory predicate
before enabling real agent/hardware access.

## 3. Current Checks

The initial gate verifies:

- enabled MCP modes expose tools
- enabled MCP modes expose resources
- write-capable tools stay hidden unless write policy and Swift test gates are
  open
- required first resources are present
- required always-visible tools are present
- telemetry snapshots use a stable envelope
- default hardware smoke plans contain no mutating operations

The gate is fail-closed. Disabled mode does not pass real-access enablement.

## 4. Test Coverage

`MCPTestGateTests` covers:

- disabled mode fails closed for real access
- read-only mode passes the gate and hides write tools
- developer-write mode remains closed when policy or test gate state is missing
- developer-write mode lists policy-gated writes only after gates pass
- no-device fixtures do not expose AV/C, CMP, DICE/TCAT, or SBP-2 tools
- protocol fixtures expose AV/C, CMP, DICE/TCAT, SBP-2, and IRM/CAS read
  surfaces
- hardware smoke plans with mutation fail the gate

`MCPMockHarnessTests` continues to cover the underlying mock transport,
resource envelopes, and hardware smoke plan invariants.

## 5. Running The Gate Tests

```bash
./build.sh --swift-test-only --no-bump
```

This test flow is hardware-free.

## 6. Later Tightening

FW-79 should extend the gate with concrete write-policy checks:

- every policy decision is covered
- denied writes do not reach the driver/user-client write path
- dry-run writes do not reach the driver/user-client write path
- write results include machine-readable policy reasons

Real hardware smoke tests remain opt-in and must not run mutating operations
unless explicitly enabled.
