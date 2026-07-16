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

## Focused commands

```bash
# Discover tool/resource names without expanding every response in the prompt.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py tools
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py resources

# Read an advertised resource.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py read asfw://telemetry/snapshot

# Inspect a read-only tool's full result when needed.
python3 /Users/mrmidi/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_avc_list_units '{}'
```

If the endpoint is unavailable, report the connection failure and ask the user to enable the MCP Control Plane in ASFW. Do not fall back to guessed driver state.
