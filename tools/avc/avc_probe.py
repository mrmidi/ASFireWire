#!/usr/bin/env python3
"""Hardware probe tool for AV/C devices over ASFW's loopback MCP control plane.

Sends read-only FCP commands (STATUS 0x01 and SPECIFIC INQUIRY 0x02) to discover
and record device capabilities, outputting JSON and Markdown fixtures.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_ENDPOINT = "http://127.0.0.1:8766/mcp"
PROTOCOL_VERSION = "2025-11-25"
FCP_COMMAND_ADDR_HIGH = 0xFFFF
FCP_COMMAND_ADDR_LOW = 0xF0000B00

# Subunit types (1394-1995 / AV/C General)
SUBUNIT_AUDIO = 0x01
SUBUNIT_MUSIC = 0x0C
SUBUNIT_UNIT = 0x1F


class ProbeError(RuntimeError):
    pass


class ProbeWedgeError(ProbeError):
    """Raised when transport times out or device wedges."""
    pass


def decode_mcp_message(body: str) -> dict[str, Any]:
    stripped = body.strip()
    if stripped.startswith("{"):
        return json.loads(stripped)
    data_lines = [line[5:].strip() for line in body.splitlines() if line.startswith("data:")]
    if not data_lines:
        raise ProbeError("MCP response was neither JSON nor an SSE data message.")
    return json.loads("\n".join(data_lines))


class MCPClient:
    def __init__(self, endpoint: str = DEFAULT_ENDPOINT, timeout: float = 5.0) -> None:
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
                "clientInfo": {"name": "ASFW AVC Probe", "version": "1.0"},
            },
            initialize=True,
        )
        if "serverInfo" not in result:
            raise ProbeError("MCP initialize response did not include serverInfo.")

    def request(self, method: str, params: Any | None = None, initialize: bool = False) -> dict[str, Any]:
        self.request_id += 1
        payload: dict[str, Any] = {"jsonrpc": "2.0", "id": self.request_id, "method": method}
        if params is not None:
            payload["params"] = params
        return self._post(payload, initialize=initialize)

    def _post(self, payload: dict[str, Any], *, initialize: bool = False) -> dict[str, Any]:
        headers = {
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "MCP-Protocol-Version": PROTOCOL_VERSION,
        }
        if self.session_id and not initialize:
            headers["MCP-Session-Id"] = self.session_id

        req = urllib.request.Request(self.endpoint, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as response:
                if initialize:
                    self.session_id = response.headers.get("MCP-Session-Id")
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            existing = error.headers.get("MCP-Session-Id")
            if initialize and error.code == 400 and "Session already initialized" in detail and existing:
                self.session_id = existing
                return {"serverInfo": {"name": "ASFW MCP Control Plane"}}
            raise ProbeError(f"MCP HTTP {error.code}: {detail}") from error
        except urllib.error.URLError as error:
            raise ProbeError(f"Cannot reach {self.endpoint}: {error.reason}") from error

        if not body.strip():
            return {}
        message = decode_mcp_message(body)
        if "error" in message:
            err = message["error"]
            raise ProbeError(f"MCP error {err.get('code')}: {err.get('message')}")
        if "result" not in message:
            raise ProbeError("MCP response did not contain a result.")
        return message["result"]

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        return result.get("structuredContent", {})

    def read_resource(self, uri: str) -> Any:
        result = self.request("resources/read", {"uri": uri})
        contents = result.get("contents", [])
        if contents and "text" in contents[0]:
            try:
                return json.loads(contents[0]["text"])
            except json.JSONDecodeError:
                return contents[0]["text"]
        return contents


@dataclass
class ExchangeRecord:
    name: str
    command: list[int]
    response: list[int] | None = None
    responseCode: int | None = None
    durationUsec: int | None = None
    ok: bool = False
    status: str = ""
    error: str | None = None


@dataclass
class DeviceMetadata:
    key: str
    modelName: str
    vendorName: str
    guid: str
    guidUint64: int
    nodeId: int
    generation: int
    driverVersion: str
    capturedAt: str


class AvcProbeRunner:
    def __init__(self, client: MCPClient, verbose: bool = False) -> None:
        self.client = client
        self.verbose = verbose
        self.records: list[ExchangeRecord] = []
        self.metadata: DeviceMetadata | None = None

    def pre_check(self, device_key_override: str | None = None) -> DeviceMetadata:
        # 1. Health check & generation
        health_res = self.client.read_resource("asfw://control-plane/health")
        health_data = health_res.get("data", health_res) if isinstance(health_res, dict) else {}
        status = health_data.get("status")
        if status != "ready":
            raise ProbeError(f"Driver health is not ready: status='{status}', reasons={health_data.get('reasons')}")

        generation = health_res.get("generation") or health_data.get("expectedGeneration")

        # 2. Driver version
        ver_res = self.client.call_tool("asfw_get_driver_version", {})
        ver_data = ver_res.get("data", {})
        driver_ver = ver_data.get("semanticVersion", "unknown")

        # 3. Stream health - must not be streaming
        stream_res = self.client.call_tool("asfw_get_audio_stream_health", {})
        stream_data = stream_res.get("data", {})
        endpoints = stream_data.get("endpoints", [])
        for ep in endpoints:
            if ep.get("streaming", False):
                raise ProbeError(f"Device is actively streaming audio (guid={ep.get('guid')}). Stop streaming before probing.")

        # 4. Telemetry / Nodes
        nodes_res = self.client.read_resource("asfw://nodes")
        nodes_data = nodes_res.get("data", nodes_res) if isinstance(nodes_res, dict) else {}
        node_list = nodes_data.get("nodes", []) if isinstance(nodes_data, dict) else []
        if not node_list:
            raise ProbeError("No FireWire nodes discovered by driver.")

        # Find AV/C node
        target_node = None
        for n in node_list:
            hints = n.get("protocolHints", [])
            if "avc" in hints or "bebob" in hints:
                target_node = n
                break
        if target_node is None:
            target_node = node_list[0]

        guid_str = target_node.get("guid", "0x0")
        guid_int = int(guid_str, 16) if guid_str.startswith("0x") else int(guid_str)
        model_name = target_node.get("modelName", "Unknown Model")
        vendor_name = target_node.get("vendorName", "Unknown Vendor")
        node_id = target_node.get("nodeId", 0)

        # Fallback generation check from telemetry/snapshot
        if not generation:
            telem_res = self.client.read_resource("asfw://telemetry/snapshot")
            generation = telem_res.get("generation", 1)

        key = device_key_override
        if not key:
            # Generate slug from model name
            key = model_name.lower().replace(" ", "-").replace("/", "-")

        self.metadata = DeviceMetadata(
            key=key,
            modelName=model_name,
            vendorName=vendor_name,
            guid=f"0x{guid_int:016X}",
            guidUint64=guid_int,
            nodeId=node_id,
            generation=generation,
            driverVersion=driver_ver,
            capturedAt=datetime.now(timezone.utc).isoformat(),
        )
        return self.metadata

    def send_frame(self, name: str, payload: list[int], intent: str = "status") -> ExchangeRecord:
        if self.metadata is None:
            raise ProbeError("Runner has not passed pre_check.")

        args = {
            "targetGuid": self.metadata.guidUint64,
            "nodeId": self.metadata.nodeId,
            "generation": self.metadata.generation,
            "addressHigh": FCP_COMMAND_ADDR_HIGH,
            "addressLow": FCP_COMMAND_ADDR_LOW,
            "intent": intent,
            "payload": payload,
        }

        if self.verbose:
            cmd_hex = " ".join(f"{b:02X}" for b in payload)
            print(f"  [>] {name:<35} : {cmd_hex}")

        result = self.client.call_tool("asfw_fcp_send_command", args)
        ok = result.get("ok", False)
        data = result.get("data", {})
        status = data.get("status", "")
        response = data.get("response")
        duration = data.get("durationUsec")
        errors = result.get("errors", [])
        err_msg = errors[0].get("reason") if errors else None

        # Check for transport wedge
        if not ok and (status in ("timeout", "unavailable", "wedged") or "timeout" in str(err_msg).lower()):
            record = ExchangeRecord(
                name=name,
                command=payload,
                response=response,
                responseCode=response[0] if response else None,
                durationUsec=duration,
                ok=False,
                status=status,
                error=err_msg or "Transport timeout",
            )
            self.records.append(record)
            raise ProbeWedgeError(f"Transport wedge on '{name}': status={status}, error={err_msg}")

        resp_code = response[0] if (response and len(response) > 0) else None
        record = ExchangeRecord(
            name=name,
            command=payload,
            response=response,
            responseCode=resp_code,
            durationUsec=duration,
            ok=ok,
            status=status,
            error=err_msg,
        )
        self.records.append(record)

        if self.verbose:
            if response:
                resp_hex = " ".join(f"{b:02X}" for b in response)
                code_str = {0x08: "NOT_IMPL", 0x09: "ACCEPTED", 0x0A: "REJECTED", 0x0B: "IN_PROCESS", 0x0C: "STABLE"}.get(resp_code, f"CODE_{resp_code:02X}")
                print(f"  [<] {code_str:<10} ({duration or 0:>5} µs) : {resp_hex}")
            else:
                print(f"  [<] FAILED ({status}) : {err_msg}")

        return record

    def run_battery(self) -> None:
        print(f"\nProbing {self.metadata.vendorName} {self.metadata.modelName} (GUID {self.metadata.guid}, node {self.metadata.nodeId})...")

        # ---------------------------------------------------------------------
        # 1. UNIT INFO
        # ---------------------------------------------------------------------
        print("\n[1/9] Probing UNIT INFO...")
        unit_info_rec = self.send_frame("unit_info", [0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF])

        # ---------------------------------------------------------------------
        # 2. SUBUNIT INFO
        # ---------------------------------------------------------------------
        print("\n[2/9] Probing SUBUNIT INFO (pages 0..7)...")
        subunits_found: list[tuple[int, int]] = []  # (subunit_type, subunit_id)
        for page in range(8):
            page_rec = self.send_frame(f"subunit_info_page_{page}", [0x01, 0xFF, 0x31, (page << 4) | 0x07, 0xFF, 0xFF, 0xFF, 0xFF])
            if not page_rec.response or page_rec.responseCode in (0x08, 0x0A):  # NOT IMPLEMENTED or REJECTED
                break
            if page_rec.responseCode == 0x0C and len(page_rec.response) >= 8:
                for entry_byte in page_rec.response[4:8]:
                    if entry_byte == 0xFF:
                        break
                    stype = (entry_byte >> 3) & 0x1F
                    max_id = entry_byte & 0x07
                    for sid in range(max_id + 1):
                        if (stype, sid) not in subunits_found:
                            subunits_found.append((stype, sid))

        print(f"  Discovered subunits: {subunits_found}")

        # ---------------------------------------------------------------------
        # 3. PLUG INFO
        # ---------------------------------------------------------------------
        print("\n[3/9] Probing PLUG INFO...")
        plug_info_00 = self.send_frame("plug_info_unit_00", [0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
        plug_info_01 = self.send_frame("plug_info_unit_01", [0x01, 0xFF, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF])

        num_iso_in = 0
        num_iso_out = 0
        if plug_info_00.response and plug_info_00.responseCode == 0x0C and len(plug_info_00.response) >= 8:
            num_iso_in = plug_info_00.response[4]
            num_iso_out = plug_info_00.response[5]
            print(f"  Unit Iso Plugs: inputs={num_iso_in}, outputs={num_iso_out}")

        subunit_plugs: dict[tuple[int, int], tuple[int, int]] = {}  # (stype, sid) -> (destPlugs, srcPlugs)
        for stype, sid in subunits_found:
            saddr = (stype << 3) | (sid & 0x07)
            stype_name = {SUBUNIT_AUDIO: "audio", SUBUNIT_MUSIC: "music"}.get(stype, f"subunit_0x{stype:02X}")
            sub_plug_rec = self.send_frame(f"plug_info_{stype_name}_{sid}", [0x01, saddr, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
            if sub_plug_rec.response and sub_plug_rec.responseCode == 0x0C and len(sub_plug_rec.response) >= 6:
                d_plugs = sub_plug_rec.response[4]
                s_plugs = sub_plug_rec.response[5]
                subunit_plugs[(stype, sid)] = (d_plugs, s_plugs)
                print(f"  {stype_name}:{sid} Plugs: destination={d_plugs}, source={s_plugs}")

        # ---------------------------------------------------------------------
        # 4. PLUG SIGNAL FORMAT (Input and Output unit iso plugs)
        # ---------------------------------------------------------------------
        print("\n[4/9] Probing PLUG SIGNAL FORMAT for unit iso plugs...")
        valid_input_formats: list[tuple[int, list[int]]] = []
        valid_output_formats: list[tuple[int, list[int]]] = []

        for plug_id in range(num_iso_in):
            r_all = self.send_frame(f"plug_signal_format_in_{plug_id}_all_wildcard", [0x01, 0xFF, 0x19, plug_id, 0xFF, 0xFF, 0xFF, 0xFF])
            r_am = self.send_frame(f"plug_signal_format_in_{plug_id}_am824_wildcard", [0x01, 0xFF, 0x19, plug_id, 0x90, 0xFF, 0xFF, 0xFF])
            if r_all.response and r_all.responseCode == 0x0C and len(r_all.response) >= 8:
                valid_input_formats.append((plug_id, r_all.response[4:8]))
            elif r_am.response and r_am.responseCode == 0x0C and len(r_am.response) >= 8:
                valid_input_formats.append((plug_id, r_am.response[4:8]))

        for plug_id in range(num_iso_out):
            r_all = self.send_frame(f"plug_signal_format_out_{plug_id}_all_wildcard", [0x01, 0xFF, 0x18, plug_id, 0xFF, 0xFF, 0xFF, 0xFF])
            r_am = self.send_frame(f"plug_signal_format_out_{plug_id}_am824_wildcard", [0x01, 0xFF, 0x18, plug_id, 0x90, 0xFF, 0xFF, 0xFF])
            if r_all.response and r_all.responseCode == 0x0C and len(r_all.response) >= 8:
                valid_output_formats.append((plug_id, r_all.response[4:8]))
            elif r_am.response and r_am.responseCode == 0x0C and len(r_am.response) >= 8:
                valid_output_formats.append((plug_id, r_am.response[4:8]))

        # ---------------------------------------------------------------------
        # 5. STREAM FORMAT (0x2F and 0xBF)
        # ---------------------------------------------------------------------
        print("\n[5/9] Probing STREAM FORMAT (0x2F and 0xBF opcodes)...")
        unit_answered_2f = False
        valid_single_stream_formats: list[tuple[int, int, list[int], list[int]]] = []  # (address, opcode, plug_addr, format_bytes)

        # Plugs to test:
        # (name, address, plug_bytes_5)
        stream_test_plugs: list[tuple[str, int, list[int]]] = []
        for p in range(num_iso_in):
            stream_test_plugs.append((f"unit_iso_in_{p}", 0xFF, [0x00, 0x00, 0x00, p, 0xFF]))
        for p in range(num_iso_out):
            stream_test_plugs.append((f"unit_iso_out_{p}", 0xFF, [0x01, 0x00, 0x00, p, 0xFF]))

        for (stype, sid), (d_plugs, s_plugs) in subunit_plugs.items():
            if stype == SUBUNIT_MUSIC:
                saddr = (stype << 3) | sid
                for dp in range(d_plugs):
                    stream_test_plugs.append((f"music_{sid}_dest_{dp}", saddr, [0x00, 0x01, dp, 0xFF, 0xFF]))
                for sp in range(s_plugs):
                    stream_test_plugs.append((f"music_{sid}_src_{sp}", saddr, [0x01, 0x01, sp, 0xFF, 0xFF]))

        for plug_label, saddr, plug_bytes in stream_test_plugs:
            for opcode, op_name in ((0x2F, "0x2F"), (0xBF, "0xBF")):
                # Single query
                single_payload = [0x01, saddr, opcode, 0xC0] + plug_bytes + [0xFF]
                single_rec = self.send_frame(f"stream_format_{op_name}_single_{plug_label}", single_payload)
                if single_rec.response and single_rec.responseCode == 0x0C:
                    if opcode == 0x2F:
                        unit_answered_2f = True
                    # format data starts after status byte at operand index 7 (byte 10 of frame)
                    if len(single_rec.response) > 10:
                        valid_single_stream_formats.append((saddr, opcode, plug_bytes, single_rec.response[10:]))

                # List queries 0..31
                for idx in range(32):
                    list_payload = [0x01, saddr, opcode, 0xC1] + plug_bytes + [0xFF, idx]
                    list_rec = self.send_frame(f"stream_format_{op_name}_list_{plug_label}_idx_{idx}", list_payload)
                    if not list_rec.response or list_rec.responseCode != 0x0C:
                        break  # Stop list queries on first non-STABLE
                    if opcode == 0x2F:
                        unit_answered_2f = True

        print(f"  Unit answered 0x2F STREAM FORMAT: {unit_answered_2f}")

        # ---------------------------------------------------------------------
        # 6. SIGNAL SOURCE STATUS
        # ---------------------------------------------------------------------
        print("\n[6/9] Probing SIGNAL SOURCE STATUS for music subunit destination plugs...")
        valid_signal_sources: list[tuple[int, list[int], list[int]]] = []  # (first_byte, source_2, dest_2)
        for (stype, sid), (d_plugs, _) in subunit_plugs.items():
            if stype == SUBUNIT_MUSIC:
                saddr = (stype << 3) | sid
                for dp in range(d_plugs):
                    dest_signal = [saddr, dp]
                    for fb in (0xFF, 0x0F):
                        fb_name = f"0x{fb:02X}"
                        ss_payload = [0x01, 0xFF, 0x1A, fb, 0xFF, 0xFE, dest_signal[0], dest_signal[1]]
                        ss_rec = self.send_frame(f"signal_source_{fb_name}_music_{sid}_dest_{dp}", ss_payload)
                        if ss_rec.response and ss_rec.responseCode == 0x0C and len(ss_rec.response) >= 8:
                            src_bytes = ss_rec.response[4:6]
                            valid_signal_sources.append((fb, src_bytes, dest_signal))

        # ---------------------------------------------------------------------
        # 7. BridgeCo Extended PLUG INFO (only if unit answered 0x2F)
        # ---------------------------------------------------------------------
        if unit_answered_2f:
            print("\n[7/9] Probing BridgeCo extended PLUG INFO...")
            info_types = [
                (0x00, "plug_type"),
                (0x01, "plug_name"),
                (0x02, "channel_count"),
                (0x03, "channel_positions"),
                (0x04, "channel_name"),
                (0x05, "plug_input"),
                (0x06, "plug_outputs"),
            ]
            unit_iso_plugs_5 = []
            for p in range(num_iso_in):
                unit_iso_plugs_5.append((f"in_{p}", [0x00, 0x00, 0x00, p, 0xFF]))
            for p in range(num_iso_out):
                unit_iso_plugs_5.append((f"out_{p}", [0x01, 0x00, 0x00, p, 0xFF]))

            for plug_name, pbytes in unit_iso_plugs_5:
                pos_rec = None
                for itype_val, itype_name in info_types:
                    bco_payload = [0x01, 0xFF, 0x02, 0xC0] + pbytes + [itype_val]
                    r = self.send_frame(f"bridgeco_plug_info_{itype_name}_{plug_name}", bco_payload)
                    if itype_val == 0x03:
                        pos_rec = r

                # Cluster info for section IDs 1..N
                if pos_rec and pos_rec.response and pos_rec.responseCode == 0x0C and len(pos_rec.response) >= 11:
                    # channel positions response layout:
                    # operand 0: 0xC0
                    # operand 1..5: plug address
                    # operand 6: 0x03
                    # operand 7 (byte 10): section count N
                    section_count = pos_rec.response[10]
                    for s_id in range(1, section_count + 1):
                        cluster_payload = [0x01, 0xFF, 0x02, 0xC0] + pbytes + [0x07, s_id, 0x00, 0x00, 0x00, 0x00]
                        self.send_frame(f"bridgeco_cluster_info_{plug_name}_sec_{s_id}", cluster_payload)
        else:
            print("\n[7/9] Skipping BridgeCo extended PLUG INFO (unit did not answer 0x2F).")

        # ---------------------------------------------------------------------
        # 8. FUNCTION BLOCK STATUS (Audio Subunit)
        # ---------------------------------------------------------------------
        audio_subunits = [sid for stype, sid in subunits_found if stype == SUBUNIT_AUDIO]
        valid_selectors: list[tuple[int, int, int]] = []  # (audio_addr, fb_id, input_plug)
        valid_features: list[tuple[int, int, int, list[int]]] = []  # (audio_addr, fb_id, channel, vol_bytes)

        if audio_subunits:
            print("\n[8/9] Probing FUNCTION BLOCK STATUS (Audio subunit)...")
            for asid in audio_subunits:
                audio_addr = (SUBUNIT_AUDIO << 3) | asid

                # Selectors: probe 0, then 1..15 stopping at first NOT IMPLEMENTED
                selector_active = True
                for fbid in range(16):
                    if fbid > 0 and not selector_active:
                        break
                    sel_payload = [0x01, audio_addr, 0xB8, 0x80, fbid, 0x10, 0x02, 0xFF, 0x01]
                    sel_rec = self.send_frame(f"function_block_selector_status_fb_{fbid}", sel_payload)
                    if sel_rec.response and sel_rec.responseCode == 0x0C and len(sel_rec.response) >= 9:
                        in_plug = sel_rec.response[7]
                        valid_selectors.append((audio_addr, fbid, in_plug))
                    elif fbid > 0 and sel_rec.responseCode in (0x08, 0x0A):
                        selector_active = False

                # Features: probe 0, then 1..15 stopping at first NOT IMPLEMENTED
                feature_active = True
                for fbid in range(16):
                    if fbid > 0 and not feature_active:
                        break
                    mute_payload = [0x01, audio_addr, 0xB8, 0x81, fbid, 0x10, 0x02, 0x00, 0x01, 0x01, 0xFF]
                    mute_rec = self.send_frame(f"function_block_feature_mute_status_fb_{fbid}", mute_payload)

                    vol_cur_payload = [0x01, audio_addr, 0xB8, 0x81, fbid, 0x10, 0x02, 0x00, 0x02, 0x02, 0xFF, 0xFF]
                    vol_rec = self.send_frame(f"function_block_feature_volume_current_fb_{fbid}", vol_cur_payload)

                    if vol_rec.response and vol_rec.responseCode == 0x0C and len(vol_rec.response) >= 11:
                        vol_bytes = vol_rec.response[9:11]
                        valid_features.append((audio_addr, fbid, 0x00, vol_bytes))

                        # Also query min / max volume
                        vol_min_payload = [0x01, audio_addr, 0xB8, 0x81, fbid, 0x02, 0x02, 0x00, 0x02, 0x02, 0xFF, 0xFF]
                        self.send_frame(f"function_block_feature_volume_min_fb_{fbid}", vol_min_payload)

                        vol_max_payload = [0x01, audio_addr, 0xB8, 0x81, fbid, 0x03, 0x02, 0x00, 0x02, 0x02, 0xFF, 0xFF]
                        self.send_frame(f"function_block_feature_volume_max_fb_{fbid}", vol_max_payload)
                    elif fbid > 0 and (mute_rec.responseCode in (0x08, 0x0A) or vol_rec.responseCode in (0x08, 0x0A)):
                        feature_active = False
        else:
            print("\n[8/9] Skipping FUNCTION BLOCK STATUS (no audio subunit discovered).")

        # ---------------------------------------------------------------------
        # 9. SPECIFIC INQUIRY (CONTROL forms)
        # ---------------------------------------------------------------------
        print("\n[9/9] Probing SPECIFIC INQUIRY for discovered CONTROL forms...")

        # Plug Signal Format INQUIRY
        for plug_id, fmt_bytes in valid_input_formats:
            inq_payload = [0x02, 0xFF, 0x19, plug_id] + fmt_bytes
            self.send_frame(f"inquiry_plug_signal_format_in_{plug_id}", inq_payload, intent="inquiry")

        for plug_id, fmt_bytes in valid_output_formats:
            inq_payload = [0x02, 0xFF, 0x18, plug_id] + fmt_bytes
            self.send_frame(f"inquiry_plug_signal_format_out_{plug_id}", inq_payload, intent="inquiry")

        # Stream Format Single INQUIRY
        for saddr, opcode, pbytes, fmt_bytes in valid_single_stream_formats:
            op_name = f"0x{opcode:02X}"
            inq_payload = [0x02, saddr, opcode, 0xC0] + pbytes + [0x00] + fmt_bytes
            self.send_frame(f"inquiry_stream_format_{op_name}_single", inq_payload, intent="inquiry")

        # Signal Source INQUIRY
        for fb, src, dst in valid_signal_sources:
            fb_name = f"0x{fb:02X}"
            inq_payload = [0x02, 0xFF, 0x1A, fb, src[0], src[1], dst[0], dst[1]]
            self.send_frame(f"inquiry_signal_source_{fb_name}", inq_payload, intent="inquiry")

        # Selector INQUIRY
        for saddr, fbid, in_plug in valid_selectors:
            inq_payload = [0x02, saddr, 0xB8, 0x80, fbid, 0x10, 0x02, in_plug, 0x01]
            self.send_frame(f"inquiry_function_block_selector_fb_{fbid}", inq_payload, intent="inquiry")

        # Feature Mute & Volume INQUIRY
        for saddr, fbid, ch, vol_bytes in valid_features:
            # Mute On
            inq_mute_on = [0x02, saddr, 0xB8, 0x81, fbid, 0x10, 0x02, ch, 0x01, 0x01, 0x70]
            self.send_frame(f"inquiry_feature_mute_on_fb_{fbid}_ch_{ch}", inq_mute_on, intent="inquiry")
            # Mute Off
            inq_mute_off = [0x02, saddr, 0xB8, 0x81, fbid, 0x10, 0x02, ch, 0x01, 0x01, 0x60]
            self.send_frame(f"inquiry_feature_mute_off_fb_{fbid}_ch_{ch}", inq_mute_off, intent="inquiry")
            # Volume
            inq_vol = [0x02, saddr, 0xB8, 0x81, fbid, 0x10, 0x02, ch, 0x02, 0x02, vol_bytes[0], vol_bytes[1]]
            self.send_frame(f"inquiry_feature_volume_fb_{fbid}_ch_{ch}", inq_vol, intent="inquiry")

    def post_check(self) -> None:
        print("\nRunning post-check...")
        # 1. Health
        health_res = self.client.read_resource("asfw://control-plane/health")
        health_data = health_res.get("data", health_res) if isinstance(health_res, dict) else {}
        if health_data.get("status") != "ready":
            raise ProbeError(f"Post-check failed: driver health is {health_data.get('status')}")

        # 2. Same generation
        post_gen = health_res.get("generation") or health_data.get("expectedGeneration")
        if not post_gen:
            telem_res = self.client.read_resource("asfw://telemetry/snapshot")
            post_gen = telem_res.get("generation", 1)
        if post_gen != self.metadata.generation:
            raise ProbeError(f"Post-check failed: bus generation shifted from {self.metadata.generation} to {post_gen}")

        # 3. PLUG INFO still answers
        test_rec = self.send_frame("post_check_plug_info", [0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
        if not test_rec.response or test_rec.responseCode != 0x0C:
            raise ProbeError("Post-check failed: PLUG INFO no longer answers STABLE.")

        print("Post-check PASSED: bus stable, health ready, device responding.")


def export_fixtures(runner: AvcProbeRunner, out_dir: Path) -> tuple[Path, Path]:
    meta = runner.metadata
    assert meta is not None

    json_path = out_dir / f"{meta.key}.json"
    md_path = out_dir / f"{meta.key}.md"

    # Export JSON
    payload = {
        "device": asdict(meta),
        "totalRecords": len(runner.records),
        "records": [asdict(r) for r in runner.records],
    }
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)

    # Export Markdown matrix
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(f"# AV/C Hardware Probe Matrix: {meta.vendorName} {meta.modelName}\n\n")
        f.write(f"- **Key**: `{meta.key}`\n")
        f.write(f"- **GUID**: `{meta.guid}`\n")
        f.write(f"- **Node ID**: `{meta.nodeId}` (generation {meta.generation})\n")
        f.write(f"- **Driver Version**: `{meta.driverVersion}`\n")
        f.write(f"- **Captured At**: `{meta.capturedAt}`\n")
        f.write(f"- **Total Exchanges**: `{len(runner.records)}`\n\n")

        f.write("| Exchange | Intent | Command (Hex) | Response Code | Duration (µs) | Result |\n")
        f.write("|---|---|---|---|---|---|\n")

        code_names = {
            0x08: "NOT IMPLEMENTED (0x08)",
            0x09: "ACCEPTED (0x09)",
            0x0A: "REJECTED (0x0A)",
            0x0B: "IN PROCESS (0x0B)",
            0x0C: "STABLE (0x0C)",
        }

        for r in runner.records:
            cmd_hex = " ".join(f"{b:02X}" for b in r.command)
            intent = "STATUS" if r.command[0] == 0x01 else ("INQUIRY" if r.command[0] == 0x02 else "CONTROL")
            code_str = code_names.get(r.responseCode, f"0x{r.responseCode:02X}" if r.responseCode is not None else "NO_RESPONSE")
            dur_str = f"{r.durationUsec}" if r.durationUsec is not None else "-"
            res_str = "OK" if r.ok else f"ERR ({r.status})"
            f.write(f"| `{r.name}` | {intent} | `{cmd_hex}` | {code_str} | {dur_str} | {res_str} |\n")

    return json_path, md_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, help="ASFW MCP endpoint")
    parser.add_argument("--device", help="Explicit device key (e.g. duet, phase88, 1814)")
    parser.add_argument("--timeout", type=float, default=5.0, help="Per-request timeout seconds")
    parser.add_argument("--out-dir", default="documentation/fixtures/AVC", help="Output directory for fixtures")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print every frame sent/received")
    args = parser.parse_args()

    client = MCPClient(endpoint=args.endpoint, timeout=args.timeout)
    try:
        client.connect()
    except ProbeError as e:
        print(f"Error connecting to MCP: {e}", file=sys.stderr)
        return 1

    runner = AvcProbeRunner(client, verbose=args.verbose)
    try:
        meta = runner.pre_check(device_key_override=args.device)
        print(f"Target node identified: {meta.vendorName} {meta.modelName} (nodeId {meta.nodeId}, generation {meta.generation})")

        runner.run_battery()
        runner.post_check()

        out_path = Path(args.out_dir)
        out_path.mkdir(parents=True, exist_ok=True)
        json_file, md_file = export_fixtures(runner, out_path)
        print(f"\nProbe complete! Exported fixtures:")
        print(f"  JSON: {json_file}")
        print(f"  Markdown: {md_file}")
        return 0

    except ProbeWedgeError as e:
        print(f"\n[!] PROBE WEDGE DETECTED: {e}", file=sys.stderr)
        # Still dump records collected so far if any
        if runner.metadata:
            out_path = Path(args.out_dir)
            out_path.mkdir(parents=True, exist_ok=True)
            export_fixtures(runner, out_path)
            print(f"Wedge dump written to {out_path}/{runner.metadata.key}.json")
        return 2
    except ProbeError as e:
        print(f"Probe error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
