---
name: asfw-mcp-control-plane
description: Inspect and safely operate the local ASFW FireWire MCP control plane. Use when an ASFW task needs live driver status, node discovery, protocol telemetry, MCP tool discovery, or explicitly authorized guarded FireWire control through the loopback MCP endpoint.
---

# ASFW MCP Control Plane

Use the bundled client instead of reconstructing MCP HTTP/SSE sessions or pasting large tool schemas into the conversation.

## Default workflow

1. Confirm the app-hosted MCP server is enabled. Its default endpoint is `http://127.0.0.1:8766/mcp`.
2. Ask the versioned health resource whether deeper reads are trustworthy:

   ```bash
   python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py health
   ```

   `ready` permits targeted read-only diagnostics. `degraded` permits only
   high-level inspection until its reasons are resolved. `unavailable` means
   do not infer driver state. Preserve `expectedGeneration` for any follow-up
   bus request.

3. Run the compact read-only summary when node or protocol detail is needed:

   ```bash
   python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py summary
   ```

4. Use `tools` or `resources` only when the summary lacks the needed detail.
5. Call a read tool with typed JSON arguments only after checking its schema:

   ```bash
   python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py tools
   python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_read_quadlet '{"nodeId":0,"generation":9,"addressHigh":65535,"addressLow":4026532864}'
   ```

Pass `--endpoint` or set `ASFW_MCP_ENDPOINT` when the server uses a non-default loopback port.

## Safety

- Treat `summary`, `tools`, `resources`, and `read` as the normal path.
- The client refuses non-allowlisted tool calls unless `--allow-mutation` is present. This flag is only a local acknowledgement; the MCP server's developer/mutation policy remains authoritative.
- Use `--allow-mutation` only after the user explicitly authorizes the exact hardware action. Do not infer permission from a request to inspect, diagnose, capture, or test.
- Read and preserve the current generation. A generation change means prior node IDs and writes are stale; refresh the summary before any follow-up action.
- Do not use the skill to issue raw writes or control commands merely to probe a device. Prefer the MCP hardware-smoke runner's read-only mode.
- `asfw_apogee_duet_apply_format_dev` is an intentional interruption: use it only when the user authorizes the exact rate change, includes `acknowledgeInterruption: true`, and has confirmed that audio is stopped. It is not suitable for discovery or routine diagnostics.

## Focused commands

```bash
# Discover tool/resource names without expanding every response in the prompt.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py tools
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py resources

# Read an advertised resource.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py read asfw://telemetry/snapshot

# Inspect a read-only tool's full result when needed.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_avc_list_units '{}'

# BridgeCo/BeBoB generic unit PLUG_INFO (fixed, STATUS-only FCP command).
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_bebob_get_unit_plug_info '{"targetGuid":3003878663639543,"nodeId":0,"generation":2}'
```

If the endpoint is unavailable, report the connection failure and ask the user to enable the MCP Control Plane in ASFW. Do not fall back to guessed driver state.

## PHASE 88 / BeBoB discovery rule

For the exact TerraTec PHASE 88 Rack FW identity (vendor `0x000AAC`, model
`0x000003`), do **not** gate BeBoB discovery behind generic AV/C `UNIT_INFO`
or `SUBUNIT_INFO`. Linux BeBoB begins with generic unit `PLUG_INFO`, then
BridgeCo extended ISO-plug type and stream-format-list STATUS commands.

- A generic AV/C inventory with zero subunits or zero ISO plugs is not evidence
  that the device has no BeBoB capabilities.
- Preserve the BridgeCo operand layout: its unit address is the AV/C subunit
  byte (`0xff`); the extension operands begin with direction. Stream-format
  list places support-status before entry index.
- Before treating a BeBoB STATUS command as unsupported, distinguish an FCP
  transport failure or bus reset from an AV/C `REJECTED`/`NOT IMPLEMENTED`
  response. Include `[FCP]` in the user-provided unified-log predicate.
