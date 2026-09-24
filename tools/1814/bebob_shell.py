#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 ASFireWire Project
#
# Call the BeBoB shell MCP tool. It checks the current generation and 1814
# identity before sending Virtual UART mailbox transactions. Shell commands can
# change device settings; the MCP developer-write gate applies.

import argparse
import json
import sys
import urllib.error
import urllib.request

DEFAULT_ENDPOINT = "http://127.0.0.1:8766/mcp"


class BeBoBTools:
    def __init__(self, endpoint, node, generation):
        self.endpoint = endpoint
        self.node = node
        self.generation = generation
        self.session = self._initialize()

    def _post(self, payload, headers=None):
        request_headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if headers:
            request_headers.update(headers)
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(payload).encode(),
            headers=request_headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return dict(response.headers), response.read().decode()
        except urllib.error.HTTPError as error:
            return dict(error.headers), error.read().decode() if error.fp else ""

    def _initialize(self):
        headers, _ = self._post({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "bebob_shell", "version": "1"},
            },
        })
        session = next((value for key, value in headers.items()
                        if key.lower() == "mcp-session-id"), None)
        if not session:
            sys.exit(f"no MCP session from {self.endpoint}; is ASFW.app running?")
        return session

    def execute(self, command):
        command = command.strip()
        if not command or len(command.encode("utf-8")) > 1024 or any(
            ord(character) < 0x20 or ord(character) > 0x7e for character in command
        ):
            raise ValueError("command must be one printable ASCII line (1–1024 bytes)")

        _, body = self._post(
            {
                "jsonrpc": "2.0",
                "id": 99,
                "method": "tools/call",
                "params": {
                    "name": "asfw_bebob_shell_execute",
                    "arguments": {
                        "nodeId": self.node,
                        "generation": self.generation,
                        "command": command,
                    },
                },
            },
            {"Mcp-Session-Id": self.session},
        )
        for line in body.splitlines():
            if not line.startswith("data: "):
                continue
            try:
                message = json.loads(line[6:])
            except json.JSONDecodeError:
                continue
            result = message.get("result", {})
            for item in result.get("content", []):
                if item.get("type") != "text":
                    continue
                try:
                    value = json.loads(item["text"])
                except (KeyError, json.JSONDecodeError):
                    continue
                data = value.get("data", {})
                if value.get("ok") and isinstance(data, dict):
                    return data.get("stdout", "")
                details = value.get("errors", [])
                reason = details[0].get("reason") if details else "MCP tool call failed"
                raise RuntimeError(reason)
        raise RuntimeError("MCP response did not contain a BeBoB shell result")


def main():
    parser = argparse.ArgumentParser(
        description="Run M-Audio 1814 Virtual UART shell commands through ASFW MCP."
    )
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument("--node", type=int, required=True,
                        help="current FireWire node ID")
    parser.add_argument("--gen", type=int, required=True,
                        help="current bus generation")
    parser.add_argument("commands", nargs="+",
                        help="one or more shell command lines")
    args = parser.parse_args()

    client = BeBoBTools(args.endpoint, args.node, args.gen)
    for command in args.commands:
        print(f"\n$ {command}")
        try:
            print(client.execute(command))
        except (ValueError, RuntimeError) as error:
            parser.error(str(error))


if __name__ == "__main__":
    main()
