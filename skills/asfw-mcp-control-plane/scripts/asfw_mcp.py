#!/usr/bin/env python3
"""Compact, dependency-free client for ASFW's loopback MCP control plane."""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from typing import Any


DEFAULT_ENDPOINT = "http://127.0.0.1:8766/mcp"
PROTOCOL_VERSION = "2025-11-25"
SAFE_TOOL_PREFIXES = (
    "asfw_read_",
    "asfw_get_",
    "asfw_list_",
    "asfw_snapshot_",
    # AV/C inspection tools are read-only.  Keep this prefix separate from
    # FCP: the developer FCP command may mutate device state.
    "asfw_avc_",
    "asfw_cmp_get_",
    "asfw_sbp2_get_",
    "asfw_dice_read_",
    "asfw_irm_get_",
)

# Read-only FCP operations do not share a safe prefix with
# `asfw_fcp_send_command_dev`, which is intentionally mutation-gated.
SAFE_TOOL_NAMES = {
    # These names are verb-first rather than get/list, but MCP advertises them
    # as read-only. Keep the exception set intentionally small; in particular
    # it must not admit any CMP/FCP write or connection-establishment tool.
    "asfw_bebob_read_bootrom_info",
    "asfw_bebob_get_unit_plug_info",
    "asfw_bebob_get_clock_topology",
    "asfw_cmp_list_plugs",
    "asfw_cmp_read_pcr",
    "asfw_fcp_send_command",
    "asfw_fcp_get_recent_responses",
    "asfw_irm_list_allocations",
    "asfw_phase88_get_clock",
}


class MCPError(RuntimeError):
    pass


class MCPClient:
    def __init__(self, endpoint: str, timeout: float) -> None:
        self.endpoint = endpoint
        self.timeout = timeout
        self.session_id: str | None = None
        self.request_id = 0

    def connect(self) -> None:
        result = self.request(
            "initialize",
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "ASFW Codex skill", "version": "1"},
            },
            initialize=True,
        )
        if "serverInfo" not in result:
            raise MCPError("MCP initialize response did not include serverInfo.")

    def request(self, method: str, params: Any | None = None, initialize: bool = False) -> dict[str, Any]:
        self.request_id += 1
        payload: dict[str, Any] = {"jsonrpc": "2.0", "id": self.request_id, "method": method}
        if params is not None:
            payload["params"] = params
        return self._post(payload, initialize=initialize)

    def notify(self, method: str, params: Any | None = None) -> None:
        payload: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        self._post(payload, notification=True)

    def _post(
        self,
        payload: dict[str, Any],
        *,
        initialize: bool = False,
        notification: bool = False,
    ) -> dict[str, Any]:
        headers = {
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
        }
        if self.session_id and not initialize:
            headers["MCP-Session-Id"] = self.session_id
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                if initialize:
                    self.session_id = response.headers.get("MCP-Session-Id")
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            # ASFW's single-session host currently returns the active session
            # rather than creating a second one. Reuse it for read-only agent
            # access; normal MCP servers still take the success path above.
            existing_session = error.headers.get("MCP-Session-Id")
            if initialize and error.code == 400 and "Session already initialized" in detail and existing_session:
                self.session_id = existing_session
                return {"serverInfo": {"name": "ASFW MCP Control Plane"}}
            raise MCPError(f"MCP HTTP {error.code}: {detail}") from error
        except urllib.error.URLError as error:
            raise MCPError(f"Cannot reach {self.endpoint}: {error.reason}") from error

        if notification or not body.strip():
            return {}
        message = decode_mcp_message(body)
        if "error" in message:
            error = message["error"]
            raise MCPError(f"MCP error {error.get('code')}: {error.get('message')}")
        if "result" not in message:
            raise MCPError("MCP response did not contain a result.")
        return message["result"]


def decode_mcp_message(body: str) -> dict[str, Any]:
    """Decode a JSON response or the first JSON-RPC message from an SSE body."""
    stripped = body.strip()
    if stripped.startswith("{"):
        return json.loads(stripped)
    data_lines = [line[5:].strip() for line in body.splitlines() if line.startswith("data:")]
    if not data_lines:
        raise MCPError("MCP response was neither JSON nor an SSE data message.")
    return json.loads("\n".join(data_lines))


def resource_json(result: dict[str, Any]) -> Any:
    contents = result.get("contents")
    if not isinstance(contents, list) or not contents:
        return result
    text = contents[0].get("text") if isinstance(contents[0], dict) else None
    if not isinstance(text, str):
        return result
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def unwrap(value: Any) -> Any:
    if isinstance(value, dict) and isinstance(value.get("data"), dict):
        return value["data"]
    return value


