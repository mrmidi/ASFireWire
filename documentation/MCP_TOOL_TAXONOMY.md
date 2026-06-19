# ASFW MCP Tool Taxonomy

Linear: [FW-77](https://linear.app/asfirewire/issue/FW-77/design-dynamic-mcp-discovery-and-tool-taxonomy)

Status: Accepted taxonomy for the first MCP control-plane design pass.

## 1. Goal

Define the MCP tool and resource taxonomy for ASFW so agents get a compact,
discoverable FireWire control plane instead of a large static list of every
possible driver action.

The first MCP phase focuses on FireWire driver/protocol diagnostics:

- bus, topology, node, and Config ROM inspection
- async read/block-read and compare-swap modeling
- guarded write schemas for later policy-gated enablement
- register access for devices, DICE/TCAT, and controller diagnostics
- IRM/CAS, AV/C/FCP, CMP, SBP-2, and DICE/TCAT low-level protocol surfaces
- compact telemetry resources

Audio UX is out of scope for this taxonomy. Do not add phantom power, mixer,
routing, device-specific audio controls, or CoreAudio-facing controls here.

## 2. Design Principles

1. Keep the always-visible tool set small.
2. Prefer dynamic discovery over a giant static tool list.
3. Prefer semantic protocol tools over raw byte tools when ASFW can decode the
   protocol state.
4. Return compact structured results by default.
5. Make raw payloads and large dumps opt-in.
6. Hide write-capable tools until policy and test gates permit them.
7. If a listed tool becomes invalid because hardware state changed, return a
   structured capability/policy error instead of failing opaquely.
8. Tool names should be plain, stable, and searchable.

## 3. Runtime Modes

The MCP host uses the runtime modes defined in
`documentation/MCP_CONTROL_PLANE_ARCHITECTURE.md`.

| Mode | Tool discovery behavior |
| --- | --- |
| `disabled` | No MCP endpoint is available. |
| `mock` | Test fixture tools/resources are listed from `MockASFWDriverControl`. No live hardware access. |
| `readOnlyDeveloper` | Read-only inspection tools are listed. Mutating tools are not listed by default. |
| `developerWriteEnabled` | Read tools plus policy-gated write tools are listed only after FW-79 and FW-89 gates are satisfied. |

Write-capable tool schemas may be documented before `developerWriteEnabled`
exists, but live mutating calls must not reach the driver/user-client write path
until the policy engine and Swift MCP test gate are complete.

## 4. Minimal Always-Visible Tools

These tools are available whenever MCP is enabled. They should remain few in
number because every always-visible tool competes for agent attention.

| Tool | Mode | Purpose |
| --- | --- | --- |
| `asfw_get_capabilities` | mock, read-only, developer-write | Summarize current MCP runtime mode, driver connection state, detected tool groups, and unavailable groups. |
| `asfw_get_policy` | mock, read-only, developer-write | Report runtime mode, write gate status, policy decisions supported, and why writes are or are not listed. |
| `asfw_list_nodes` | mock, read-only, developer-write | List current bus nodes with generation, node ID, GUID if known, Config ROM cache state, and detected protocol hints. |
| `asfw_get_node_summary` | mock, read-only, developer-write | Return a compact single-node summary by node ID or GUID. |
| `asfw_explain_capability` | mock, read-only, developer-write | Explain why a tool group is available, unavailable, hidden, or policy-gated. |

`asfw_get_capabilities` is the primary discovery entry point. It should mention
dynamic groups without forcing all specialized tool definitions into the model's
active tool set.

## 5. Dynamic Tool Groups

Dynamic groups are listed only when their predicates are true. Group names are
used in capability output, telemetry resources, and tool documentation.

### 5.1 Bus and Topology

Predicate:

- driver connected
- topology/status APIs available

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_get_controller_state` | read-only | Controller status, bus generation, reset count, and basic health. |
| `asfw_get_topology` | read-only | Current topology snapshot and node map. |
| `asfw_get_bus_reset_history` | read-only | Bounded recent bus reset history. |
| `asfw_get_self_id_capture` | read-only | Latest Self-ID capture, bounded by generation when available. |
| `asfw_trigger_config_rom_read` | developer-write | Initiate Config ROM read for a node. Mutating because it starts driver work. |

Resources:

- `asfw://controller/state`
- `asfw://bus/topology`
- `asfw://bus/self-id/latest`
- `asfw://bus/resets/recent`

### 5.2 Config ROM and Discovery

Predicate:

- driver connected
- node list or Config ROM cache available

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_get_config_rom` | read-only | Return cached Config ROM bytes plus parsed summary. Raw bytes opt-in. |
| `asfw_list_discovered_devices` | read-only | Return discovered FWDevice/FWUnit state. |
| `asfw_decode_config_rom` | read-only | Decode supplied or cached Config ROM data. |

Resources:

- `asfw://nodes`
- `asfw://nodes/{nodeId}/summary`
- `asfw://nodes/{nodeId}/config-rom`
- `asfw://devices`

### 5.3 Async Transactions

Predicate:

- driver connected
- async transaction user-client methods available

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_read_quadlet` | read-only | Async quadlet read by node/generation/address. |
| `asfw_read_block` | read-only | Async block read by node/generation/address/length. |
| `asfw_get_transaction_result` | read-only | Poll or fetch a submitted transaction result by handle/correlation ID. |
| `asfw_write_quadlet` | developer-write | Policy-gated quadlet write with optional readback verification. |
| `asfw_write_block` | developer-write | Policy-gated block write with optional readback verification. |
| `asfw_compare_swap` | developer-write | Policy-gated lock/compare-swap. Also used by IRM/CMP flows. |

Address arguments must be explicit:

```json
{
  "nodeId": 2,
  "generation": 17,
  "addressHigh": 65535,
  "addressLow": 4026532864,
  "length": 4
}
```

Results must include:

- `ok`
- `status`
- `rcode`
- `generation`
- `durationUsec`
- `correlationId`
- `payload` when requested or naturally small
- `policy` for write-capable calls

### 5.4 Register Access

Predicate:

- driver connected
- target register space can be classified

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_read_device_register` | read-only | Read a device CSR/register address through async transaction path. |
| `asfw_read_device_register_block` | read-only | Read a bounded block from a device register/address space. |
| `asfw_write_device_register` | developer-write | Policy-gated device register write. |
| `asfw_write_device_register_block` | developer-write | Policy-gated block register write. |
| `asfw_read_ohci_register` | read-only | Read host OHCI/controller register for diagnostics. |
| `asfw_snapshot_ohci_registers` | read-only | Return selected bounded OHCI register snapshot. |
| `asfw_write_ohci_register_dev` | developer-write | Developer-tier controller write; hidden until explicitly enabled. |

Device register reads are normal protocol diagnostics. OHCI/controller writes are
developer-tier escape hatches and must stay hidden until both policy and test
gates exist.

### 5.5 IRM and CAS

Predicate:

- bus manager/IRM state available, or
- async compare-swap available for CAS tools

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_irm_get_state` | read-only | IRM owner, bus manager state, local/remote role, generation. |
| `asfw_irm_get_bandwidth` | read-only | Read bandwidth availability where implemented. |
| `asfw_irm_get_channels` | read-only | Read channel availability bitmaps where implemented. |
| `asfw_irm_list_allocations` | read-only | List ASFW-known allocations. |
| `asfw_cas_quadlet` | developer-write | Policy-gated compare-swap primitive. |
| `asfw_irm_allocate_channel` | developer-write | Policy-gated channel allocation. |
| `asfw_irm_free_channel` | developer-write | Policy-gated channel release. |
| `asfw_irm_allocate_bandwidth` | developer-write | Policy-gated bandwidth allocation. |
| `asfw_irm_free_bandwidth` | developer-write | Policy-gated bandwidth release. |

Resources:

- `asfw://irm/state`
- `asfw://irm/channels`
- `asfw://irm/bandwidth`
- `asfw://irm/allocations`

IRM mutation tools must report stale generation, lost IRM, compare failed, retry
exhausted, and bus reset during operation as structured reasons.

### 5.6 AV/C and FCP

Predicate:

- at least one AV/C unit detected, or
- developer discovery mode requests probing

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_avc_list_units` | read-only | List AV/C units, subunits, plugs, vendor/model IDs. |
| `asfw_avc_get_subunit_capabilities` | read-only | Return decoded subunit capabilities where available. |
| `asfw_avc_get_subunit_descriptor` | read-only | Return bounded descriptor bytes and parsed summary when available. |
| `asfw_fcp_send_command` | read-only by default | Send raw FCP/AV/C command that is inquiry/status-only by schema. |
| `asfw_fcp_send_command_dev` | developer-write | Developer-tier raw FCP command for commands that may mutate device state. |
| `asfw_fcp_get_recent_responses` | read-only | Inspect recent command/response records. |

The raw FCP tool must require a declared command intent:

- `inquiry`
- `status`
- `control`
- `notify`
- `vendorDependent`

Only inquiry/status intents are listed in read-only mode. Control, notify, and
vendor-dependent mutation paths are developer-write.

Resources:

- `asfw://protocols/avc/units`
- `asfw://protocols/fcp/recent`

### 5.7 CMP

Predicate:

- AV/C/CMP-capable node detected, or
- plug/PCR state is available

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_cmp_list_plugs` | read-only | List known iPCR/oPCR state. |
| `asfw_cmp_read_pcr` | read-only | Read and decode a plug control register. |
| `asfw_cmp_write_pcr` | developer-write | Policy-gated PCR write. |
| `asfw_cmp_establish_connection` | developer-write | Policy-gated connection establishment. |
| `asfw_cmp_break_connection` | developer-write | Policy-gated connection break. |

Resources:

- `asfw://protocols/cmp/plugs`
- `asfw://protocols/cmp/connections`

CMP write tools should use CAS where possible and return compare-failed and
generation-stale states explicitly.

### 5.8 SBP-2

Predicate:

- SBP-2 unit directory or session state detected

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_sbp2_list_units` | read-only | List SBP-2 units discovered from Config ROM/unit directories. |
| `asfw_sbp2_inspect_unit` | read-only | Decode unit directory and command-set hints. |
| `asfw_sbp2_get_session_status` | read-only | Return login/session/fetch-agent state where available. |
| `asfw_sbp2_login_dev` | developer-write | Developer-tier login path. Hidden until explicitly enabled. |
| `asfw_sbp2_submit_orb_dev` | developer-write | Developer-tier ORB submission. Hidden until explicitly enabled. |

Resources:

- `asfw://protocols/sbp2/units`
- `asfw://protocols/sbp2/sessions`

SBP-2 mutation tools are intentionally lower priority than inspection. They must
not bypass the write policy engine.

### 5.9 DICE and TCAT Low-Level Access

Predicate:

- DICE/TCAT identity or address-space hints detected from Config ROM/profile, or
- developer discovery mode requests probing

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_dice_read_register` | read-only | Read known DICE register address and optionally decode it. |
| `asfw_dice_read_block` | read-only | Read bounded DICE register block. |
| `asfw_dice_decode_status` | read-only | Decode supplied or cached DICE status/register data. |
| `asfw_tcat_read_application_block` | read-only | Read TCAT application-space block. |
| `asfw_dice_write_register` | developer-write | Policy-gated DICE register write. |
| `asfw_tcat_write_application_block` | developer-write | Policy-gated TCAT application block write. |

Resources:

- `asfw://protocols/dice/state`
- `asfw://protocols/dice/registers`
- `asfw://protocols/tcat/application`

DICE/TCAT register access is low-level protocol work, not audio UX. Do not expose
phantom power, routing, mixer, or device-control semantics in this group.

### 5.10 Telemetry and Diagnostics

Predicate:

- MCP enabled

Tools:

| Tool | Visibility | Purpose |
| --- | --- | --- |
| `asfw_get_telemetry_snapshot` | read-only | Compact snapshot across controller, topology, transactions, protocols. |
| `asfw_list_recent_transactions` | read-only | Bounded recent transaction history. |
| `asfw_get_driver_version` | read-only | Driver/app version and ABI compatibility summary. |
| `asfw_set_logging_config_dev` | developer-write | Developer-tier logging verbosity or hex dump changes. |

Resources:

- `asfw://telemetry/snapshot`
- `asfw://transactions/recent`
- `asfw://driver/version`
- `asfw://logs/recent`

Logs are supporting context, not the primary interface. Default telemetry should
be bounded and structured.

## 6. Dynamic Discovery Rules

`ListTools` should be computed from current state:

```text
runtime mode
+ driver connection state
+ current bus generation
+ discovered node summaries
+ detected protocol hints
+ available user-client methods
+ write policy gates
=> visible tool list
```

Rules:

1. Always-visible tools are listed whenever MCP is enabled.
2. Read-only dynamic tools are listed when their capability predicate is true.
3. Write-capable tools are hidden unless `developerWriteEnabled` is active and
   FW-79/FW-89 gates are satisfied.
4. Developer discovery mode may list probe tools that are safe by policy, but it
   must not list mutating raw tools in read-only mode.
5. If a tool was listed but the bus resets before invocation, the call must
   return `staleGeneration` or `capabilityChanged` rather than silently using new
   state.
6. `asfw_get_capabilities` should include unavailable groups and reasons so an
   agent can ask `asfw_explain_capability` instead of guessing.

## 7. Visibility Matrix

| Group | mock | readOnlyDeveloper | developerWriteEnabled |
| --- | --- | --- | --- |
| Always-visible | yes | yes | yes |
| Bus/topology read | fixture | yes | yes |
| Config ROM read/decode | fixture | yes | yes |
| Async read | fixture | yes | yes |
| Async write | policy fixture only | hidden | yes, policy-gated |
| Register read | fixture | yes | yes |
| Device register write | policy fixture only | hidden | yes, policy-gated |
| OHCI register write | hidden unless explicit fixture | hidden | hidden unless raw dev tier enabled |
| IRM read | fixture | yes when available | yes |
| IRM mutation/CAS | policy fixture only | hidden | yes, policy-gated |
| AV/C/FCP inquiry/status | fixture | yes when available | yes |
| AV/C/FCP control/vendor mutation | policy fixture only | hidden | yes, policy-gated |
| CMP read | fixture | yes when available | yes |
| CMP mutation | policy fixture only | hidden | yes, policy-gated |
| SBP-2 inspection | fixture | yes when available | yes |
| SBP-2 mutation | hidden unless explicit fixture | hidden | hidden unless raw dev tier enabled |
| DICE/TCAT read | fixture | yes when available | yes |
| DICE/TCAT write | policy fixture only | hidden | yes, policy-gated |

## 8. Capability Predicates

Each dynamic group must have a predicate function in `ASFWMCPCore`. Predicates
should return both a boolean and a reason string.

Example predicate result:

```json
{
  "group": "dice_tcat",
  "available": false,
  "reason": "No connected node has DICE/TCAT identity hints in the current generation",
  "requiredMode": "readOnlyDeveloper",
  "requiredEvidence": ["configRom.vendorModel", "unitDirectory.specifierId"]
}
```

Predicate inputs:

- runtime mode
- driver connection state
- current generation
- node summaries
- Config ROM cache state
- known user-client method availability
- protocol discovery results
- write gate status

## 9. Naming Rules

Tool names:

- prefix with `asfw_`
- use lowercase snake case
- put the protocol/group near the front
- use `_dev` suffix for raw/developer-tier escape hatches
- prefer verbs: `get`, `list`, `read`, `write`, `decode`, `send`, `snapshot`

Good:

- `asfw_avc_list_units`
- `asfw_dice_read_register`
- `asfw_cmp_read_pcr`
- `asfw_snapshot_ohci_registers`

Avoid:

- `asfw_do_command`
- `asfw_raw`
- `asfw_debug`
- `asfw_write`

Descriptions should state:

- whether the tool is read-only or policy-gated
- required target identity fields
- whether raw bytes are returned by default
- whether the tool can be affected by bus generation changes

## 10. Tool Annotations

Where the Swift MCP SDK supports annotations, ASFW should mark tools accurately:

- read-only tools: `readOnlyHint: true`
- idempotent inspection tools: `idempotentHint: true`
- write tools: no read-only hint
- destructive or raw developer tools: explicitly described as developer-tier

Do not mark a tool idempotent if it initiates driver work, even if it does not
write device state. For example, `asfw_trigger_config_rom_read` changes driver
activity and should not be treated as a pure read.

## 11. Structured Error Vocabulary

Common error/status reasons:

- `driverNotConnected`
- `mcpDisabled`
- `unsupportedRuntimeMode`
- `capabilityUnavailable`
- `capabilityChanged`
- `policyDenied`
- `dryRunOnly`
- `requiresDeveloperMode`
- `testGateMissing`
- `unsupportedAddressSpace`
- `staleGeneration`
- `busResetDuringOperation`
- `transactionTimeout`
- `rcodeError`
- `compareFailed`
- `unsupportedProtocol`
- `malformedRequest`
- `payloadTooLarge`
- `rawDataOmitted`

Every denial should include:

- machine-readable code
- human-readable reason
- current mode
- required mode or missing gate when applicable

## 12. Example Discovery Response Shape

`asfw_get_capabilities` should return a compact map, not a prose report:

```json
{
  "runtimeMode": "readOnlyDeveloper",
  "driverConnected": true,
  "generation": 17,
  "groups": [
    {
      "id": "bus_topology",
      "available": true,
      "tools": ["asfw_get_controller_state", "asfw_get_topology"]
    },
    {
      "id": "dice_tcat",
      "available": true,
      "tools": ["asfw_dice_read_register", "asfw_dice_read_block"],
      "hiddenTools": ["asfw_dice_write_register"],
      "hiddenReason": "Write tools require developerWriteEnabled and FW-79/FW-89 gates"
    }
  ]
}
```

## 13. Acceptance Criteria Mapping

- Tool taxonomy documented: this document.
- Dynamic discovery rules documented: sections 5 through 8.
- Minimal always-loaded set defined: section 4.
- Each dynamic group has clear capability predicates: sections 5 and 8.
