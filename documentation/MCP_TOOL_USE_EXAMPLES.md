# ASFW MCP Tool-Use Examples and Return Schemas

Linear: [FW-87](https://linear.app/asfirewire/issue/FW-87/add-mcp-tool-use-examples-and-return-schema-documentation)

Status: Reference examples for the implemented MCP schema layer (FW-78..85).

> **Scope note.** This documents the request parameters and stable result shapes
> an agent should expect. The schema, discovery, and policy layers are
> implemented and unit-tested; the live `callTool` dispatch that serializes these
> shapes is not yet wired (it returns refusals/dry-runs until then — see the MCP
> architecture doc). Field names below match the Swift schema types
> (`ASFWMCP*Request` / `ASFWMCPTransactionResult` / `ASFWMCPPolicyDecision`).
> Audio-layer controls are intentionally out of scope for this phase.

## 1. Parameter conventions

| Field | Meaning |
| --- | --- |
| `nodeId` | 16-bit bus/node id (`bus_id << 6 \| phy_id`), as a decimal integer |
| `generation` | Bus generation the request is pinned to; a mismatch is `staleGeneration` |
| `addressHigh` | High 16 bits of the 48-bit address (decimal) |
| `addressLow` | Low 32 bits of the 48-bit address (decimal) |
| `length` | Block byte count; non-zero, multiple of 4, ≤ 2048 (S400 ceiling) |
| `payload` | Byte array in **bus (big-endian) order** |
| `value` / `expected` / `swap` | Quadlet in **host order**; serialized big-endian onto the bus |
| `verifyReadback` | Issue a verifying read-back after a write |
| `decode` | Request decoded register fields alongside the raw value |

**Endianness.** Quadlet scalar fields (`value`, `expected`, `swap`) are host-order
integers; the driver serializes them big-endian on the wire. Block `payload`
bytes are already in bus order. **Timeouts.** Durations are reported in
microseconds (`durationUsec`); a transaction that exceeds its deadline returns
`status: "timeout"`.

## 2. Stable result shapes

All async transaction tools return `ASFWMCPTransactionResult`:

```json
{
  "kind": "readQuadlet",
  "ok": true,
  "status": "ok",
  "rcode": "complete",
  "generation": 17,
  "durationUsec": 142,
  "correlationId": "c-00123",
  "payload": [0, 0, 4, 0],
  "decoded": null,
  "policy": null
}
```

`status` ∈ `ok | timeout | rcodeError | busReset | compareFailed | denied | dryRun | malformed`.
`policy` is present only on write-capable calls. Canonical outcomes:

| Outcome | `ok` | `status` | Other |
| --- | --- | --- | --- |
| Success | `true` | `ok` | `rcode: "complete"`, `payload` set |
| Timeout | `false` | `timeout` | `rcode: null` |
| Protocol error | `false` | `rcodeError` | `rcode: "conflictError"` / `"typeError"` … |
| Policy denial | `false` | `denied` | `policy.decision`, `policy.reason` |
| Dry run | `false` | `dryRun` | `policy.decision: "dryRunOnly"` |
| Compare failed | `false` | `compareFailed` | CAS only |
| Malformed | `false` | `malformed` | failed schema validation |

A policy decision (`ASFWMCPPolicyDecision`):

```json
{
  "decision": "requiresDeveloperMode",
  "reason": "Writes require developerWriteEnabled mode; current mode is readOnlyDeveloper.",
  "errorCode": "requiresDeveloperMode",
  "requiredMode": "developerWriteEnabled",
  "requiredCapability": null
}
```

`decision` ∈ `allowed | denied | dryRunOnly | requiresDeveloperMode | unsupportedAddressSpace | staleGeneration | unsupportedProtocol`.

## 3. Examples

### 3.1 Read quadlet — `asfw_read_quadlet`

```json
// request
{ "nodeId": 2, "generation": 17, "addressHigh": 65535, "addressLow": 4026531840 }
// result
{ "kind": "readQuadlet", "ok": true, "status": "ok", "rcode": "complete",
  "generation": 17, "durationUsec": 138, "correlationId": "c-1", "payload": [49, 51, 57, 52] }
```

### 3.2 Read block — `asfw_read_block`

```json
// request — read 16 bytes of Config ROM
{ "nodeId": 2, "generation": 17, "addressHigh": 65535, "addressLow": 4026532864, "length": 16 }
// malformed length (not a multiple of 4) ⇒
{ "kind": "readBlock", "ok": false, "status": "malformed", "correlationId": "c-2" }
```

### 3.3 Write quadlet with readback verification — `asfw_write_device_register`

```json
// request
{ "nodeId": 2, "generation": 17, "addressHigh": 65535, "addressLow": 4026535168,
  "value": 305419896, "verifyReadback": true }
// verification mismatch ⇒ ok:false, the readback differs from the written value
{ "kind": "writeQuadlet", "ok": false, "status": "rcodeError", "generation": 17,
  "correlationId": "c-3",
  "decoded": { "verification": { "requested": 305419896, "readback": 0, "verified": false } } }
```

### 3.4 Compare-swap — `asfw_cas_quadlet`

```json
// request
{ "nodeId": 2, "generation": 17, "addressHigh": 65535, "addressLow": 4026532896,
  "expected": 0, "swap": 1 }
// compare failed (current value != expected)
{ "kind": "compareSwap", "ok": false, "status": "compareFailed", "generation": 17, "correlationId": "c-4" }
```

### 3.5 Config ROM read — `asfw_get_config_rom`

Read-only; returns cached bytes plus a parsed summary. Raw bytes are opt-in
(`includeRaw: true`) to keep responses compact.

### 3.6 AV/C inquiry — `asfw_fcp_send_command` (intent `inquiry`/`status`)

```json
// request — status-only, not policy-gated
{ "nodeId": 0, "generation": 17, "addressHigh": 65535, "addressLow": 4026534656,
  "intent": "status", "payload": [1, 24, 0, 255, 255, 255] }
```

### 3.7 FCP control command — `asfw_fcp_send_command_dev` (intent `control`)

Mutating; policy-gated. In `readOnlyDeveloper`:

```json
{ "kind": "writeBlock", "ok": false, "status": "denied",
  "policy": { "decision": "requiresDeveloperMode", "requiredMode": "developerWriteEnabled",
              "reason": "Writes require developerWriteEnabled mode; current mode is readOnlyDeveloper." } }
```

### 3.8 CMP PCR read/write — `asfw_cmp_read_pcr` / `asfw_cmp_write_pcr`

PCR writes are compare-swaps; a stale plug value surfaces as `compareFailed`.
`plug` must be 0..30 or the request is `malformed`.

```json
// write request
{ "nodeId": 0, "generation": 17, "addressHigh": 65535, "addressLow": 4026534660,
  "plug": 0, "expected": 0, "swap": 1073741824 }
```

### 3.9 IRM allocation — `asfw_irm_allocate_channel`

```json
// request — channel 0..63, generation-pinned
{ "channel": 10, "generation": 17, "allocate": true }
// stale generation ⇒
{ "ok": false, "status": "denied",
  "policy": { "decision": "staleGeneration",
              "reason": "Request generation 16 does not match current bus generation 17; re-read topology and retry." } }
```

### 3.10 DICE/TCAT register access — `asfw_dice_read_register` / `asfw_dice_write_register`

Reads are available for capable nodes. Writes are policy-gated and support
readback verification (§3.3 shape). TCAT application blocks use
`asfw_tcat_read_application_block` / `asfw_tcat_write_application_block` with the
same block bounds.

## 4. Acceptance mapping

- Examples for read/write/CAS/Config ROM/AV/C/FCP/CMP/IRM/DICE-TCAT: §3.
- Parameter conventions (ids, generation, address, payload, endian, timeout): §1.
- Stable result shapes for success/timeout/protocol error/policy denial/verification
  mismatch: §2 + §3.3/§3.4/§3.7/§3.9.
- No audio-layer controls: enforced by the §5.x taxonomy scope.
