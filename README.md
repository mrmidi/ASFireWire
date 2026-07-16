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
- Personally tested audio hardware now includes the Apogee Duet FireWire path, Focusrite Saffire Pro 24 DSP, PreSonus StudioLive 16.0.2 (full duplex 16-in/16-out streaming), and the Midas Venice F32 (full duplex 32-in/32-out streaming).
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
- PreSonus StudioLive 16.0.2 (personally tested on one unit; broader validation welcome)
- Midas Venice F32 (personally tested; broader validation welcome)

StudioLive 16.4.2 / 24.4.2 / 32.4.2 owners can help too: the driver recognizes these mixers but does not enable audio yet because their stream layout has not been captured from hardware. If you own one, open an issue — a short register capture using the ASFW app is all that is needed to add support.

If you try ASFireWire on one of them, please open a GitHub issue or reach out with:

- exact device model
- Mac model and macOS version
- Thunderbolt/adapter chain or PCIe FireWire hardware used
- whether the device enumerates, publishes an audio device, and starts playback/capture
- logs from the ASFW app, Console, or any crash report

Even a failed test report is valuable. "It does not enumerate at all" is still useful data.

## Collecting logs

If you are reporting a bug, driver logs from the moment the device appears, publishes an audio device, or fails to start are extremely helpful.

Important detail: DriverKit does not expose `os_log_create()` to this project, so ASFW driver logs do not show up as nice unified-log categories. Instead, they are easiest to find by process/bundle name and by message prefixes such as `[DICE]`, `[Audio]`, `[Async]`, `[AVC]`, `[Discovery]`, and `[Isoch]`.

### From Terminal

To watch logs live while reproducing the problem:

```sh
log stream --style compact \
  --predicate 'processImagePath CONTAINS[c] "ASFWDriver" OR eventMessage CONTAINS[c] "[DICE]" OR eventMessage CONTAINS[c] "[Audio]" OR eventMessage CONTAINS[c] "[Async]" OR eventMessage CONTAINS[c] "[AVC]" OR eventMessage CONTAINS[c] "[Discovery]" OR eventMessage CONTAINS[c] "[Isoch]"'
```

To save the last 10 minutes of likely ASFW driver logs to a file:

```sh
log show --last 10m --style compact \
  --predicate 'processImagePath CONTAINS[c] "ASFWDriver" OR eventMessage CONTAINS[c] "[DICE]" OR eventMessage CONTAINS[c] "[Audio]" OR eventMessage CONTAINS[c] "[Async]" OR eventMessage CONTAINS[c] "[AVC]" OR eventMessage CONTAINS[c] "[Discovery]" OR eventMessage CONTAINS[c] "[Isoch]"' \
  > ~/Desktop/asfw-driver.log
```

If you want a broader capture for a difficult issue, collect a full log archive right after reproducing it:

```sh
sudo log collect --last 10m --output ~/Desktop/asfw-driver.logarchive
```

### From Console.app

1. Open `Console.app`.
2. Select your Mac in the sidebar.
3. In the search field, try one of these filters:
   - `ASFWDriver`
   - `net.mrmidi.ASFW.ASFWDriver`
   - `[DICE]`
   - `[Audio]`
4. Reproduce the issue.
5. Save or export the matching lines, or copy the relevant window around the failure.

### What to include in a bug report

- the exact time the issue happened
- whether this was during enumeration, playback start, capture start, or after some minutes of streaming
- the shell log snippet or `.logarchive`
- whether the failure is repeatable

If the logs are too sparse, mention that too. The driver has runtime verbosity knobs in `ASFWDriver/Info.plist`, and those can be turned up for a follow-up repro.

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

Personally tested with working audio:

- Apogee Duet FireWire
- Focusrite Saffire Pro 24 DSP
- PreSonus StudioLive 16.0.2
- Midas Venice F32 (32×32 full duplex, 44.1 kHz and 48 kHz, live host-driven rate switching)

Recognized but not enabled yet:

- Focusrite Saffire Pro 26
- Focusrite Saffire Pro 40
- Focusrite Saffire Pro 40 TCD3070
- Focusrite Liquid Saffire 56
- PreSonus StudioLive 16.4.2 / 24.4.2 / 32.4.2 (stream layout not yet captured from hardware)

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

Build scripts or CMakeLists are for quick testing and creating compile_commands.json for static analysis tools. The proper way to build and sign the driver is via Xcode.

### Xcode project is generated (XcodeGen)

`ASFW.xcodeproj` is generated from the root [`project.yml`](project.yml) with
[XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).
The generated project is committed, so plain checkouts (and CI) build without
XcodeGen installed — but **never edit the pbxproj or project settings in the
Xcode UI**; change `project.yml` instead.

After **adding, removing, or renaming source files**, regenerate the project
and commit it together with your change:

```bash
xcodegen generate     # ./build.sh does this automatically when xcodegen is installed
```

Output is deterministic — regenerating with no changes produces an identical
pbxproj.

NOTE: You need an Apple Developer account (paid) and appropriate entitlements — or a free account plus SIP disabled — to build/load the driver on your machine. See Apple's documentation for details: https://developer.apple.com/documentation/driverkit/debugging-and-testing-system-extensions

Enabling `systemextensionsctl developer on` is recommended — it allows installing system extensions from the build

### SCSI HBA (SBP-2 scanners/disks) — opt-in

The SCSI HBA (`ASFWSCSIControllerService`, for SBP-2 devices such as FireWire film
scanners and disks) is **excluded from the default build**, because shipping it by
default can **panic the machine into a boot loop** (~60 s registry busy-timeout,
`IOService.cpp:5986`, no device attached) through two independent paths:

