# ASFW MCP Telemetry Resources

Linear: [FW-86](https://linear.app/asfirewire/issue/FW-86/define-agent-friendly-telemetry-resources)

Status: Accepted resource model for the first MCP control-plane design pass.

## 1. Goal

Define compact MCP resources for ASFW telemetry so agents can inspect the driver,
bus, protocol, and transaction state without parsing text logs.

The resource model should be:

- stable enough for programmatic agents
- compact by default
- bounded in size
- generation-aware
- suitable for before/after comparison
- able to link to raw detail without returning large dumps by default

Logs remain useful supporting evidence, but they are not the primary telemetry
interface.

## 2. Source Material

The first resource model should be built from existing ASFW app and driver
surfaces:

- `ASFWDiagnosticsClient` generation-consistent snapshots
- `ASFWDiag*` structs from `ASFWDriver/Shared/ASFWDiagnosticsABI.h`
- `ASFWDriverConnector` status, topology, Config ROM, discovery, transaction,
  AV/C, FCP, IRM/CMP, and version calls
- existing bounded diagnostics limits:
  - `ASFW_DIAG_MAX_NODES = 64`
  - `ASFW_DIAG_MAX_PORTS = 27`
  - `ASFW_DIAG_MAX_SELF_ID_QUADS = 256`
  - `ASFW_DIAG_MAX_ASYNC_EVENTS = 128`
  - `ASFW_DIAG_MAX_CSR_ENTRIES = 32`
  - `ASFW_DIAG_MAX_PHY_REGS = 16`

Resource handlers should reuse ASFW's existing consistency behavior where
possible. In particular, multi-struct snapshots should retry on stale generation
and return a structured error if a stable snapshot cannot be collected.

## 3. Common Envelope

Every MCP telemetry resource should return a JSON object with a common envelope:

```json
{
  "schema": "asfw.telemetry.controller_state.v1",
  "uri": "asfw://controller/state",
  "snapshotId": "mcp-00000042",
  "capturedAt": "2026-06-18T16:55:00Z",
  "monotonicNs": 123456789000,
  "generation": 17,
  "driverConnected": true,
  "stale": false,
  "truncated": false,
  "data": {}
}
```

Common fields:

| Field | Purpose |
| --- | --- |
| `schema` | Stable schema identifier with version suffix. |
| `uri` | Resource URI served. |
| `snapshotId` | MCP-side correlation ID for comparing resources collected together. |
| `capturedAt` | Wall-clock timestamp when available. |
| `monotonicNs` | Driver/app monotonic timestamp where available. |
| `generation` | Bus generation for generation-scoped resources. |
| `driverConnected` | Whether live driver access was available. |
| `stale` | True when the data is known stale or collected across a generation mismatch. |
| `truncated` | True when arrays/raw data were bounded. |
| `data` | Resource-specific payload. |
| `links` | Optional related resource URIs. |
| `errors` | Optional structured recoverable errors. |

Structured errors should use the FW-77 vocabulary where possible:

- `driverNotConnected`
- `capabilityUnavailable`
- `capabilityChanged`
- `staleGeneration`
- `busResetDuringOperation`
- `payloadTooLarge`
- `rawDataOmitted`

## 4. Include Levels

Resources default to summaries. Large/raw data is opt-in.

Recommended query parameters:

| Parameter | Values | Meaning |
| --- | --- | --- |
| `includeRaw` | `false`/`true` | Include raw bytes/quadlets where supported. Default false. |
| `limit` | positive integer | Bound returned arrays. Default resource-specific. |
| `sinceSnapshotId` | string | Return deltas where supported. |
| `generation` | integer | Require a specific bus generation or return stale/capability error. |
| `nodeId` | integer | Filter node-scoped aggregate resources. |
| `protocol` | string | Filter protocol aggregate resources. |

If raw data is omitted, return `rawDataOmitted` as metadata rather than silently
pretending the data does not exist.

## 5. Resource Index

These resources form the first telemetry surface:

| URI | Purpose |
| --- | --- |
| `asfw://telemetry/snapshot` | Compact cross-system overview for agents. |
| `asfw://controller/state` | Controller, driver, and link state. |
| `asfw://driver/version` | Driver/app version and ABI summary. |
| `asfw://bus/topology` | Current topology and node graph. |
| `asfw://bus/self-id/latest` | Latest Self-ID capture summary. |
| `asfw://bus/resets/recent` | Recent bus reset history. |
| `asfw://nodes` | Node summaries. |
| `asfw://nodes/{nodeId}/summary` | Single node summary. |
| `asfw://nodes/{nodeId}/config-rom` | Cached Config ROM summary, raw optional. |
| `asfw://devices` | Device and unit discovery state. |
| `asfw://transactions/recent` | Recent async transaction events. |
| `asfw://irm/state` | IRM and bus manager state. |
| `asfw://irm/channels` | Channel availability/allocations where known. |
| `asfw://irm/bandwidth` | Bandwidth availability/allocations where known. |
| `asfw://protocols/avc/units` | AV/C unit and subunit summaries. |
| `asfw://protocols/fcp/recent` | Recent FCP command/response summaries. |
| `asfw://protocols/cmp/plugs` | CMP plug/PCR state. |
| `asfw://protocols/cmp/connections` | Known CMP connection state. |
| `asfw://protocols/sbp2/units` | SBP-2 unit summaries. |
| `asfw://protocols/sbp2/sessions` | SBP-2 session/fetch-agent state where available. |
| `asfw://protocols/dice/state` | DICE/TCAT low-level state summary. |
| `asfw://protocols/dice/registers` | Decoded known DICE register cache/snapshot. |
| `asfw://protocols/tcat/application` | TCAT application-space summary. |
| `asfw://logs/recent` | Bounded recent structured app/driver log entries. |

## 6. Cross-System Snapshot

URI: `asfw://telemetry/snapshot`

Purpose: one compact overview suitable for an agent's first read and for
before/after comparisons.

Default payload:

```json
{
  "schema": "asfw.telemetry.snapshot.v1",
  "uri": "asfw://telemetry/snapshot",
  "snapshotId": "mcp-00000042",
  "generation": 17,
  "data": {
    "controller": {
      "state": "Running",
      "linkActive": true,
      "localNodeId": 0,
      "rootNodeId": 2,
      "irmNodeId": 2,
      "isIRM": false,
      "isCycleMaster": false
    },
    "bus": {
      "nodeCount": 3,
      "busResetCount": 12,
      "gapCount": 63,
      "topologyValid": true
    },
    "async": {
      "recentEventCount": 32,
      "droppedEventCount": 0,
      "timeouts": 0,
      "lastCompletionNs": 123456780000
    },
    "protocols": {
      "avcUnits": 1,
      "sbp2Units": 0,
      "diceTcatNodes": 1,
      "cmpCapableNodes": 1
    },
    "policy": {
      "runtimeMode": "readOnlyDeveloper",
      "writesListed": false,
      "writeGate": "testGateMissing"
    }
  },
  "links": [
    "asfw://controller/state",
    "asfw://bus/topology",
    "asfw://transactions/recent"
  ]
}
```

This resource should not include raw Self-ID quadlets, raw Config ROM bytes, raw
FCP frames, or large logs by default.

## 7. Controller State

URI: `asfw://controller/state`

Backed by:

- `DriverConnectorStatus`
- `getControllerStatus`
- `ASFWDiagBusContract`
- `ASFWDiagOHCI`
- `ASFWDiagPHY`
- `ASFWDiagRoleCoordinator`
- `ASFWDiagPostResetTiming`

Default `data` shape:

```json
{
  "controllerState": "Running",
  "controllerStateCode": 4,
  "linkActive": true,
  "flags": {
    "isIRM": false,
    "isCycleMaster": false
  },
  "nodes": {
    "local": 0,
    "root": 2,
    "irm": 2,
    "busManager": 2,
    "count": 3
  },
  "bus": {
    "generation": 17,
    "resetCount": 12,
    "gapCount": 63,
    "maxHops": 2
  },
  "ohci": {
    "version": "0x01001000",
    "nodeId": "0xFFC0",
    "intEventSet": "0x00000000",
    "intMaskSet": "0x00000000",
    "linkControlSet": "0x00000000"
  },
  "phy": {
    "gapCount": 63,
    "linkOn": true,
    "contender": true,
    "regValidMask": "0x0000FFFF"
  }
}
```

OHCI and PHY raw register arrays should be summarized by default. Use
`includeRaw=true` only for bounded raw details.

## 8. Driver Version

URI: `asfw://driver/version`

Backed by `getDriverVersion`.

Default `data` shape:

```json
{
  "driver": {
    "semanticVersion": "0.0.0",
    "gitCommitShort": "abcdef0",
    "gitBranch": "feat/MCP",
    "gitDirty": false,
    "buildTimestamp": "2026-06-18T16:55:00Z"
  },
  "diagnosticsAbi": {
    "version": 11,
    "compatible": true
  },
  "mcp": {
    "schemaVersion": 1,
    "runtimeMode": "readOnlyDeveloper"
  }
}
```

## 9. Bus Topology

URI: `asfw://bus/topology`

Backed by:

- `ASFWDiagTopology`
- `TopologySnapshot`
- `getTopologySnapshot`

Default `data` shape:

```json
{
  "valid": true,
  "generation": 17,
  "nodeCount": 3,
  "localNode": 0,
  "rootNode": 2,
  "irmNode": 2,
  "gapCount": 63,
  "busBase16": "0xFFC0",
  "nodes": [
    {
      "nodeId": 0,
      "address16": "0xFFC0",
      "isLocal": true,
      "isRoot": false,
      "isIRM": false,
      "linkActive": true,
      "contender": true,
      "speed": "S400",
      "powerClass": "self+15W",
      "ports": [
        {"index": 0, "state": "child", "remoteNode": 1, "remotePort": 0}
      ]
    }
  ]
}
```

Raw Self-ID quadlets are omitted by default. Use
`asfw://bus/self-id/latest?includeRaw=true` when an agent needs packet-level
debugging.

## 10. Self-ID and Bus Reset Resources

URI: `asfw://bus/self-id/latest`

Default data:

```json
{
  "generation": 17,
  "selfIdSequenceCount": 3,
  "rawSelfIdCount": 6,
  "enumeratorError": 0,
  "rawSelfIds": {
    "included": false,
    "reason": "rawDataOmitted",
    "count": 6
  }
}
```

URI: `asfw://bus/resets/recent`

Default data:

```json
{
  "count": 10,
  "items": [
    {
      "index": 12,
      "generation": 17,
      "startedNs": 123400000000,
      "completedNs": 123450000000,
      "durationUsec": 50000,
      "initiatedByASFW": false
    }
  ]
}
```

## 11. Nodes, Config ROM, and Devices

URI: `asfw://nodes`

Default data:

```json
{
  "generation": 17,
  "nodes": [
    {
      "nodeId": 2,
      "address16": "0xFFC2",
      "guid": "0x0011223344556677",
      "vendorId": "0x0003DB",
      "modelId": "0x01DDDD",
      "vendorName": "Apogee",
      "modelName": "Duet",
      "configRomCached": true,
      "protocolHints": ["avc", "cmp"]
    }
  ]
}
```

URI: `asfw://nodes/{nodeId}/summary`

Single-node version of the same shape plus links to Config ROM and protocol
resources.

URI: `asfw://nodes/{nodeId}/config-rom`

Default data:

```json
{
  "nodeId": 2,
  "generation": 17,
  "resolvedGeneration": 17,
  "exactGenerationMatch": true,
  "quadletCount": 64,
  "busInfoBlock": {
    "busName": "1394",
    "guid": "0x0011223344556677",
    "irmc": true,
    "cmc": true,
    "isc": true,
    "bmc": true,
    "maxRec": 8,
    "linkSpeed": "S400"
  },
  "rootDirectory": {
    "vendorId": "0x0003DB",
    "modelId": "0x01DDDD",
    "unitCount": 1
  },
  "raw": {
    "included": false,
    "reason": "rawDataOmitted"
  }
}
```

URI: `asfw://devices`

Backed by `getDiscoveredDevices`.

Default data:

```json
{
  "devices": [
    {
      "guid": "0x0011223344556677",
      "nodeId": 2,
      "generation": 17,
      "state": "ready",
      "vendorId": "0x0003DB",
      "modelId": "0x01DDDD",
      "vendorName": "Apogee",
      "modelName": "Duet",
      "units": [
        {
          "specifierId": "0x00A02D",
          "softwareVersion": "0x00010001",
          "state": "ready",
          "protocolHints": ["avc"]
        }
      ]
    }
  ]
}
```

## 12. Transactions

URI: `asfw://transactions/recent`

Backed by:

- `ASFWDiagAsyncTrace`
- `ASFWDiagInboundCSRStats`
- transaction result polling where available

Default data:

```json
{
  "eventCount": 32,
  "droppedCount": 0,
  "limit": 32,
  "events": [
    {
      "timestampNs": 123456789000,
      "generation": 17,
      "direction": "tx",
      "context": "ATRequest",
      "tLabel": 42,
      "tCode": "readQuadlet",
      "sourceId": "0xFFC0",
      "destinationId": "0xFFC2",
      "address": "0xFFFFF0000400",
      "payloadBytes": 4,
      "ackCode": "complete",
      "rCode": "complete",
      "speed": "S400",
      "matchedTransaction": true,
      "dropReason": null
    }
  ],
  "inboundCsr": {
    "configRomReads": 4,
    "bandwidthReads": 2,
    "bandwidthLocks": 1,
    "channelReads": 2,
    "channelLocks": 1,
    "unsupportedRequests": 0,
    "droppedRequests": 0
  }
}
```

Default limit should be lower than the ABI maximum. A practical first default is
32 events with a hard cap of `ASFW_DIAG_MAX_ASYNC_EVENTS`.

## 13. IRM Resources

URI: `asfw://irm/state`

Backed by:

- `ASFWDiagBusContract`
- `ASFWDiagBusManager`
- `ASFWDiagInboundCSRStats`

Default data:

```json
{
  "generation": 17,
  "irmNode": 2,
  "busManagerNode": 2,
  "localIsIRM": false,
  "localIsBusManager": false,
  "localIsRoot": false,
  "fallback": {
    "state": "idle",
    "plannedAction": "none",
    "annexHGateOpen": true,
    "remainingMs": 0
  },
  "localResourceController": {
    "state": "synced",
    "readbackValid": true,
    "csrControlLastStatus": 0
  }
}
```

URI: `asfw://irm/bandwidth`

```json
{
  "initial": "0x00001000",
  "available": "0x00000FBB",
  "knownLocalRegister": true,
  "lastReadGeneration": 17
}
```

URI: `asfw://irm/channels`

```json
{
  "availableHi": "0x3FFFFFFE",
  "availableLo": "0xFFFFFFFF",
  "allocatedChannelsKnownToASFW": [1],
  "lastReadGeneration": 17
}
```

## 14. Protocol Resources

### 14.1 AV/C and FCP

URI: `asfw://protocols/avc/units`

Backed by `getAVCUnits`, subunit capability calls, and descriptor calls.

```json
{
  "units": [
    {
      "guid": "0x0011223344556677",
      "nodeId": "0xFFC2",
      "vendorId": "0x0003DB",
      "modelId": "0x01DDDD",
      "plugs": {
        "isoInput": 1,
        "isoOutput": 1,
        "externalInput": 0,
        "externalOutput": 0
      },
      "subunits": [
        {
          "type": "music",
          "subunitId": 0,
          "sourcePlugs": 2,
          "destinationPlugs": 2
        }
      ]
    }
  ]
}
```

URI: `asfw://protocols/fcp/recent`

```json
{
  "limit": 16,
  "commands": [
    {
      "correlationId": "fcp-12",
      "guid": "0x0011223344556677",
      "intent": "status",
      "submittedAtNs": 123456789000,
      "completedAtNs": 123456799000,
      "status": "accepted",
      "responseBytes": 12,
      "rawIncluded": false
    }
  ]
}
```

### 14.2 CMP

URI: `asfw://protocols/cmp/plugs`

```json
{
  "nodes": [
    {
      "nodeId": 2,
      "guid": "0x0011223344556677",
      "plugs": [
        {
          "kind": "oPCR",
          "index": 0,
          "online": true,
          "channel": 1,
          "p2pConnections": 1,
          "broadcastConnections": 0,
          "rawPcr": "0x80000001"
        }
      ]
    }
  ]
}
```

URI: `asfw://protocols/cmp/connections`

Should return known or inferred CMP connection state. If ASFW only has PCR
snapshots and no higher-level connection table, report `source: "pcrSnapshot"`
and avoid implying ownership.

### 14.3 SBP-2

URI: `asfw://protocols/sbp2/units`

```json
{
  "units": [
    {
      "guid": "0x0011223344556677",
      "nodeId": 2,
      "specifierId": "0x00609E",
      "softwareVersion": "0x010483",
      "managementAgentOffset": "0x00010000",
      "lun": 0,
      "commandSet": "SCSI",
      "state": "discovered"
    }
  ]
}
```

URI: `asfw://protocols/sbp2/sessions`

Read-only session/fetch-agent state where implemented. If unavailable, return a
capability error with `capabilityUnavailable` rather than an empty object.

### 14.4 DICE and TCAT

URI: `asfw://protocols/dice/state`

```json
{
  "nodes": [
    {
      "nodeId": 2,
      "guid": "0x0011223344556677",
      "identityHint": "dice-tcat",
      "registerBase": "0xFFFFE0000000",
      "applicationBase": "0xFFFFE0200000",
      "knownStatus": {
        "clockLocked": true,
        "streaming": false,
        "notificationPending": false
      }
    }
  ]
}
```

URI: `asfw://protocols/dice/registers`

Should expose decoded known registers and bounded raw register values when
`includeRaw=true`. It must not expose audio UX controls.

URI: `asfw://protocols/tcat/application`

Should summarize the TCAT application section if present. Raw application blocks
are opt-in and bounded.

## 15. Logs

URI: `asfw://logs/recent`

Default data:

```json
{
  "limit": 50,
  "items": [
    {
      "timestamp": "2026-06-18T16:55:00Z",
      "level": "warning",
      "source": "ASFWDriverConnector",
      "message": "getConfigROM received stale cache",
      "correlationId": null
    }
  ]
}
```

This is intentionally secondary. Agents should first inspect structured
resources, then use logs to explain unexpected transitions.

## 16. Snapshot Comparison

Agents should be able to compare two `asfw://telemetry/snapshot` payloads without
special parsing logic.

Comparison-friendly rules:

- stable field names
- scalar counters for monotonic activity
- explicit `generation`
- explicit `snapshotId`
- bounded arrays with deterministic ordering
- raw data represented as included/omitted metadata
- unknown/unavailable represented with structured errors, not missing fields

If a bus reset occurs between before/after snapshots, comparison should flag:

```json
{
  "comparison": {
    "sameGeneration": false,
    "beforeGeneration": 17,
    "afterGeneration": 18,
    "notes": ["Bus generation changed; node IDs may have been reassigned"]
  }
}
```

## 17. Resource Subscription Semantics

If MCP resource subscriptions are enabled later, start with:

- `asfw://telemetry/snapshot`
- `asfw://controller/state`
- `asfw://bus/topology`
- `asfw://transactions/recent`

Do not stream high-volume raw traces by default. Subscription updates should be
coalesced and bounded.

## 18. Acceptance Criteria Mapping

- Resource URI scheme documented: sections 5 through 15.
- Large/raw data is opt-in: sections 4, 6, 9, 10, 12, 14, and 15.
- Each resource has a stable JSON shape: sections 6 through 15.
- Snapshot output is suitable for before/after comparison: sections 3, 6, and 16.
