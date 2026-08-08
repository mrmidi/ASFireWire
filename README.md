# ASFireWire

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=mrmidi_ASFW&metric=alert_status&token=3ca1b3d10414117bb3e75b1779090b4ea47f1585)](https://sonarcloud.io/summary/new_code?id=mrmidi_ASFW) [![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/mrmidi/ASFireWire)

## Table of Contents

- [Preamble](#preamble)
- [Overview](#overview)
- [Current status](#current-status)
- [Call for testing](#call-for-testing)
- [Collecting logs](#collecting-logs)
- [Hardware compatibility](#hardware-compatibility)
- [FireWire protocol brief overview](#firewire-protocol-brief-overview)
- [What is OHCI?](#what-is-ohci)
- [Old Apple FireWire driver overview](#old-apple-firewire-driver-overview)
- [What currently works](#what-currently-works)
- [Driver initialization (high level)](#driver-initialization-high-level)
- [What is planned](#what-is-planned)
- [Developer MCP control plane](#developer-mcp-control-plane)
- [Codex MCP skill](#codex-mcp-skill)
- [Code guidelines](#code-guidelines)
- [General pitfalls and gotchas](#general-pitfalls-and-gotchas)
- [Project structure](#project-structure)
- [Building](#building)
- [Installing a prebuilt build (testers)](#installing-a-prebuilt-build-testers)
- [Contributing](#contributing)
- [Contacts](#contacts)
- [References](#references)

## Preamble

TL;DR: Apple removed the built-in FireWire stack in macOS Tahoe (26). ASFireWire is an attempt to rebuild enough of it in DriverKit to keep legacy FireWire hardware usable on modern macOS again. The project is public for historical, educational, and practical reasons, and to help people keep older audio interfaces alive. [YouTube demo video](https://youtu.be/Q1TbehOGnW0)

> WARNING: This project is still experimental. The driver can enumerate hardware, move async traffic, and bring up selected audio paths, but it is not production-ready. Expect instability, missing controls, and regressions during longer playback/capture runs.

## Overview

ASFireWire is a macOS driver extension project that restores FireWire (IEEE 1394) functionality on modern macOS versions where native support has been removed. It uses DriverKit and PCIDriverKit to implement the stack in user space instead of relying on the old kernel-extension model.

The codebase currently covers OHCI controller bring-up, topology and Config ROM handling, async transactions, AV/C plumbing, and an in-progress audio stack for both AV/C and DICE-based devices.

## Current status

What is real today:

- OHCI controller bring-up, bus resets, Self-ID decoding, and topology tracking are implemented.
- Async FireWire transactions are in place and used by discovery and protocol code.
- AV/C FCP and CMP plumbing exists and is working on the main test rig.
- Audio publication and experimental streaming paths exist in-tree.
- Audio hardware tested by the maintainer: the Apogee Duet FireWire path, Terratec PHASE 88 Rack, and Focusrite Saffire Pro 24 DSP. Contributors have additionally verified the PreSonus StudioLive 16.0.2 (full duplex 16-in/16-out streaming) and the Midas Venice F32 (full duplex 32-in/32-out streaming).
- Experimental DICE support is now enabled in-tree for Focusrite Saffire Pro 14, Saffire Pro 24, Saffire Pro 24 DSP, PreSonus StudioLive 16.0.2, and the Midas Venice F32.
- **Multi-stream DICE now works.** The Midas Venice F32 runs two isochronous streams per direction (2×16 channels = 32×32 total duplex).
- **Host-controlled sample-rate switching is implemented**, including 44.1 kHz alongside 48 kHz. The driver decodes the device's advertised clock capabilities and drives DICE `CLOCK_SELECT`, so a rate change in the host (e.g. Logic) reprograms the device live without a reconnect. Switching rates on a CoreAudio aggregate device whose clock master is the FireWire interface is supported.
- **Per-channel names** (device nickname plus per-channel TX/RX labels) are read from DICE devices and surfaced to CoreAudio.
- Focusrite Saffire Pro 26, Saffire Pro 40, Saffire Pro 40 TCD3070, and Liquid Saffire 56 are recognized but intentionally not enabled yet — their stream layouts still need to be captured from real hardware.
- PreSonus StudioLive 16.4.2, 24.4.2, and 32.4.2 are recognized by name but not audio-enabled yet: their FireWire channel counts differ from the 16.0.2 and must be captured from real hardware first (a wrong channel count means the device never locks to the stream). If you own one, see the call for testing below.
- The project is still not stable enough to recommend as a drop-in replacement for Apple's old FireWire stack.

## Call for testing

If you own a supported Focusrite Saffire card, testing would help a lot right now. Saffire Pro 24 DSP is already personally tested here, but broader validation is still welcome.

Please test these currently enabled DICE devices:

- Focusrite Saffire Pro 14
- Focusrite Saffire Pro 24
- Focusrite Saffire Pro 24 DSP
- PreSonus StudioLive 16.0.2 (contributor-verified on one unit; broader validation welcome)
- Midas Venice F32 (contributor-verified; broader validation welcome)

StudioLive 16.4.2 / 24.4.2 / 32.4.2 owners can help too: the driver recognizes these mixers but does not enable audio yet because their stream layout has not been captured from hardware. If you own one, open an issue — a short register capture using the ASFW app is all that is needed to add support.

If you try ASFireWire on one of them, please open a GitHub issue or reach out with:

- exact device model
- Mac model and macOS version
- Thunderbolt/adapter chain or PCIe FireWire hardware used
- whether the device enumerates, publishes an audio device, and starts playback/capture
- logs from the ASFW app, Console, or any crash report

Even a failed test report is valuable. "It does not enumerate at all" is still useful data.

## Collecting logs

If you are reporting a bug, please share the logs from the ASFW app:

1. Open **System Logs** in the sidebar.
2. Reproduce the problem.
3. Copy the log entries around the failure and attach them to the report.

Please include the ASFW version, device, and approximate time of the reproduction. Do
not worry about interpreting the messages or removing repeated lines — the complete
log is more useful.

If the app cannot show logs, use Console.app to reproduce the problem and export the
entries mentioning ASFW, then attach that file instead. The [Logging wiki page](https://github.com/mrmidi/ASFireWire/wiki/Logging)
has optional fallback instructions if a maintainer asks for a broader capture.

## Hardware compatibility

Current development and packet-analyzer hardware:

- Apple MacBook Air 2020 (M1, 13-inch)
- Thunderbolt 3 to Thunderbolt 2 adapter
- Thunderbolt 2 to FireWire 800 adapter
- Apogee Duet 2 FireWire audio interface
- Focusrite Saffire Pro 24 DSP audio interface
- PowerMac G3 (Blue and White) with built-in FireWire 400 ports used as a packet analyzer

Audio-device support in tree today:

- Apogee Duet FireWire
- Focusrite Saffire Pro 14
- Focusrite Saffire Pro 24
- Focusrite Saffire Pro 24 DSP
- PreSonus StudioLive 16.0.2
- Midas Venice F32 (multi-stream DICE, 32-in/32-out)
- Terratec PHASE 88 Rack
- Weiss INT202 and INT203 (DICE 2-channel layout; wired up but **never run against real hardware**)

Personally tested with working audio (hardware owned by the maintainer):

- Apogee Duet FireWire
- Terratec PHASE 88 Rack
- Focusrite Saffire Pro 24 DSP

Verified working by contributors on their own hardware:

- PreSonus StudioLive 16.0.2 (full duplex 16-in/16-out) — [@klochowicz](https://github.com/klochowicz)
- Midas Venice F32 (32×32 full duplex, 44.1 kHz and 48 kHz, live host-driven rate switching) — [@alicankaralar](https://github.com/alicankaralar)
- Nikon Coolscan 9000 and Coolscan 4000 — SBP-2/SCSI film scanners, plug and play — [@mhellevang](https://github.com/mhellevang)
- Panasonic MiniDV camcorder — DV capture and tape transport — [@hoffmabc](https://github.com/hoffmabc)

A fuller breakdown, including recognized-but-not-enabled devices and how to report your
own results, lives on the
[Device Compatibility wiki page](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility).

Recognized but not enabled yet:

- Focusrite Saffire Pro 26
- Focusrite Saffire Pro 40
- Focusrite Saffire Pro 40 TCD3070
- Focusrite Liquid Saffire 56
- PreSonus StudioLive 16.4.2 / 24.4.2 / 32.4.2 (stream layout not yet captured from hardware)
- Weiss ADC2, Vesta, DAC2/Minerva, AFI1, DAC202, Maya, MAN301 (identified by name; audio enablement needs a verified stream/clock trace per model)

In theory the driver can be extended to other OHCI controllers and many more FireWire devices, but hardware access is still the limiting factor. Host-controller matching and audio-device enablement are intentionally conservative until more real machines are tested.

## FireWire protocol brief overview

FireWire, also known as IEEE 1394, is a high-speed serial bus interface standard for connecting peripheral devices to a computer. It was developed in the late 1980s and early 1990s by Apple, Sony, and others. FireWire was widely used in the early 2000s for connecting digital video cameras, external hard drives, and audio interfaces due to its high data transfer rates and low latency.

From the driver perspective, the key point is that the FireWire protocol stack contains several layers: isochronous and asynchronous data transfer. Isochronous transfers are used for time-sensitive data, such as audio and video streams, where data must be delivered at regular intervals; they do not guarantee delivery — if a packet is missed, it is lost. Asynchronous transfers are used for general-purpose data transfer and are more reliable: they include acknowledgments and retransmissions.

Each stack layer contains several contexts:

- Asynchronous:
  1. Asynchronous Transmit Request
  2. Asynchronous Transmit Response
  3. Asynchronous Receive Request
  4. Asynchronous Receive Response
- Isochronous:
  1. Isochronous Transmit
  2. Isochronous Receive

The devil is in the details.

## What is OHCI?

OHCI (Open Host Controller Interface) is a standard for FireWire host controllers that defines a hardware interface and programming model for FireWire devices. It was developed by Apple, Microsoft, IBM, and others in the late 1990s to promote interoperability among FireWire devices. Some historical material is available from IBM: https://public.dhe.ibm.com/rs6000/chrptech/1394ohci/

OHCI handles low-level details such as bus arbitration, data transfer protocols, and error handling, allowing device and software developers to focus on higher-level functionality.

The latest publicly available OHCI specification is version 1.1 (2000), though later drafts (e.g., 1.2) exist and many modern silicon implementations reflect those changes. Do not rely solely on version 1.1; always cross-verify with Linux drivers or the original Apple driver behavior.

## Old Apple FireWire driver overview

Apple's original series of kernel extensions (kexts) for FireWire was developed in the early 2000s. The IOFireWireFamily source first appears around OS X 10.1 (Puma) in 2001. Much of the design likely persisted for many years; some code may even predate OS X.

The FireWire stack is large and complex. Modernizing it with DriverKit and moving away from kernel extensions is a significant effort. Apple chose to remove FireWire support rather than maintain it.

Key components historically included:

- AppleFWOHCI.kext — part of IOFireWireFamily, provides the lowest-level API for OHCI-compliant controllers (hardware init, DMA management, interrupts, low-level transfers). Not open-sourced.
- IOFireWireFamily.kext — main FireWire framework; higher-level abstractions for enumeration and communication. Open-sourced.
- IOFireWireAVC.kext — Audio/Video Control protocol support. Open-sourced.
- IOFireWireSBP2.kext — SBP-2 (storage) support. Open-sourced.
- AppleFWAudio.kext — FireWire audio support (not open-sourced).
- IOFireWireSerialBusProtocolTransport.kext — SBP-2 transport layer. Open-sourced.

For isochronous transfers, Apple used a "language" called DCL (later NuDCL) — a wrapper for isochronous DMA programs. Documentation is sparse; historic Mac OS 9 developer docs and some header comments in IOFireWireLib were helpful when researching this.

## What currently works

This project is in active development. The following features are implemented:

- OHCI controller initialization and configuration
- PCIe device probing and matching
- Config ROM staging, scanning, and device discovery
- DMA buffer allocation and management
- Interrupt handling
- Bus reset and Self-ID processing
- Asynchronous data transfer
- Isochronous transmit DMA (OUTPUT_MORE-Immediate + OUTPUT_LAST) with interrupt-driven ring refill
- AV/C FCP request/response and CMP plug connection
- IRM (Isochronous Resource Manager)
- AudioDriverKit publication for supported devices
- Experimental DICE audio bring-up and runtime capability discovery for selected Focusrite Saffire, PreSonus StudioLive, and Midas Venice models
- Multi-stream DICE duplex (two isochronous streams per direction) for higher-channel devices such as the Midas Venice F32 (32×32)
- Host-controlled sample-rate switching (44.1 kHz / 48 kHz) driven from the device's advertised clock capabilities, including live rate changes on CoreAudio aggregate devices clocked by the FireWire interface
- DICE per-channel naming (device nickname plus TX/RX channel labels) surfaced to CoreAudio
- DV (IEC 61883-2) capture from MiniDV camcorders to raw `.dv` files, with AV/C tape transport control (play/stop/rewind) from the app. DV capture and audio receive are mutually exclusive — both use IR context 0

## Driver initialization (high level)

A concise, high-level breakdown of initialization steps (not exhaustive):

- Device probe: The driver probes the PCI device and detects vendor/device IDs.
- Controller/service construction: Core controller objects and the user-facing service/client interface are created and registered.
- Async subsystem setup: Initialize the asynchronous subsystem, allocate coherent DMA memory regions, create buffer rings and context managers, and map memory for device access.
- Config ROM staging: Generate/stage a Config ROM image into DMA-backed memory, prepare shadow registers, and make the ROM ready so it can be activated on the next bus reset.
- OHCI core bring-up: Perform a software reset, program PHY/link settings, write GUID and BusOptions, and configure link/transfer parameters required by the OHCI hardware.
- Self-ID & interrupt arming: Arm a Self-ID buffer before the first bus reset and enable interrupts. The controller typically sets linkEnable, which triggers an automatic bus reset so topology can be discovered.
- Bus-reset handling: On bus-reset events the driver quiesces transmit contexts, decodes Self-ID packets to build the topology, restores or activates the staged Config ROM, and re-arms asynchronous transmit/receive contexts for the new generation.
- Service readiness: After initialization and the first bus reset, the driver reports readiness, registers the user client, and continues to update topology and bus-state information as events arrive.

See runtime logs for example traces (DMA allocation, Config ROM staging, Self-ID decode, topology snapshots) and consult ASFWDriver sources for exact ordering and implementation details.

## What is planned

Current priorities are less about "first light" and more about hardening, timing, and hardware coverage.

Planned work:

1. Stabilize the audio path for longer playback/capture runs, especially timing and timestamp monotonicity.
2. Finish the remaining isochronous receive and bus-reset recovery work.
3. Broaden DICE support beyond the currently enabled set (Focusrite Saffire, PreSonus StudioLive, Midas Venice), including the recognized-but-not-yet-captured Saffire Pro 26/40 and larger StudioLive mixers.
4. Improve hardware coverage with more community-tested hosts, adapters, and interfaces.
5. Continue filling out device-specific controls where generic FireWire or generic DICE handling is not enough.

## Developer MCP control plane

The ASFW control app hosts an experimental Model Context Protocol (MCP) control plane (`ASFW/MCP/`) so AI agents and tooling can inspect driver state and run guarded low-level FireWire operations without parsing log dumps. It is a **development-only diagnostics interface**, not an audio-control or production feature, and it is **not enabled by default**.

Key points:

- It lives inside the existing Xcode project (no separate Swift package or CLI). The MCP layer talks to the driver through a narrow `ASFWDriverControlling` boundary over `ASFWDriverConnector`, so handlers can be unit-tested with mocks and later backed by the live connector.
- A local HTTP/SSE endpoint exposes the tool surface, gated behind an explicit runtime mode plus a write-policy engine — read/inspection tools first, with raw register and CAS writes refused unless the policy and test gates allow them.
- Tool surfaces include register access, AV/C (including one explicitly acknowledged, guarded Apogee Duet format transition) and raw FCP, CMP, IRM/CAS, SBP-2, and DICE/TCAT inspection. Telemetry and transaction schemas are published as MCP resources.
- `asfw://control-plane/health` is the versioned first query for agents. It reports `ready`, `degraded`, or `unavailable`, explicit reasons, the expected generation, and whether deeper read-only diagnostics are trustworthy.
- The default endpoint is loopback-only: `http://127.0.0.1:8766/mcp`. Enable the control plane in the ASFW app before connecting an agent; do not expose it on the network.
- The app's **Run read-only smoke** action is the preferred hardware check. It captures generation and node inventory, reads Config ROM data, identifies adapter gaps, and never embeds a device-specific write recipe.
- Design and usage are documented under `documentation/MCP_*.md` (control-plane architecture, tool taxonomy, write policy, mock/smoke harness, telemetry resources, tool-use examples, and agent workflows).

Because this can issue real bus transactions, keep it disabled unless you are actively developing against it.

### Advertised tools that are not implemented (TODO)

The catalog advertises a few read-only tools whose dispatch arms are still stubs. They
return `capabilityUnavailable` with an explicit reason rather than fabricating state.
An agent must report the gap, not work around it or substitute a mutating call.

| Tool | Why it is not implemented | What it needs |
|------|---------------------------|---------------|
| `asfw_sbp2_list_units` | No app-side plumbing. The dext has `UserClient/Handlers/SBP2Handler.cpp`, but there is no `DriverConnector+SBP2.swift` to reach it. | A connector extension exposing SBP-2 unit inventory, then a `ASFWDriverControlling` method. Related: FW-54/FW-56 session port. |
| `asfw_sbp2_inspect_unit` | Same as above. Unit-directory decode could come from the cached Config ROM, but session/ORB state cannot. | Same. |
| `asfw_sbp2_get_session_status` | Same as above, and login/fetch-agent state lives only in the driver. | Same. |
| `asfw_avc_get_subunit_descriptor` | Not a routing gap. AV/C descriptor access is a wire-observable `OPEN`/`READ`/`CLOSE DESCRIPTOR` sequence, so it must be written against the AV/C descriptor mechanism and cross-checked with `references/IOFireWireAVC` before issuing FCP to a real device. | Reference-validated descriptor implementation plus hardware verification. |
| `asfw_dice_decode_status` | Decoding is a pure function over quadlets, but every offset and mask lives in `ASFWDriver/Audio/Protocols/DICE/Core/DICETypes.hpp`, a C++ header that cannot be bridged to Swift. Mirroring it by hand would create a second source of truth. | Either generate the Swift constants from the header or add a parity test, then decode. Only testable with a DICE/TCAT device attached. |

Two shipped tools are also narrower than their names suggest, by design:

- `asfw_read_ohci_register` / `asfw_snapshot_ohci_registers` read the **diagnostics
  snapshot** (`ASFWDiagOHCI`), which covers a fixed set of named registers. They are not
  arbitrary MMIO offset reads; an uncovered offset is refused rather than guessed.
- `asfw_irm_list_allocations` reports **bus-wide occupancy** from the IRM resource CSRs.
  Those registers cannot say which node holds a channel, so the result carries
  `ownershipAttributed: false`.

## Codex MCP skill

The repository carries a compact Codex client skill at [`skills/asfw-mcp-control-plane/SKILL.md`](skills/asfw-mcp-control-plane/SKILL.md). It keeps the MCP session and compact response handling out of normal agent prompts, so agents can inspect real driver state without rediscovering the HTTP/SSE protocol or pasting the full tool catalog.

Install or refresh it for Codex with:

```bash
mkdir -p ~/.codex/skills
cp -R skills/asfw-mcp-control-plane ~/.codex/skills/
```

Then invoke `$asfw-mcp-control-plane`, or run its client directly:

```bash
python3 ~/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py health
python3 ~/.codex/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py summary
```

`health` is read-only and determines whether deeper queries are safe; preserve its expected generation for any follow-up bus request. `summary` adds driver state, compact node inventory, protocol availability, and write gate. Use `tools`, `resources`, and `read` for more detail. The helper refuses non-allowlisted tool calls unless `--allow-mutation` is supplied; that acknowledgement never bypasses the MCP server's developer and mutation gates. Use it only after explicit user authorization for the exact hardware action.

## Code guidelines

Current code is a work in progress. Target guidelines:

- Use C++23 features: std::expected for error handling, std::span for array views, smart pointers for memory management, and concepts for template constraints.
- Use CRTP for static polymorphism in hot paths (e.g., async/isochronous transactions and buffer management) where beneficial; otherwise prefer clarity.
- RAII for resource management (buffers, locks, etc.).
- High modularity: keep components isolated and single-responsibility. Avoid mega-classes where possible.
- Keep logic isolated from DriverKit where feasible so code remains testable without DriverKit dependencies.

## General pitfalls and gotchas

Recommendations based on experience:

1. Get a packet analyzer. OHCI error reporting is not very informative. You form packet headers in little-endian for OHCI with big-endian payloads; the controller converts to IEEE 1394-formatted packets on the wire. A packet analyzer helps detect malformed packets.
2. Always verify endianness.
3. Keep constants centralized. Do not create constants inside implementation files or class headers — use a single source of truth to avoid duplication and subtle bugs.
4. Use compile-time checks where possible: static_assert, concepts, etc. Tests are valuable — one missed bit shift can make the controller silent.

## Project structure

The repository is organized into these top-level components:

- **ASFW/** — Control app and installer (Swift/SwiftUI). The supported method to install DriverKit-based drivers on macOS. Also hosts the developer-only MCP control plane (`ASFW/MCP/`).
- **ASFWDriver/** — Main DriverKit-based FireWire driver (detailed below).
- **ASFWTests/** — DriverKit-independent unit and integration tests.
- **tests/** — Additional test fixtures and test infrastructure.
- **ADKVirtualAudioLab/** — AudioDriverKit virtual audio lab for testing audio paths without hardware (has its own `project.yml`).
- **documentation/** — Public project documentation and implementation guides.
- **diagrams/** — Architecture and design diagrams.
- **tools/** — Build and development utilities.
- **project.yml** — [XcodeGen](https://github.com/yonaskolb/XcodeGen) spec that `ASFW.xcodeproj` is generated from — the source of truth for targets, build settings, entitlements wiring, and schemes (see [Building](#building)).

### ASFWDriver structure

The driver is organized into functional subsystems:

**Core components:**

- **Hardware/** — OHCI register definitions, hardware interface abstraction, interrupt management.
- **Controller/** — Controller state machine, initialization, lifecycle, discovery integration.
- **Bus/** — Bus manager, Self-ID capture and decoding, topology management, bus reset coordination, gap count optimization, generation tracking, IRM (Isochronous Resource Manager), CSR space.
- **ConfigROM/** — Config ROM building, staging, parsing, scanning, local/remote ROM storage.
- **Discovery/** — Device enumeration, device manager, device registry, speed negotiation.
- **Phy/** — PHY packet encoding and decoding (type-safe, constexpr).

**Async subsystem:**

- **Async/** — Asynchronous packet transmission and reception.
  - **Commands/** — High-level async operations: Read, Write, Lock, PHY.
  - **Contexts/** — OHCI DMA context wrappers (AT/AR Request/Response).
  - **Core/** — Transaction management, DMA memory, payload handling.
  - **Engine/** — Context managers and DMA engine coordination.
  - **Rx/** — Packet parsing and routing.
  - **Tx/** — Descriptor building, packet submission.
  - **Track/** — Transaction tracking, label allocation, completion queues.
  - **Interfaces/** — Abstract interfaces for testability (IDMAMemory, IFireWireBus).

**Isochronous subsystem:**

- **Isoch/** — Isochronous packet transmission and reception.
  - **Transmit/** — Transmit context, DMA ring, descriptor slab.
  - **Receive/** — Receive context, DMA ring.
  - **Memory/** — Isochronous DMA memory management.
  - **Config/** — Isochronous configuration and timing.
  - **Core/** — Isochronous service orchestration.

**Audio:**

- **Audio/** — FireWire audio stack.
  - **Protocols/** — Device-specific audio protocols.
    - **DICE/** — DICE/TCAT protocol: core transaction layer, clock-capability decode and sample-rate selection (`CLOCK_SELECT`), per-device isoch profiles (Focusrite Saffire, PreSonus StudioLive, Midas Venice), generic TCAT backend.
    - **Oxford/** — Oxford/Apogee protocol (Duet FireWire).
    - **Backends/** — Audio backend implementations (AVC-driven and DICE-driven).
  - **Engine/Direct/** — Direct audio engine: clock publisher, output reader, input writer, DICE TX stream engine, RX packet processor.
  - **DriverKit/** — AudioDriverKit nub and driver (ASFWAudioDriver, ASFWAudioNub), controls, lifecycle, ZTS support.
  - **Core/** — Audio coordinator, nub publisher, runtime registry.
  - **Wire/** — Wire format layers: AM824, AMDTP, CIP, IEC 61883, Raw PCM 24-in-32.
  - **Config/** — Audio constants, RX/TX profiles, timing cursor policy.
  - **Ports/** — Audio port interfaces (TX slot provider, cycle timeline, diag sink).
  - **Model/** — Audio device model and property keys.
  - **Runtime/** — Host clock anchor, playback ring range.

**Protocols:**

- **Protocols/AVC/** — Audio/Video Control protocol: FCP transport, CMP plug connection, PCR space, AVC unit management, discovery, signal/stream format commands, descriptors.
- **Protocols/SBP2/** — SBP-2 (storage) protocol: command ORBs, management ORBs, address space management, page tables.
- **Protocols/Ports/** — Protocol port abstractions (FireWire bus port, RX port, register I/O).

**Supporting subsystems:**

- **DeviceProfiles/** — Device identity and audio profile registry. Vendor-specific profiles for Focusrite, Apogee, Alesis, PreSonus, and Midas.
- **Diagnostics/** — Runtime diagnostics, controller metrics, status publishing, signposts.
- **Logging/** — Structured logging system.
- **Debug/** — Packet capture and async trace tools.
- **Scheduling/** — Scheduler and watchdog coordinator.
- **Snapshot/** — System state snapshots for debugging.
- **Shared/** — Shared data models, completion helpers, memory abstractions, ring buffers.
- **Common/** — Utilities: wire format helpers, barrier utilities, timing, type definitions.
- **Testing/** — Test hooks and DriverKit stubs for offline testing.

## Building

The repository uses the root build scripts for reproducible project generation and
installable builds. The complete build-and-sign procedure is in the
[Building and Signing wiki](https://github.com/mrmidi/ASFireWire/wiki/Building-and-Signing).

### Xcode project is generated (XcodeGen)

`ASFW.xcodeproj` is generated from the root [`project.yml`](project.yml) with
[XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).
The generated project is **not committed** — it is gitignored, so XcodeGen is a
build prerequisite for checkouts and CI alike. Never edit the pbxproj or project
settings in the Xcode UI; change `project.yml` instead.

Generate it after cloning, and again after **adding, removing, or renaming
source files**:

```bash
xcodegen generate     # ./build.sh does this automatically when xcodegen is installed
```

Output is deterministic — regenerating with no changes produces an identical project.

### Build and sign locally

`build.sh` deliberately produces an unsigned app and dext. It also refreshes the
generated version metadata and, for an installable build, increments
`CFBundleVersion` unless `--no-bump` is supplied:

```bash
./build.sh --config Release
./sign.sh
```

The default output is
`build/DerivedData/Build/Products/Release/ASFW.app`. `sign.sh` ad-hoc signs the
embedded dext first and then the app with the checked-in per-target entitlement
files, and verifies that the required entitlements are present in both signatures.
Use the script rather than manually signing an app bundle.

This local path does not depend on a provisioning profile or an Apple Developer
account. It uses ad-hoc signing (`codesign --sign -`) with the checked-in entitlement
files. For this repository's ad-hoc test path, enable system-extension developer mode
and keep SIP disabled while testing:

```bash
systemextensionsctl developer on
```

See [Installing](https://github.com/mrmidi/ASFireWire/wiki/Installing) for the full
SIP and recovery procedure. `./build.sh --no-bump` is useful for builds that are not
being installed; when replacing an installed extension, build without that option or
turn off **Require a newer build** in **Overview → Driver Management**.

Tagged release ZIPs use the separate `install.sh` workflow described below: signing
happens locally on the tester's Mac with the same ad-hoc signing path.

### SCSI HBA (SBP-2 scanners/disks)

Every build includes a SCSI host adapter (`ASFWSCSIControllerService`) that exposes
SBP-2 FireWire devices — film scanners, disks — to macOS, so tools like VueScan see
them as regular SCSI devices. It needs no setup beyond the driver installation
itself.

If the machine ever ends up in a boot panic loop with the driver installed: boot
into Recovery, `csrutil disable`, boot normally, uninstall the extension
(`systemextensionsctl uninstall - net.mrmidi.ASFW.ASFWDriver`), then re-enable SIP.
As a last resort, `sudo nvram boot-args="io=0"` makes macOS log the failure instead
of panicking (clear with `sudo nvram -d boot-args`).

## Installing a prebuilt build (testers)

Tagged releases attach an unsigned `ASFW-<version>.zip` containing `ASFW.app`,
`install.sh`, and the entitlement resources needed for local signing. Run the
installer from the extracted release directory:

```bash
cd ASFW-<version>
./install.sh
```

The installer checks that SIP is disabled, enables system-extension developer mode,
removes the download quarantine flag, ad-hoc signs the dext first and the app second,
verifies the embedded entitlements, copies the result to `/Applications`, and opens
the installed app.

No Apple Developer account, signing identity, or provisioning profile is required for
this experimental ad-hoc install path. The script does not ask for or upload an Apple
ID password.

> **Note:** installing requires disabling SIP, which lowers your Mac's security
> system-wide. Re-enable SIP when you are finished testing. The installer refuses to
> install while SIP is enabled.

**→ Full instructions: [Installing (wiki)](https://github.com/mrmidi/ASFireWire/wiki/Installing)**

To build and sign it yourself instead, see
[Building and Signing (wiki)](https://github.com/mrmidi/ASFireWire/wiki/Building-and-Signing).
For bug reports, see [Reporting Issues (wiki)](https://github.com/mrmidi/ASFireWire/wiki/Reporting-Issues).

## Contributing

Nice place to start with — [DeepWiki page for ASFW](https://deepwiki.com/mrmidi/ASFireWire).

Contributions are VERY welcome! If you want to contribute to the project, please follow these steps:

1. Fork the repository on GitHub
2. Create a new branch for your feature or bugfix
3. Make your changes and commit them with clear messages
4. Push your changes to your forked repository
5. Open a pull request on the original repository, describing your changes and why they should be merged

> **Note:** `ASFW.xcodeproj` is generated from `project.yml` and is not tracked
> in git (see [Building](#building)). If your change adds, removes, or renames
> source files, edit `project.yml` and commit that. Don't hand-edit the pbxproj
> or change build settings through the Xcode UI — those changes are wiped on the
> next `xcodegen generate`.

Literally any help is appreciated, from fixing typos in documentation to implementing new features or fixing bugs. Writing tests, improving code quality, testing on hardware, and reporting regressions are all valuable. Hardware reports for supported Saffire devices are especially useful right now. If you have any experience with FireWire protocol, just opening an issue or emailing me is invaluable. If you have any experience with Swift, the ASFW app could use some love too.

## License

ASFireWire is licensed under the [Apache License, Version 2.0](LICENSE).

The in-tree reference stacks used for behavioral validation (Linux
`drivers/firewire`, Apple IOFireWireFamily, libffado, and others) are **not**
part of this repository and are never distributed with it; they are consulted
as read-only behavioral references and no code from them is copied. See
[NOTICE](NOTICE) for attribution details.

Contributions are accepted under the terms of the Apache License 2.0
(inbound = outbound, per section 5 of the license).

## Contacts

You can reach me via:

- Discord server: https://discord.gg/c82rmSEEPY
- Email: me [at] mrmidi.net
- LinkedIn: https://www.linkedin.com/in/mrmidi/

## References

- [Apple DriverKit Documentation](https://developer.apple.com/documentation/driverkit) - NB. Oficcial documentation on Apple Developer website is incomplete and sometimes outdated. Refer to header files in DriverKit SDK for more accurate information.
- [Apple PCIDriverKit Documentation](https://developer.apple.com/documentation/pcidriverkit) s- Same as above
- [System Extensions and DriverKit](https://developer.apple.com/videos/play/wwdc2019/702/) — WWDC 2019 session introducing DriverKit and system extensions.
- [Modernize PCI and SCSI drivers with DriverKit](https://developer.apple.com/videos/play/wwdc2020/10670/) — Small but informative WWDC 2020 session about modernizing PCI and SCSI drivers.
- [IEEE 1394-2008 Standard](https://standards.ieee.org/ieee/1394/4377/) — Latest edition. This is most complete reference about FireWire 
