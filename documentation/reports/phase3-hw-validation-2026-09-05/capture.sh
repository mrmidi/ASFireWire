#!/bin/bash
# Capture the driver-side Phase 3 telemetry while a stream is running.
#
# This drives ~20 s of audio through the RTL tool so the 5 s [Ledger]/[TxFill]
# heartbeats fire. It does NOT capture them: ASFW_LOG writes the driver's own
# bounded ring and only mirrors to os_log when that mirror is enabled, so a
# `log stream` predicate matches nothing. Read the counters afterwards with the
# MCP control plane instead:
#
#   export ASFW_MCP_ENDPOINT=http://127.0.0.1:8765/mcp
#   python3 .claude/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py \
#     call asfw_log_query '{"categories":["DirectAudio"],"contains":"[Ledger]","maxRecords":200}'
#
# No loopback cable is required: every counter here is driver-side. The RTL
# tool is only being used as a well-behaved duplex client.
set -u
cd "$(dirname "$0")/../../.."
OUT="documentation/reports/phase3-hw-validation-2026-09-05/evidence"

echo "running 60 RTL trials (~20 s of streaming audio) ..."
tools/rtl/rtl_loopback -d Duet --frames 64 --trials 60 --measure \
    > "$OUT/rtl-during-capture.txt" 2>&1

sleep 6          # let one more heartbeat land after IO stops

echo "done. Now read the ring via asfw_log_query (see header)."
