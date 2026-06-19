# ASFW MCP Agent Workflows

Linear: [FW-88](https://linear.app/asfirewire/issue/FW-88/design-programmatic-mcp-workflows-for-agent-orchestration)

Status: Candidate programmatic workflows over the implemented MCP tool surface.

> Workflows compose the read-only and policy-gated tools from FW-77..85. They are
> written so an agent can run them through code/tool calling with compact,
> parseable returns. Each step is marked read-only (R) or mutating (M); mutating
> steps require `developerWriteEnabled` + policy clearance. "Parallel" marks steps
> with no data dependency that may be issued concurrently. Tool result shapes are
> in `MCP_TOOL_USE_EXAMPLES.md`.

## Conventions

- Prefer resources for bulk state (`asfw://telemetry/snapshot`, `asfw://nodes`)
  and tools for targeted actions.
- Keep raw payloads opt-in: pass `includeRaw`/`decode` only when needed.
- Re-read `generation` between discovery and any mutation; a bus reset between
  them yields `staleGeneration` and the step must restart from discovery.
- Bound fan-out: cap per-node parallelism to avoid flooding the single async
  request queue.

## W1 — Enumerate and summarize the bus (R, idempotent)

| Step | Tool | Kind | Parallel |
| --- | --- | --- | --- |
| 1 | `asfw_list_nodes` | R | — |
| 2 | `asfw_get_config_rom` per node | R | yes (per node) |
| 3 | `asfw_decode_config_rom` (if raw fetched) | R | yes |

Output: one compact row per node (nodeId, GUID, vendor/model, protocol hints).
Step 2 fans out across nodes; cap concurrency. Pure read-only — safe to repeat.

## W2 — Probe AV/C units (R, idempotent)

| Step | Tool | Kind | Parallel |
| --- | --- | --- | --- |
| 1 | `asfw_avc_list_units` | R | — |
| 2 | `asfw_avc_get_subunit_capabilities` per unit | R | yes (per unit) |
| 3 | `asfw_fcp_send_command` (intent `status`/`inquiry`) | R | yes |

Only inquiry/status intents — never `control` — keep this workflow read-only.
Gated behind the `avc` protocol hint, so it only lists for AV/C-capable nodes.

## W3 — Register snapshot (R, idempotent)

| Step | Tool | Kind | Parallel |
| --- | --- | --- | --- |
| 1 | `asfw_snapshot_ohci_registers` (bounded offset set) | R | — |
| 2 | `asfw_read_device_register` for selected CSRs | R | yes |
| 3 | `asfw_dice_read_register` (capable nodes) | R | yes |

Returns a fixed, bounded set of register values for diagnostics. Offsets are
quadlet-aligned and capped (≤ 64 for OHCI snapshot).

## W4 — Before/after bus-reset diff (R, idempotent)

| Step | Tool/Resource | Kind | Parallel |
| --- | --- | --- | --- |
| 1 | read `asfw://bus/topology` + `asfw://telemetry/snapshot` | R | yes |
| 2 | (bus reset occurs / is triggered out of band) | — | — |
| 3 | re-read the same resources at the new generation | R | yes |
| 4 | diff node set, gap count, IRM owner, reset count | R | — |

Compare by `generation`; report added/removed nodes and changed roles. No
mutation — `asfw_trigger_config_rom_read` is *not* used here (it is non-idempotent
because it starts driver work).

## W5 — Aggregate recent transaction failures (R, idempotent)

| Step | Tool/Resource | Kind | Parallel |
| --- | --- | --- | --- |
| 1 | read `asfw://transactions/recent` | R | — |
| 2 | filter `rcode != "complete"` or `matchedTransaction == false` | R | — |
| 3 | group by destination/tcode/rcode | R | — |

Bounded history (default 32). Returns a compact failure histogram, not the raw
event list, unless explicitly requested.

## Mutating workflows (M — developerWriteEnabled + policy)

These are listed for completeness; every step routes through the FW-79 policy and
returns a structured decision on refusal.

- **Channel claim:** `asfw_irm_get_channels` (R) → `asfw_irm_allocate_channel` (M,
  CAS) → verify with `asfw_irm_get_channels` (R). Restart on `staleGeneration` or
  `compareFailed`.
- **PCR connection:** `asfw_cmp_read_pcr` (R) → `asfw_cmp_establish_connection` (M,
  CAS) → `asfw_cmp_read_pcr` (R) to confirm.
- **Register poke:** `asfw_read_device_register` (R) → `asfw_write_device_register`
  with `verifyReadback` (M) → inspect the verification result.

Mutations are inherently serial against shared bus state (generation-pinned CAS);
do not parallelize allocations or PCR writes.

## Acceptance mapping

- Candidate workflows with expected tool sequences: W1–W5 + mutating set.
- Compact, parseable returns: each workflow names its bounded output.
- Idempotent/read-only marked: the Kind column (R/M) per step.
- Parallelizable operations identified: the Parallel column.
