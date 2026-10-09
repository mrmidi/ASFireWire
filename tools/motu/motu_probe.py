#!/usr/bin/env python3
"""Read-only MOTU snapshots and provenance-preserving fixture export.

No settings writes, stream starts, bus resets, or packet-capture arming.
Transport/schema follows skills/asfw-mcp-control-plane and tools/avc.
Register behavior cross-checked with local Linux motu-protocol-v{1,2,3}.c,
motu-stream.c:23 and motu-transaction.c:75. Fresh implementation, no copied code.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "skills/asfw-mcp-control-plane/scripts"))
from asfw_mcp import MCPClient, MCPError, resource_json, unwrap

MODELS = {
    "828": 1, "896": 1,
    "828mk2": 2, "896hd": 2, "traveler": 2, "ultralite": 2, "8pre": 2,
    "828mk3-fw": 3, "828mk3-hybrid": 3, "896mk3-fw": 3,
    "896mk3-hybrid": 3, "ultralite-mk3-fw": 3,
    "ultralite-mk3-hybrid": 3, "traveler-mk3": 3,
    "audio-express": 3, "track16": 3, "4pre": 3,
}
RATES = (44100, 48000, 88200, 96000, 176400, 192000)


def number(value):
    if isinstance(value, bool):
        raise ValueError("Boolean is not an identifier")
    return int(value, 0) if isinstance(value, str) else int(value)


def register_plan(model):
    # Independent directions; original 828 config and stream control overlap.
    plan = [("isoc_control", 0x0b00), ("packet_format", 0x0b10)]
    if model == "828":
        return plan
    plan.append(("clock_status", 0x0b14))
    if MODELS[model] == 2:
        plan.append(("optical_configuration", 0x0c04))
    elif model in {"828mk3-fw", "828mk3-hybrid", "896mk3-fw",
                   "896mk3-hybrid", "traveler-mk3", "track16"}:
        plan.append(("optical_banks", 0x0c94))
    return plan


def decode(model, name, value):
    """Limited, reference-backed decoding. No inferred geometry/PLL lock."""
    if name == "clock_status" or (model == "828" and name == "isoc_control"):
        if model == "828":
            return {"sampleRateHz": 48000 if value & 4 else 44100,
                    "clockSourceCode": value & 0x23,
                    "opticalInputSpdif": bool(value & 0x8000),
                    "opticalOutputSpdif": bool(value & 0x4000)}
        index = (value >> (8 if MODELS[model] == 3 else 3)) & (255 if MODELS[model] == 3 else (3 if model == "896" else 7))
        return {"sampleRateHz": RATES[index] if index < len(RATES) else None,
                "rateIndex": index,
                "clockSourceCode": value & (255 if MODELS[model] == 3 else 7)}
    if name == "optical_configuration":
        return {"inputModeCode": (value >> 8) & 3,
                "outputModeCode": (value >> 10) & 3}
    if name == "optical_banks":
        fields = (("inputA", 1, 0x10000), ("inputB", 2, 0x100000),
                  ("outputA", 0x100, 0x40000), ("outputB", 0x200, 0x400000))
        return {key: {"enabled": bool(value & enabled),
                      "mode": "spdif" if value & spdif else "adat"}
                for key, enabled, spdif in fields}
    return {}


class Probe:
    def __init__(self, client):
        self.client = client

    def resource(self, uri):
        return resource_json(self.client.request("resources/read", {"uri": uri}))

    def tool(self, name, arguments):
        result = self.client.request("tools/call", {"name": name, "arguments": arguments})
        data = result.get("structuredContent")
        if result.get("isError") or not isinstance(data, dict):
            raise MCPError(f"{name}: tool error or missing structured result: {result}")
        if data.get("ok") is not True:
            raise MCPError(f"{name}: {data}")
        return data

    def route(self, guid):
        health = self.resource("asfw://control-plane/health")
        if unwrap(health).get("status") != "ready":
            raise MCPError(f"Driver not ready: {health}")
        nodes = self.resource("asfw://nodes")
        data = unwrap(nodes)
        generation = data.get("generation", nodes.get("generation"))
        if generation is None:
            raise MCPError("No authoritative bus generation")
        matches = [n for n in data.get("nodes", [])
                   if n.get("guid") is not None and number(n["guid"]) == guid]
        if len(matches) != 1:
            raise MCPError("Target GUID missing or ambiguous")
        node = matches[0]
        if node.get("vendorId") is None or number(node["vendorId"]) != 0x0001f2:
            raise MCPError("Target discovery identity is not MOTU (vendor 0x0001f2)")
        return number(generation), number(node["nodeId"]), node

    def snapshot(self, args):
        generation, node_id, identity = self.route(args.guid)
        document = {"schema": "asfw.motu.probe.v1", "provenance": "hardware-observation",
                    "capturedAt": datetime.now(timezone.utc).isoformat(),
                    "modelDeclaration": args.model, "modelDeclarationVerified": False,
                    "firmwareDeclaration": args.firmware, "phase": args.phase,
                    "notes": args.notes, "identity": identity, "generation": generation,
                    "registers": [], "complete": False}
        try:
            if args.packet_capture:
                raw = args.packet_capture.read_bytes()
                document["packetCaptureAttachment"] = {
                    "filename": args.packet_capture.name,
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "provenanceDeclaration": args.packet_source,
                    "parsedOrValidated": False,
                    "text": raw.decode("utf-8")}
            document["driver"] = self.tool("asfw_get_driver_version", {})
            document["streamHealth"] = self.tool("asfw_get_audio_stream_health", {})
            endpoints = unwrap(document["streamHealth"]).get("endpoints")
            if not isinstance(endpoints, list):
                raise MCPError("Stream health lacks endpoints; cannot establish idle state")
            streaming = any(ep.get("streaming") is True for ep in endpoints)
            if streaming or args.telemetry_only:
                document["registersSkipped"] = "active stream" if streaming else "telemetry-only requested"
            else:
                for name, offset in register_plan(args.model):
                    current_gen, current_node, _ = self.route(args.guid)
                    if (current_gen, current_node) != (generation, node_id):
                        raise MCPError("Route changed; remaining register reads cancelled")
                    health = self.tool("asfw_get_audio_stream_health", {})
                    eps = unwrap(health).get("endpoints")
                    if not isinstance(eps, list) or any(ep.get("streaming") is True for ep in eps):
                        raise MCPError("Stream started or state unavailable; reads cancelled")
                    record = {"name": name, "addressHigh": 0xffff,
                              "addressLow": 0xf0000000 + offset}
                    document["registers"].append(record)
                    result = self.tool("asfw_read_quadlet", {
                        "nodeId": node_id, "generation": generation,
                        "addressHigh": record["addressHigh"], "addressLow": record["addressLow"]})
                    record["result"] = result
                    payload = unwrap(result).get("payload")
                    if (not isinstance(payload, list) or len(payload) != 4 or
                        any(type(b) is not int or not 0 <= b <= 255 for b in payload)):
                        raise MCPError("Read did not return exactly four wire bytes")
                    value = int.from_bytes(bytes(payload), "big")
                    record.update(value=value, valueHex=f"0x{value:08x}",
                                  decoded=decode(args.model, name, value))
            end_gen, end_node, _ = self.route(args.guid)
            if (end_gen, end_node) != (generation, node_id):
                raise MCPError("Route changed during snapshot")
            document["complete"] = True
        except (MCPError, ValueError, TypeError) as error:
            document["error"] = str(error)
        return document


def export_fixture(document):
    if document.get("schema") != "asfw.motu.probe.v1" or not document.get("complete"):
        raise ValueError("Only complete probe snapshots can be exported")
    if document.get("provenance") != "hardware-observation":
        raise ValueError("Input is not a hardware observation")
    registers = document.get("registers", [])
    if not registers:
        raise ValueError("Snapshot contains no register observations")
    return {"schema": "asfw.motu.register-fixture.v1",
            "provenance": "hardware-observation", "validatedGolden": False,
            "modelDeclaration": document["modelDeclaration"],
            "firmwareDeclaration": document["firmwareDeclaration"],
            "phase": document["phase"], "identity": document["identity"],
            "generation": document["generation"], "capturedAt": document["capturedAt"],
            "registers": [{k: r[k] for k in ("name", "addressHigh", "addressLow", "value")}
                          for r in registers],
            "sourceSha256": hashlib.sha256(json.dumps(document, sort_keys=True).encode()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    capture = sub.add_parser("snapshot")
    capture.add_argument("--guid", type=number, required=True)
    capture.add_argument("--model", choices=MODELS, required=True)
    capture.add_argument("--firmware", default="unknown")
    capture.add_argument("--phase", choices=("idle", "playing", "recording", "stopped", "after-reset", "after-wake"), required=True)
    capture.add_argument("--notes", default="")
    capture.add_argument("--telemetry-only", action="store_true")
    capture.add_argument("--packet-capture", type=Path, help="Attach an existing UTF-8 packet dump; does not arm capture")
    capture.add_argument("--packet-source", choices=("asfw", "vendor-driver", "unknown"), default="unknown")
    capture.add_argument("--endpoint", default=os.getenv("ASFW_MCP_ENDPOINT", "http://127.0.0.1:8766/mcp"))
    export = sub.add_parser("export")
    export.add_argument("input", type=Path)
    for command in (capture, export):
        command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "snapshot":
            client = MCPClient(args.endpoint, 5.0)
            client.connect()
            document = Probe(client).snapshot(args)
        else:
            document = export_fixture(json.loads(args.input.read_text()))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as output:
            json.dump(document, output, indent=2)
            output.write("\n")
        print(args.output)
        return 0 if document.get("complete", True) else 1
    except (MCPError, ValueError, OSError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