- Its Info.plist personality instantiates a kernel-side
  `IOUserSCSIParallelInterfaceController` as soon as the FireWire card matches at
  boot. The HBA currently reports SCSI target 0 as present unconditionally, and the
  probe INQUIRY is then held with no deadline waiting for an SBP-2 login that never
  arrives when no SBP-2 device is on the bus (audio interfaces are not SBP-2). The
  stalled target registration keeps the PCI nub busy past watchdogd's 60 s boot
  quiesce. This happens **regardless of SIP/AMFI state**.
- It requires the restricted
  `com.apple.developer.driverkit.family.scsicontroller` entitlement. On a machine
  where AMFI enforces entitlements, the ad-hoc-signed dext carrying it is killed at
  launch (taking the audio driver down with it, since everything runs in one
  process), and the orphaned kernel stub strands the same busy chain.

The default build carries neither the personality nor the entitlement and cannot
trigger either path.

To include the HBA, opt in explicitly:

```bash
./build.sh --scsi          # or: xcodebuild … ASFW_ENABLE_SCSI=YES
./sign.sh                  # picks the +SCSI entitlements automatically
```

> **Warning:** `--scsi` builds are currently **not cold-boot-safe**: cold-booting
> with the FireWire controller attached and no powered-on SBP-2 device on the bus
> can hit the 60 s boot panic even with SIP fully disabled. Power the SBP-2 device
> on before booting. A stalled probe on a running system does not panic
> immediately, but can panic later when the adapter is unplugged or the extension
> is torn down. The HBA-side fix (create the target at SBP-2 login instead of
> boot) will follow in a separate PR.

If a machine ever ends up in a panic loop: boot into Recovery, `csrutil disable`,
boot normally, uninstall the extension
(`systemextensionsctl uninstall - net.mrmidi.ASFW.ASFWDriver`), then re-enable SIP.

## Installing a prebuilt build (testers)

If you want to test ASFireWire without building it yourself, tagged releases attach a
prebuilt `ASFW.app`. This build is **ad-hoc signed and NOT notarized**, so it can only
be loaded on a machine with System Integrity Protection (SIP) disabled. It is intended
for experimental testing only — not general use.

> **Warning:** These steps disable SIP, which lowers your Mac's security system-wide.
> Only do this on a machine you are comfortable using for testing, and re-enable SIP
> (`csrutil enable`) when you are done. The build is unsigned/un-notarized and provided
> as-is; run it only if you understand and accept that.
>
> **Uninstall the extension _before_ re-enabling SIP.** With SIP back on, AMFI refuses
> to launch the ad-hoc-signed dext; an installed build that includes the SCSI HBA then
> leaves an orphaned kernel-side SCSI stub behind at every boot, which can panic the
> machine into a boot loop (recovery: Recovery → `csrutil disable` → boot → uninstall
> → `csrutil enable`).

**Requirements:** an Apple Silicon Mac running macOS 26 (Tahoe), and FireWire hardware
(a PCIe FireWire/OHCI card, or an Apple Thunderbolt-to-FireWire adapter).

1. **Download** the `ASFW-<version>-adhoc.zip` from the [Releases](../../releases) page and unzip it.
2. **Remove the quarantine flag** (Gatekeeper quarantines downloaded apps):
   ```bash
   xattr -dr com.apple.quarantine ASFW.app
   ```
3. **Disable SIP** (see Apple's guide on
   [disabling and enabling System Integrity Protection](https://developer.apple.com/documentation/security/disabling-and-enabling-system-integrity-protection)).
   Shut down, then boot into
   [Recovery](https://support.apple.com/en-us/102518) (hold the power button until
   "Loading startup options" appears → **Options** → open **Terminal**), and run:
   ```bash
   csrutil disable
   ```
   Reboot back into macOS. Confirm with `csrutil status` (should report disabled).
4. **Enable system-extension developer mode** (runtime toggle, no reboot; requires SIP
   already off):
   ```bash
   systemextensionsctl developer on
   ```
5. **Install the driver.** Move `ASFW.app` to `/Applications`, open it, and use its
   **Install** button. Approve the extension in **System Settings → General → Login Items
   & Extensions** if prompted (the prompt may not appear in developer mode).
6. **Verify** the driver is active:
   ```bash
   systemextensionsctl list
   ```
   You should see `net.mrmidi.ASFW.ASFWDriver` marked `[activated enabled]`.

**Uninstall / revert:** remove the extension via the app, or manually (ad-hoc builds
have no team ID, hence the `-`):
```bash
systemextensionsctl uninstall - net.mrmidi.ASFW.ASFWDriver
```
Then re-enable SIP from Recovery with `csrutil enable`.

**Troubleshooting:** if installation fails with `Missing entitlement
com.apple.developer.system-extension.install`, the build was not signed with its
entitlements embedded — that is a packaging bug in the release, not something to fix on
your machine; please open an issue rather than changing boot-args.

## Contributing

Nice place to start with — [DeepWiki page for ASFW](https://deepwiki.com/mrmidi/ASFireWire).

Contributions are VERY welcome! If you want to contribute to the project, please follow these steps:

1. Fork the repository on GitHub
2. Create a new branch for your feature or bugfix
3. Make your changes and commit them with clear messages
4. Push your changes to your forked repository
5. Open a pull request on the original repository, describing your changes and why they should be merged

> **Note:** `ASFW.xcodeproj` is generated from `project.yml` (see
> [Building](#building)). If your change adds, removes, or renames source
> files, run `xcodegen generate` and include the regenerated project in the
> same commit. Don't hand-edit the pbxproj or change build settings through
> the Xcode UI — those changes will be overwritten; edit `project.yml` instead.

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
