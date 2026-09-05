# ASFW Duet latency investigation — 2026-09-05

Parent plan and next decisions: [Audio latency ledger and timing SSOT](../../AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md#current-decision--explain-the-delay-then-reduce-it).

Local code review and electrical loopback, Duet L/output 1 to input 1, 48 kHz. No driver rebuild, install, reset, or hardware-control write was performed by the agent. The user restarted the hardware between sessions. Tests used the RTL executable built from `dad154d5`, amplitude 0.1, zero-based channels 0 → 0: 20 trials in the original run and eight per follow-up run. The final requested device buffer is 64 frames.

## Observations

| Session / requested and observed callback size | Accepted | RTL raw | Raw minus measured scheduling |
|---|---:|---:|---:|
| Earlier session / 64 | 20/20 | 2598.95 frames / 54.145 ms | 2370.95 frames / 49.395 ms |
| After hardware restart / 64, initial | 5/8 | 582.98 frames / 12.145 ms | 354.98 frames / 7.395 ms |
| Same restarted session / 128 | 6/8 | 710.93 frames / 14.811 ms | 354.93 frames / 7.394 ms |
| Same restarted session / 256 | 5/8 | 966.94 frames / 20.145 ms | 354.94 frames / 7.395 ms |
| Same restarted session / 64, return | 5/8 | 582.95 frames / 12.145 ms | 354.95 frames / 7.395 ms |

Every fresh run had exactly the requested callback span, zero reported overloads, delivered-frame gaps, re-anchors, clock disagreements, or missing timestamps. Rejected trials were no-signal. Fresh runs had weak SNR (16–18 dB); these measurements are provisional, not a completed Phase 1 baseline. The reporting-only latency perturbation self-check has not been performed.

Measured scheduling equals 2 × callback frames + 100 safety frames throughout. Buffer changes produce the expected 2 × delta in raw RTL. This supports correct client buffer negotiation and a stable remaining delay within this session; it does not independently prove all timestamps or the detector are correct.

The remaining ~355 frames are not established hardware latency, despite that label in the tool output. Declared device + stream latency is 107 frames, leaving ~248 frames / 5.166 ms unexplained. The previous session was ~2016 frames / 42 ms slower at the same client size. A restart changed session state; the measurements do not locate that difference in the driver versus interface internals.

Between those sessions, both 128- and 64-frame tests returned all-zero input. PCM publication advanced while observed TX completion and scheduling cursors remained fixed. Those observations describe the silent session only. A later driver log cursor reset and new StartIO match the user's hardware restart; fresh completion counters advance.

## Code findings

- RTL buffer setting uses the global/main BufferFrameSize property, but discards setter status (`tools/rtl/rtl_loopback.c:991`). Getter errors also need explicit handling. Actual callback spans validate these particular runs, but future failures should be surfaced before measuring.
- Driver BeginRead and WriteEnd use the actual `ioBufferFrameSize`, with stream-ring capacity as the bound (`ASFWDriver/Audio/DriverKit/ASFWAudioDriverIO.cpp:263`). WriteEnd publishes that range and requests TX preparation; it does not wait for a fixed 512-frame batch. The 512-frame configuration is a sizing budget.
- `txTransportStatus` has initialization/reset writes but no runtime producer in current source (`ASFWDriver/Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp:524`, reset at 903). MCP maps zero to stopped (`ASFW/MCP/ASFWMCPAudioStreamTools.swift:261`). This status cannot establish a live transport stop. The advancing fresh completion counters directly contradict its stopped label.
- TX initial frame mapping uses the RX-derived observation and requested presentation bus time, then advances a persistent frame cursor (`ASFWDriver/Audio/Runtime/HardwareSampleTimeline.hpp:308`). Startup/recovery alignment is a concrete tracing target, not an established cause of the measured difference.

## Path that needs one correlated trace

Carry the same marker through one epoch, retaining absolute sample coordinates and monotonic host/bus correlations. Aggregate cursor snapshots cannot locate a specific impulse.

| Boundary | Evidence to correlate |
|---|---|
| Client output IOProc | Marker index, output sample/host timestamp, actual callback span |
| ADK WriteEnd | Published frame range and actual publication wall time |
| PCM cache → TX plan | Marker frame, selected packet, finalization time, epoch |
| TX transmission → requested presentation | Completion/transmit bus time and stamped SYT; keep observed transmission separate from requested presentation |
| RX packet → capture ring | Returned marker index, packet bus/host correlation, decoded absolute frame, publication wall time |
| ADK BeginRead → client input IOProc | Requested and available ranges, read wall time, input sample/host timestamp, detected marker |

This separates client scheduling, publication/finalization delay, transmission/presentation mapping, receive visibility, and the remaining device path. Do not tune declarations to absorb the discrepancy before this attribution and the reporting-only self-check.

## Apple USB source relevance

The supplied source separates safety offsets from reported latency and adjusts safety for which stream generates timestamps (`/Users/mrmidi/Downloads/OSXUSBAudioDriverSource 4/AppleUSBAudioStream.cpp:1488`). Its input conversion path explicitly gathers received data whose completion processing has not yet made it visible (`AppleUSBAudioEngine.cpp:506`). Those are useful behavioral questions for ASFW: where is time anchored, and when are bytes actually readable? USB-specific constants and the old IOKit lifecycle are not ADK authority. No reference code was copied.

## Evidence files

- [asfw-duet-rtl-64-2026-09-05.txt](evidence/asfw-duet-rtl-64-2026-09-05.txt)
- [asfw-duet-rtl-buffer-sweep-128-2026-09-05.txt](evidence/asfw-duet-rtl-buffer-sweep-128-2026-09-05.txt)
- [asfw-duet-rtl-64-followup-2026-09-05.txt](evidence/asfw-duet-rtl-64-followup-2026-09-05.txt)
- [asfw-duet-rtl-64-fresh-2026-09-05.txt](evidence/asfw-duet-rtl-64-fresh-2026-09-05.txt)
- [asfw-duet-rtl-fresh-sweep-128-2026-09-05.txt](evidence/asfw-duet-rtl-fresh-sweep-128-2026-09-05.txt)
- [asfw-duet-rtl-fresh-sweep-256-2026-09-05.txt](evidence/asfw-duet-rtl-fresh-sweep-256-2026-09-05.txt)
- [asfw-duet-rtl-fresh-sweep-64-2026-09-05.txt](evidence/asfw-duet-rtl-fresh-sweep-64-2026-09-05.txt)
- [asfw-duet-cursors-fresh-64-2026-09-05.json](evidence/asfw-duet-cursors-fresh-64-2026-09-05.json)
- [asfw-duet-directaudio-ring-final-2026-09-05.json](evidence/asfw-duet-directaudio-ring-final-2026-09-05.json) (new session; response has cursorReset=true)
