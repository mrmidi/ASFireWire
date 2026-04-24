# Nikon 4000ED Diagnostic Slice

This slice captures the next connected-device evidence for the Nikon LS-4000 ED
without requiring manual note taking during bring-up.

## Why This Exists

The latest `../moderncoolscan` records show the 4000ED is now discovered as a
full SBP-2 unit:

- GUID `0x0090b54001ffffff`
- Config ROM length `136` bytes
- `UNIT_SPEC_ID=0x00609e`
- `UNIT_SW_VERSION=0x010483`
- `Management_Agent_Offset=0x00c000`
- Apple-equivalent management agent CSR address `0xf0030000`

The remaining blocker is not ordinary discovery. It is whether ASFWDriver can
complete the SBP-2 management/login path: block transactions, management agent
access, target reads from our login ORB, and target writes to login response or
status FIFO.

## One Command

From the ASFireWire repository:

```bash
tools/diagnostics/nikon4000ed_slice.sh
```

The script defaults to `../moderncoolscan` and writes to:

```text
build/diagnostics/nikon4000ed-YYYYMMDD-HHMMSS/
```

Useful variants:

```bash
tools/diagnostics/nikon4000ed_slice.sh --node 0
tools/diagnostics/nikon4000ed_slice.sh --skip-login
tools/diagnostics/nikon4000ed_slice.sh --mcs-root /Users/gly/workspace/github/moderncoolscan
```

It is safe to run without the scanner. The script records failed commands and
keeps whatever context is available.

## Files To Compare First

- `04-info-verbose.txt`: confirms full ROM and `ManagementAgent csr_offset=0x00c000`.
- `05-raw-rom-trigger.txt`: captures the raw 136-byte Config ROM dump.
- `09-read-csr-config-rom-8.txt`: checks whether block read still fails where quadlet read works.
- `11-read-mgmt-agent-block-shifted.txt`: checks `0xf0030000`.
- `13-sbp2-probe.txt`: compares management agent, CSR, and command-set parsing.
- `asfw-log-last-30m.txt`: contains ASFWDriver-side transaction evidence.

## What Changed In ASFW Observability

SBP-2 address-space ranges now have debug labels. During login, logs should show
whether the target actually reaches the initiator-owned buffers:

```text
remote read-block label=sbp2-login-orb
remote write label=sbp2-login-response
remote write label=sbp2-status-fifo
```

Interpretation:

- If `sbp2-login-orb` is absent, the target did not fetch the login ORB after
  the management agent write.
- If `sbp2-login-orb` appears but `sbp2-login-response` and `sbp2-status-fifo`
  are absent, the target fetched the ORB but did not complete login.
- If `sbp2-status-fifo` appears, the next blocker is inside SBP-2 status/login
  response parsing rather than bus-level reachability.

The login timeout log now prints the exact ORB and status FIFO addresses it was
waiting on, which makes the absence of those labels actionable instead of just
another timeout.

## Current Expected Baseline

Based on the existing `../moderncoolscan` records, the next connected run should
start by checking whether these still hold:

- `mcs list` shows `guid=0x0090b54001ffffff`.
- `mcs info --node 0 --verbose` shows `ConfigROM bytes=136`.
- `tx read --addr 0xf0000400 --len 4` succeeds.
- `tx read --addr 0xf0000400 --len 8` fails.
- `tx read --addr 0xf0030000 --len 8` fails until the management path is fixed.

Any deviation from this baseline is useful evidence and should be kept with the
script output directory.
