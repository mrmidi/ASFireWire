#!/usr/bin/env python3
"""Hardware probe tool specifically for M-Audio FireWire 1814 and ProjectMix I/O.

Unlike standard AV/C and BridgeCo devices, the FW 1814 and ProjectMix I/O run
custom DM1000 firmware with a fragile AV/C parser. Standard discovery commands
will FREEZE the hardware, requiring a physical power cycle.

Strict Client-Side Allowlist:
  This probe enforces an exact client-side mirror of the driver's kernel-side
  allowlist (kMAudioSpecialPermittedFrames in AVCCommandFilter.hpp). Every AV/C
  frame is validated against masked prefix rules before being submitted to FCP.
  Any frame outside this allowlist is rejected immediately before transmission.

What Will FREEZE the 1814 / ProjectMix (DO NOT SEND):
  ❌ SUBUNIT_INFO (0x31)
  ❌ PLUG_INFO (0x02)
  ❌ Extended Stream Format (0x2F / 0xBF, subfunctions 0xC0 / 0xC1)
  ❌ SIGNAL SOURCE (0x1A)
  ❌ UNIT_INFO (0x30)
  ❌ BridgeCo Extended Plug Info (0x02 subfunction 0xC0)
  ❌ Standard Feature Blocks (FB 0, FB 1..3 mute/volume)
  ❌ Status queries for proprietary vendor commands (LED, Clock 04 00 04)

What this probe safely inspects:
  1. BootROM / Software Info Register (Async memory read at 0xFFC700000000)
  2. Hardware Peak Meters & Clock Sync Status (Async block read at 0xFFC700600000, 84 bytes)
  3. Hardware Mixer Matrix snapshot (Async block read at 0xFFC700700000, 160 bytes)
  4. AV/C Plug Signal Format Status (0x18 / 0x19 with FMT pinned to 0x90 AM824)
  5. (Optional, --include-control) Proven Vendor Controls:
     - Front-panel LED toggle (CONTROL 00 FF 00 03 00 01 [01/00] 00)
     - Blank-slate input selector (CONTROL 00 08 B8 80 04 10 02 00 01 00 00 00)
     - Internal Clock selection (CONTROL 16-byte frame, lock=0x00 per Linux bebob_maudio.c:276)
     - Sample Rate switching via CONTROL signal format
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

# DM1000 memory map for M-Audio special firmware
MAUDIO_SPECIAL_ADDR_HIGH = 0xFFC7
MAUDIO_INFO_ADDR_LOW = 0x00000000
MAUDIO_METER_ADDR_LOW = 0x00600000
MAUDIO_MIXER_ADDR_LOW = 0x00700000

METER_BUFFER_SIZE = 84
MIXER_BUFFER_SIZE = 160

# AM824 / IEC 61883-6 Sampling Frequency Code (SFC) Table
SFC_TABLE: dict[int, str] = {
    0x00: "32.0 kHz",
    0x01: "44.1 kHz",
    0x02: "48.0 kHz",
    0x03: "88.2 kHz",
    0x04: "96.0 kHz",
    0x05: "176.4 kHz",
    0x06: "192.0 kHz",
}

# ==============================================================================
# Strict Client-Side Allowlist (Mirror of kMAudioSpecialPermittedFrames)
# ==============================================================================

PERMITTED_FRAMES: list[dict[str, Any]] = [
    # 1. STATUS, unit, INPUT PLUG SIGNAL FORMAT (length 8, FMT pinned to 0x90 AM824)
    #    references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:302-313
    {
        "name": "sig-fmt STATUS input plug",
        "length": 8,
        "prefix": [0x01, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00],
    },
    # 2. STATUS, unit, OUTPUT PLUG SIGNAL FORMAT (length 8, FMT pinned to 0x90 AM824)
    #    references/alsa-userspace-control-protocols-impl/protocols/bebob/src/maudio/special.rs:101-119
    {
        "name": "sig-fmt STATUS output plug",
        "length": 8,
        "prefix": [0x01, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00],
    },
    # 3. CONTROL, unit, INPUT PLUG SIGNAL FORMAT (length 8, FMT pinned to 0x90 AM824)
    #    references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:315-338
    {
        "name": "sig-fmt CONTROL input plug",
        "length": 8,
        "prefix": [0x00, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00],
    },
    # 4. CONTROL, unit, OUTPUT PLUG SIGNAL FORMAT (length 8, FMT pinned to 0x90 AM824)
    #    references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:315-338
    {
        "name": "sig-fmt CONTROL output plug",
        "length": 8,
        "prefix": [0x00, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00],
    },
    # 5. M-Audio vendor clock/format (16 bytes, OUI 0x04 0x00 0x04)
    #    references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:186-198, 276
    #    vendor kext SetClockSourceInternal @ 0xe25c
    {
        "name": "M-Audio vendor clock/format",
        "length": 16,
        "prefix": [0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                   0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF],
    },
    # 6. M-Audio blank-slate input selector (12 bytes, audio subunit 0, FB 4)
    #    vendor kext SetClockSourceInternal @ 0xe45a -> base @ 0x23148
    #    references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:461-462,514
    {
        "name": "M-Audio blank-slate input selector",
        "length": 12,
        "prefix": [0x00, 0x08, 0xB8, 0x80, 0x04, 0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF],
    },
    # 7. M-Audio front-panel LED (8 bytes, OUI 0x03 0x00 0x01)
    #    vendor kext AVCControlSetLEDStatus @ 0xdaaa
    #    references/alsa-userspace-control-protocols-impl/protocols/bebob/src/maudio/special.rs:121-167
    {
        "name": "M-Audio LED",
        "length": 8,
        "prefix": [0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00],
        "care":   [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF],
    },
]


def frame_matches(rule: dict[str, Any], payload: list[int]) -> bool:
    if len(payload) != rule["length"]:
        return False
    for p_byte, c_byte, byte in zip(rule["prefix"], rule["care"], payload):
        if (byte & c_byte) != (p_byte & c_byte):
            return False
    return True


def frame_is_permitted(payload: list[int]) -> tuple[bool, str]:
    for rule in PERMITTED_FRAMES:
        if frame_matches(rule, payload):
            return True, rule["name"]
    return False, ""


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
                "clientInfo": {"name": "ASFW FW1814 Probe", "version": "1.0"},
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
    vendorId: int
    modelId: int
    guid: str
    guidUint64: int
    nodeId: int
    generation: int
    driverVersion: str
    capturedAt: str
    is1814: bool = True
    softwareDate: str | None = None


class FW1814ProbeRunner:
    def __init__(
        self,
        client: MCPClient,
        verbose: bool = False,
        force: bool = False,
        include_control: bool = False,
        allow_clock_write: bool = False,
    ) -> None:
        self.client = client
        self.verbose = verbose
        self.force = force
        self.include_control = include_control
        self.allow_clock_write = allow_clock_write
        self.records: list[ExchangeRecord] = []
        self.metadata: DeviceMetadata | None = None
        self.register_dumps: dict[str, Any] = {}

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

        target_node = None
        for n in node_list:
            guid_str = n.get("guid", "0x0")
            guid_int = int(guid_str, 16) if guid_str.startswith("0x") else int(guid_str)
            vendor_id = n.get("vendorId", (guid_int >> 40) & 0xFFFFFF)
            model_id = n.get("modelId", 0)
            model = n.get("modelName", "").lower()
            if (
                (vendor_id == 0x000D6C and model_id in (0x00010071, 0x00010091))
                or n.get("avcCommandFilter") == "MAudioSpecialBeBoB"
                or "1814" in model
                or "projectmix" in model
            ):
                target_node = n
                break

        if target_node is None:
            if not self.force:
                models_found = [f"{n.get('vendorName')} {n.get('modelName')}" for n in node_list]
                raise ProbeError(
                    f"No M-Audio FW 1814 or ProjectMix I/O found on bus.\n"
                    f"Discovered nodes: {', '.join(models_found)}\n"
                    f"Pass --force if you intentionally wish to run this probe against another node."
                )
            target_node = node_list[0]

        guid_str = target_node.get("guid", "0x0")
        guid_int = int(guid_str, 16) if guid_str.startswith("0x") else int(guid_str)
        model_name = target_node.get("modelName", "FireWire 1814")
        vendor_name = target_node.get("vendorName", "M-Audio")
        node_id = target_node.get("nodeId", 0)
        vendor_id = target_node.get("vendorId", (guid_int >> 40) & 0xFFFFFF)
        model_id = target_node.get("modelId", 0)

        if not generation:
            telem_res = self.client.read_resource("asfw://telemetry/snapshot")
            generation = telem_res.get("generation", 1)

        key = device_key_override
        if not key:
            key = "maudio-fw1814" if "1814" in model_name.lower() or model_id == 0x00010071 else "maudio-projectmix"

        self.metadata = DeviceMetadata(
            key=key,
            modelName=model_name,
            vendorName=vendor_name,
            vendorId=vendor_id,
            modelId=model_id,
            guid=f"0x{guid_int:016X}",
            guidUint64=guid_int,
            nodeId=node_id,
            generation=generation,
            driverVersion=driver_ver,
            capturedAt=datetime.now(timezone.utc).isoformat(),
            is1814=("1814" in model_name.lower() or model_id == 0x00010071),
        )
        return self.metadata

    def send_frame(self, name: str, payload: list[int], intent: str = "status") -> ExchangeRecord:
        """Sends an AV/C frame strictly guarded by the M-Audio special allowlist."""
        if self.metadata is None:
            raise ProbeError("Runner has not passed pre_check.")

        if len(payload) < 3:
            raise ProbeError(f"Payload too short ({len(payload)} bytes): {payload}")

        permitted, rule_name = frame_is_permitted(payload)
        if not permitted:
            cmd_hex = " ".join(f"{b:02X}" for b in payload)
            raise ProbeError(
                f"SAFETY ALLOWLIST REFUSAL: Frame '{name}' ({cmd_hex}) is NOT in the M-Audio special allowlist!\n"
                f"Sending unpermitted commands to 1814/ProjectMix hardware will FREEZE the DM1000 firmware."
            )

        ctype = payload[0]
        tool_name = "asfw_fcp_send_command_dev" if ctype == 0x00 else "asfw_fcp_send_command"

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
            print(f"  [FCP] {name:<35} : {cmd_hex} (rule: {rule_name})")

        result = self.client.call_tool(tool_name, args)
        ok = result.get("ok", False)
        data = result.get("data", {})
        status = data.get("status", "")
        response = data.get("response")
        duration = data.get("durationUsec")
        errors = result.get("errors", [])
        err_msg = errors[0].get("reason") if errors else None

        if not ok and (status in ("timeout", "unavailable", "wedged") or "timeout" in str(err_msg).lower()):
            record = ExchangeRecord(
                name=name,
                command=payload,
                ok=False,
                status=status or "wedge_timeout",
                error=err_msg or "Transport timed out or wedged",
            )
            self.records.append(record)
            raise ProbeWedgeError(f"Device appears wedged on command '{name}': {err_msg}")

        resp_code = response[0] if (response and len(response) > 0) else None
        record = ExchangeRecord(
            name=name,
            command=payload,
            response=response,
            responseCode=resp_code,
            durationUsec=duration,
            ok=ok,
            status=status or ("ok" if ok else "error"),
            error=err_msg,
        )
        self.records.append(record)
        if self.verbose and response:
            resp_hex = " ".join(f"{b:02X}" for b in response)
            print(f"  [<] {' ' * 35} : {resp_hex} ({duration or 0} µs)")
        return record

    def read_register_block(self, name: str, addr_high: int, addr_low: int, length: int) -> list[int] | None:
        """Reads a memory-mapped block via IEEE 1394 async block read (stored in register_dumps, not records)."""
        if self.metadata is None:
            raise ProbeError("Runner has not passed pre_check.")

        args = {
            "nodeId": self.metadata.nodeId,
            "generation": self.metadata.generation,
            "addressHigh": addr_high,
            "addressLow": addr_low,
            "length": length,
        }

        if self.verbose:
            print(f"  [MMIO] {name:<33} : ReadBlock 0x{addr_high:04X}:{addr_low:08X} ({length} bytes)")

        result = self.client.call_tool("asfw_read_device_register_block", args)
        ok = result.get("ok", False)
        data = result.get("data", {})
        payload = data.get("payload")
        if ok and payload:
            self.register_dumps[name] = payload
            if self.verbose:
                print(f"  [<] {' ' * 35} : Read {len(payload)} bytes successfully")
            return payload
        else:
            errors = result.get("errors", [])
            err_msg = errors[0].get("reason") if errors else "Read block failed"
            if self.verbose:
                print(f"  [!] {' ' * 35} : Failed: {err_msg}")
            return None

    def probe_info_registers(self) -> None:
        """Reads DM1000 BootROM / software build info."""
        print("-> Probing DM1000 Info Registers (0xFFC7:00000000)...")
        block = self.read_register_block("dm1000_info_header", MAUDIO_SPECIAL_ADDR_HIGH, MAUDIO_INFO_ADDR_LOW, 64)
        if block and len(block) >= 40:
            # Offset 0x20 has build date ASCII string (e.g. "20070713")
            date_bytes = bytes(block[32:40])
            try:
                date_str = date_bytes.decode("ascii", errors="ignore").strip()
                if self.metadata:
                    self.metadata.softwareDate = date_str
                print(f"   [+] DM1000 Firmware Build Date: {date_str}")
            except Exception:
                pass

    def probe_meters_and_sync(self) -> None:
        """Reads the 84-byte hardware peak meter and sync lock buffer."""
        print(f"-> Reading Hardware Meter Block (0xFFC7:{MAUDIO_METER_ADDR_LOW:08X}, {METER_BUFFER_SIZE} bytes)...")
        block = self.read_register_block("hardware_meters_84b", MAUDIO_SPECIAL_ADDR_HIGH, MAUDIO_METER_ADDR_LOW, METER_BUFFER_SIZE)
        if block and len(block) >= METER_BUFFER_SIZE:
            # Byte 0..3: Controls
            rot1 = (block[0] >> 6) & 3
            rot2 = (block[0] >> 4) & 3
            rot3 = (block[0] >> 2) & 3
            sw = block[0] & 3
            print(f"   [+] Front Panel Encoders: Rot1={rot1}, Rot2={rot2}, Rot3={rot3}, Switch={sw}")

            # Trailing quadlet: Sync Lock & External Rate
            sync_val = (block[80] << 24) | (block[81] << 16) | (block[82] << 8) | block[83]
            is_locked = (sync_val & 0x08) != 0
            rate_code = (sync_val >> 8) & 0x07
            rate_map = {0: "44.1k", 1: "48k", 2: "88.2k", 3: "96k", 4: "176.4k", 5: "192k"}
            detected_rate = rate_map.get(rate_code, f"unknown ({rate_code})")
            print(f"   [+] Clock Sync Status: Locked={is_locked}, Detected Rate={detected_rate}")

    def probe_mixer_matrix(self) -> None:
        """Reads the 160-byte hardware mixer registers."""
        print(f"-> Reading Hardware Mixer Matrix (0xFFC7:{MAUDIO_MIXER_ADDR_LOW:08X}, {MIXER_BUFFER_SIZE} bytes)...")
        self.read_register_block("mixer_matrix_160b", MAUDIO_SPECIAL_ADDR_HIGH, MAUDIO_MIXER_ADDR_LOW, MIXER_BUFFER_SIZE)

    def probe_safe_avc(self) -> None:
        """Probes safe, read-only AV/C frames strictly matching kMAudioSpecialPermittedFrames."""
        print("-> Probing Safe AV/C Commands (Allowlist Enforced)...")

        # 1. Input Plug 0 Signal Format (Status - safe per Linux bebob_maudio.c:308)
        #    Byte 4 pinned to 0x90 (AM824) to match driver filter care mask
        rec_in0 = self.send_frame("input_plug_0_signal_format_status", [0x01, 0xFF, 0x19, 0x00, 0x90, 0xFF, 0xFF, 0xFF])
        if rec_in0.ok and rec_in0.response and len(rec_in0.response) >= 6:
            sfc = rec_in0.response[5] & 0x07
            rate_str = SFC_TABLE.get(sfc, f"unknown (0x{sfc:02X})")
            print(f"   [+] Input Plug 0 Signal Format: Rate={rate_str} (SFC=0x{sfc:02X})")

        # 2. Output Plug 0 Signal Format (Status)
        rec_out0 = self.send_frame("output_plug_0_signal_format_status", [0x01, 0xFF, 0x18, 0x00, 0x90, 0xFF, 0xFF, 0xFF])
        if rec_out0.ok and rec_out0.response and len(rec_out0.response) >= 6:
            sfc = rec_out0.response[5] & 0x07
            rate_str = SFC_TABLE.get(sfc, f"unknown (0x{sfc:02X})")
            print(f"   [+] Output Plug 0 Signal Format: Rate={rate_str} (SFC=0x{sfc:02X})")

    def probe_optional_controls(self) -> None:
        """Runs state-modifying vendor control tests if enabled (--include-control)."""
        if not self.include_control:
            return

        print("-> Running State-Modifying Vendor Control Tests (--include-control enabled)...")

        # Read current rate first to restore it at the end
        rec_orig = self.send_frame("pre_control_output_rate_status", [0x01, 0xFF, 0x18, 0x00, 0x90, 0xFF, 0xFF, 0xFF])
        orig_sfc = None
        if rec_orig.ok and rec_orig.response and len(rec_orig.response) >= 6:
            orig_sfc = rec_orig.response[5] & 0x07
            orig_name = SFC_TABLE.get(orig_sfc, f"0x{orig_sfc:02X}")
            print(f"   [+] Pre-control output sample rate: {orig_name} (SFC=0x{orig_sfc:02X})")

        # 1. Front-panel LED test: toggle ON, then OFF (CONTROL 8 bytes, OUI 0x03 0x00 0x01)
        print("   [+] Toggling LED ON...")
        self.send_frame("vendor_led_control_on", [0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x01, 0x00], intent="control")
        time.sleep(0.5)
        print("   [+] Toggling LED OFF...")
        self.send_frame("vendor_led_control_off", [0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00], intent="control")

        # 2. Blank-slate input selector (CONTROL 12 bytes, Audio Subunit 0, FB 4)
        print("   [+] Sending blank-slate input selector...")
        self.send_frame(
            "blank_slate_input_selector",
            [0x00, 0x08, 0xB8, 0x80, 0x04, 0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00],
            intent="control",
        )

        # 3. Clock Source control (16 bytes, lock = 0x00 per Linux bebob_maudio.c:276)
        #    Internal clock = 0x03, S/PDIF input = 0x00, S/PDIF output = 0x00, lock = 0x00
        #    NOTE: This write alters internal clock and resets dig_in_fmt/dig_out_fmt,
        #    which select the formation table (hazard doc H5).
        if self.allow_clock_write:
            print("   [!] WARNING: Writing vendor clock control changes device internal clock and resets dig_in_fmt/dig_out_fmt routing!")
            print("   [+] Setting Clock Source (Internal, Unlocked, 16 bytes)...")
            self.send_frame(
                "vendor_clock_control_internal",
                [0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
                intent="control",
            )
        else:
            print("   [-] Skipping clock source write: requires --i-understand-this-changes-clock (resets digital formation table routing)")

        # 4. Signal Format Rate Switching via CONTROL (AM824 format 0x90)
        #    SFC indices: 0x00=32k, 0x01=44.1k, 0x02=48k, 0x03=88.2k, 0x04=96k, 0x05=176.4k, 0x06=192k
        #    Linux sets OUT plug first, then sleeps 100ms, then sets IN plug.
        rates = [
            ("32k", 0x00),
            ("44k1", 0x01),
            ("48k", 0x02),
            ("88k2", 0x03),
            ("96k", 0x04),
            ("176k4", 0x05),
            ("192k", 0x06),
        ]
        for name, sfc in rates:
            # Skip 176.4k and 192k on ProjectMix (max rate is 96k)
            if self.metadata and not self.metadata.is1814 and sfc >= 0x05:
                continue
            print(f"   [+] Testing Sample Rate CONTROL: {name} (SFC=0x{sfc:02X})...")
            self.send_frame(
                f"control_output_rate_{name}",
                [0x00, 0xFF, 0x18, 0x00, 0x90, sfc, 0xFF, 0xFF],
                intent="control",
            )
            time.sleep(0.1)  # 100 ms settle delay matching Linux bebob_maudio.c:328
            self.send_frame(
                f"control_input_rate_{name}",
                [0x00, 0xFF, 0x19, 0x00, 0x90, sfc, 0xFF, 0xFF],
                intent="control",
            )

        # 5. Restore original sample rate if one was captured
        if orig_sfc is not None:
            orig_name = SFC_TABLE.get(orig_sfc, f"0x{orig_sfc:02X}")
            print(f"   [+] Restoring original sample rate: {orig_name} (SFC=0x{orig_sfc:02X})...")
            self.send_frame(
                "restore_output_rate",
                [0x00, 0xFF, 0x18, 0x00, 0x90, orig_sfc, 0xFF, 0xFF],
                intent="control",
            )
            time.sleep(0.1)
            self.send_frame(
                "restore_input_rate",
                [0x00, 0xFF, 0x19, 0x00, 0x90, orig_sfc, 0xFF, 0xFF],
                intent="control",
            )


def export_fixtures(runner: FW1814ProbeRunner, out_dir: Path) -> tuple[Path, Path]:
    if runner.metadata is None:
        raise ProbeError("Cannot export fixtures without metadata.")

    out_dir.mkdir(parents=True, exist_ok=True)
    slug = runner.metadata.key
    json_path = out_dir / f"{slug}.json"
    md_path = out_dir / f"{slug}.md"

    # 1. JSON Export
    data: dict[str, Any] = {
        "device": asdict(runner.metadata),
        "totalRecords": len(runner.records),
        "records": [asdict(r) for r in runner.records],
    }
    if runner.register_dumps:
        data["registerDumps"] = runner.register_dumps

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    # 2. Markdown Export
    with open(md_path, "w", encoding="utf-8") as f:
        f.write("# M-Audio FireWire 1814 / ProjectMix Probe Fixture\n\n")
        f.write(f"- **Device Key**: `{runner.metadata.key}`\n")
        f.write(f"- **Model**: `{runner.metadata.modelName}`\n")
        f.write(f"- **Vendor**: `{runner.metadata.vendorName}`\n")
        f.write(f"- **Vendor ID**: `0x{runner.metadata.vendorId:06X}`\n")
        f.write(f"- **Model ID**: `0x{runner.metadata.modelId:08X}`\n")
        f.write(f"- **GUID**: `{runner.metadata.guid}`\n")
        f.write(f"- **Node ID**: `{runner.metadata.nodeId}` (Gen {runner.metadata.generation})\n")
        f.write(f"- **Driver Version**: `{runner.metadata.driverVersion}`\n")
        if runner.metadata.softwareDate:
            f.write(f"- **Firmware Date**: `{runner.metadata.softwareDate}`\n")
        f.write(f"- **Captured At**: `{runner.metadata.capturedAt}`\n")
        f.write(f"- **Total Records**: {len(runner.records)}\n\n")

        f.write("## AV/C Transactions\n\n")
        f.write("| # | Name | Command | Response | Code | Duration | Status |\n")
        f.write("|---|------|---------|----------|------|----------|--------|\n")
        for i, r in enumerate(runner.records, 1):
            cmd_h = " ".join(f"{b:02X}" for b in r.command)
            resp_h = " ".join(f"{b:02X}" for b in r.response) if r.response else "-"
            code_h = f"0x{r.responseCode:02X}" if r.responseCode is not None else "-"
            dur_s = f"{r.durationUsec} µs" if r.durationUsec is not None else "-"
            stat_s = "✅ ok" if r.ok else f"❌ {r.status}"
            f.write(f"| {i} | `{r.name}` | `{cmd_h}` | `{resp_h}` | `{code_h}` | {dur_s} | {stat_s} |\n")

        if runner.register_dumps:
            f.write("\n## MMIO Register Dumps\n\n")
            for reg_name, payload in runner.register_dumps.items():
                f.write(f"### `{reg_name}` ({len(payload)} bytes)\n\n```\n")
                for offset in range(0, len(payload), 16):
                    chunk = payload[offset:offset+16]
                    hex_str = " ".join(f"{b:02X}" for b in chunk)
                    f.write(f"{offset:04X}: {hex_str}\n")
                f.write("```\n\n")

    return json_path, md_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, help="ASFW MCP endpoint URL")
    parser.add_argument("--device-key", default=None, help="Device slug override")
    parser.add_argument("--out-dir", default="documentation/fixtures/AVC", help="Output directory for fixtures")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print raw commands and responses")
    parser.add_argument("--force", action="store_true", help="Bypass 1814/ProjectMix model name safety check")
    parser.add_argument(
        "--include-control",
        action="store_true",
        help="Run state-modifying vendor control tests (LED, rate sweep and restoration)",
    )
    parser.add_argument(
        "--i-understand-this-changes-clock",
        action="store_true",
        help="Explicitly authorize vendor clock source write (resets digital format routing and internal clock)",
    )

    args = parser.parse_args()

    client = MCPClient(args.endpoint)
    try:
        client.connect()
    except ProbeError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    runner = FW1814ProbeRunner(
        client,
        verbose=args.verbose,
        force=args.force,
        include_control=args.include_control,
        allow_clock_write=args.i_understand_this_changes_clock,
    )

    try:
        meta = runner.pre_check(args.device_key)
        print(f"Connected to '{meta.modelName}' ({meta.vendorName}) at Node {meta.nodeId} (Gen {meta.generation})")

        runner.probe_info_registers()
        runner.probe_meters_and_sync()
        runner.probe_mixer_matrix()
        runner.probe_safe_avc()
        runner.probe_optional_controls()

        out_path = Path(args.out_dir)
        json_file, md_file = export_fixtures(runner, out_path)
        print(f"\nProbe complete! Exported fixtures:")
        print(f"  JSON: {json_file}")
        print(f"  MD:   {md_file}")
        return 0

    except ProbeWedgeError as error:
        print(f"\nCRITICAL: Device wedged: {error}", file=sys.stderr)
        out_path = Path(args.out_dir)
        if runner.metadata:
            export_fixtures(runner, out_path)
        return 2
    except ProbeError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