def compact_telemetry(value: Any) -> Any:
    if not isinstance(value, dict):
        return value
    data = unwrap(value)
    if not isinstance(data, dict):
        return value
    bus = data.get("bus", {})
    controller = data.get("controller", {})
    policy = data.get("policy", {})
    output = {
        "driverConnected": value.get("driverConnected"),
        "generation": value.get("generation"),
        "controllerState": controller.get("state"),
        "nodeCount": bus.get("nodeCount"),
        "localNodeId": controller.get("localNodeId"),
        "irmNodeId": controller.get("irmNodeId"),
        "rootNodeId": controller.get("rootNodeId"),
        "protocols": data.get("protocols"),
        "writeGate": policy.get("writeGate"),
    }
    return {key: item for key, item in output.items() if item is not None}


def compact_nodes(value: Any) -> Any:
    value = unwrap(value)
    nodes = value.get("nodes") if isinstance(value, dict) else value
    if not isinstance(nodes, list):
        return value
    keys = ("nodeId", "guid", "vendorName", "modelName", "protocolHints", "configRomCached")
    return [{key: node[key] for key in keys if key in node} for node in nodes if isinstance(node, dict)]


def compact_health(value: Any) -> Any:
    if not isinstance(value, dict):
        return value
    data = unwrap(value)
    if not isinstance(data, dict):
        return value
    keys = (
        "status",
        "reasons",
        "expectedGeneration",
        "allowReadOnlyQueries",
        "allowTargetedReads",
        "capabilities",
    )
    return {key: data[key] for key in keys if key in data}


def read_resource(client: MCPClient, uri: str) -> Any:
    return resource_json(client.request("resources/read", {"uri": uri}))


def advertised_resource_uri(resources: list[dict[str, Any]], fragment: str) -> str | None:
    fragment = fragment.lower()
    for resource in resources:
        uri = resource.get("uri")
        if isinstance(uri, str) and fragment in uri.lower():
            return uri
    return None


def command_summary(client: MCPClient) -> Any:
    listing = client.request("resources/list")
    resources = listing.get("resources", [])
    if not isinstance(resources, list):
        raise MCPError("resources/list returned an invalid resource list.")
    telemetry_uri = advertised_resource_uri(resources, "telemetry")
    nodes_uri = advertised_resource_uri(resources, "nodes")
    health_uri = advertised_resource_uri(resources, "control-plane/health")
    output: dict[str, Any] = {"endpoint": client.endpoint}
    if health_uri:
        output["health"] = compact_health(read_resource(client, health_uri))
    if telemetry_uri:
        output["telemetry"] = compact_telemetry(read_resource(client, telemetry_uri))
    if nodes_uri:
        output["nodes"] = compact_nodes(read_resource(client, nodes_uri))
    if not telemetry_uri and not nodes_uri:
        output["resources"] = [resource.get("uri") for resource in resources]
    return output


def command_health(client: MCPClient) -> Any:
    listing = client.request("resources/list")
    resources = listing.get("resources", [])
    if not isinstance(resources, list):
        raise MCPError("resources/list returned an invalid resource list.")
    health_uri = advertised_resource_uri(resources, "control-plane/health")
    if health_uri is None:
        raise MCPError("Server does not advertise asfw://control-plane/health; update ASFW and retry.")
    return compact_health(read_resource(client, health_uri))


def is_safe_tool(name: str) -> bool:
    return name.startswith(SAFE_TOOL_PREFIXES) or name in SAFE_TOOL_NAMES


def parse_arguments(raw: str) -> Any:
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise MCPError(f"Tool arguments must be valid JSON: {error.msg}") from error
    if not isinstance(value, dict):
        raise MCPError("Tool arguments must be a JSON object.")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", default=os.environ.get("ASFW_MCP_ENDPOINT", DEFAULT_ENDPOINT))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--allow-mutation", action="store_true")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("health")
    subparsers.add_parser("summary")
    subparsers.add_parser("tools")
    subparsers.add_parser("resources")
    read = subparsers.add_parser("read")
    read.add_argument("uri")
    call = subparsers.add_parser("call")
    call.add_argument("tool")
    call.add_argument("arguments", nargs="?", default="{}")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        client = MCPClient(args.endpoint, args.timeout)
        client.connect()
        if args.command == "health":
            output = command_health(client)
        elif args.command == "summary":
            output = command_summary(client)
        elif args.command == "tools":
            output = client.request("tools/list")
        elif args.command == "resources":
            output = client.request("resources/list")
        elif args.command == "read":
            output = read_resource(client, args.uri)
        else:
            if not args.allow_mutation and not is_safe_tool(args.tool):
                raise MCPError(
                    f"Refusing non-allowlisted tool '{args.tool}'. "
                    "Use --allow-mutation only with explicit user authorization."
                )
            output = client.request("tools/call", {"name": args.tool, "arguments": parse_arguments(args.arguments)})
        print(json.dumps(output, indent=2, sort_keys=True))
        return 0
    except MCPError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
