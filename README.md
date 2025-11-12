# ASFW project

## Table of Contents

- [Preamble](#preamble)
- [Overview](#overview)
- [Hardware compatibility](#hardware-compatibility)
- [FireWire protocol brief overview](#firewire-protocol-brief-overview)
- [What is OHCI?](#what-is-ohci)
- [Old Apple FireWire driver overview](#old-apple-firewire-driver-overview)
- [What currently works](#what-currently-works)
- [Driver initialization (high level)](#driver-initialization-high-level)
- [What is planned](#what-is-planned)
- [Code guidelines](#code-guidelines)
- [General pitfalls and gotchas](#general-pitfalls-and-gotchas)
- [Project structure](#project-structure)
- [Building](#building)
- [Contributing](#contributing)
- [Contacts](#contacts)
- [References](#references)

## Preamble

TL;DR — Since macOS Tahoe (26) Apple completely removed the FireWire stack from macOS. This driver aims to restore FireWire functionality on modern macOS versions. The goal is to make the project public for historical and educational purposes, and to help people with legacy FireWire devices. [Youtube demo video](hhttps://youtu.be/hg1p_yXbfnc)

> WARNING: This project is in early development. It is not a working driver yet. It is intended for developers and people interested in macOS driver development.

## Overview

ASFW is a macOS driver extension (dext) that restores FireWire (IEEE 1394) functionality on modern macOS versions where native support has been removed. It uses DriverKit and PCIDriverKit frameworks to implement the driver in user space — the modern approach to writing drivers on macOS instead of traditional kernel extensions.

## Hardware compatibility

Currently I am developing and testing the driver on the following hardware:

- Apple MacBook Air 2020 (M1, 13-inch)
- Thunderbolt 3 to Thunderbolt 2 adapter
- Thunderbolt 2 to FireWire 800 adapter
- Apogee Duet 2 FireWire audio interface
- PowerMac G3 (Blue and White) with built-in FireWire 400 ports used as a packet analyzer

In theory, the driver could be extended to support other FireWire OHCI controllers (for example, via a TB3-to-PCIe chassis with a PCIe FireWire card), but I cannot test that right now. Device matching is currently hardcoded to my vendor/device ID, but it can be extended. See [ASFWDriver/Info.plist](ASFWDriver/Info.plist) for details on device probing/matching.

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

This project is in early development. The following features are implemented:

- OHCI controller initialization and configuration
- PCIe device probing and matching
- DMA buffer allocation and management
- Interrupt handling
- Bus reset and Self-ID processing
- Basic asynchronous data transfer (reading quadlets from devices)

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

Next steps include reading Config ROMs from attached devices and parsing them. This is required for proper device enumeration, obtaining device GUIDs and capabilities, and publishing IORegistry entries for connected devices. Existing code already supports reading quadlets, which is the minimum required for Config ROM access.

Planned work:

1. Adopt other async commands from IOFireWireFamily: block read/write, lock, PHY, etc. These are straightforward based on existing async APIs.
2. Implement AV/C support required for the Apogee Duet FireWire audio interface to start isochronous transfers. This uses async block reads/writes and FCP (Function Control Protocol); some devices require CMP (Connection Management Procedure) negotiation.
3. Implement isochronous transfers. There's no need to reimplement Apple's DCL/NuDCL; basic isochronous DMA programs should suffice.
4. Add IRM (Isochronous Resource Manager) support for isochronous bandwidth allocation and management.

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

The project is organized into several components:

- **ASFW** — Control app and installer for the driver, plus debugging utilities. Written in Swift/SwiftUI. This app is the supported method to install DriverKit-based drivers on macOS.
- **ASFWDriver** — The main DriverKit-based FireWire driver implementation.

More detailed documentation for ASFWDriver is planned: [ASFWDriver README](https://github.com/mrmidi/ASFireWire/ASFWDriver/README.md).

## Building

Build scripts or CMakeLists are for quick testing and creating compile_commands.json for static analysis tools. The proper way to build and sign the driver is via Xcode.

NOTE: You need an Apple Developer account (paid) and appropriate entitlements — or a free account plus SIP disabled — to build/load the driver on your machine. See Apple's documentation for details: https://developer.apple.com/documentation/driverkit/debugging-and-testing-system-extensions

Enabling `systemextensionsctl developer on` is recommended — it allows installing system extensions from the build folder rather than copying it to '/Applications'

## Contributing

Contributions are VERY welcome! If you want to contribute to the project, please follow these steps:

1. Fork the repository on GitHub
2. Create a new branch for your feature or bugfix
3. Make your changes and commit them with clear messages
4. Push your changes to your forked repository
5. Open a pull request on the original repository, describing your changes and why they should be merged

Literally any help is appreciated, from fixing typos in documentation to implementing new features or fixing bugs. Writing tests, improving code quality, testing on hardware and reporting any bugs. If you have any experience with FireWire protocol - just opening an issue or emailing me is invaluable! If you have any experience with Swift - ASFW app could use some love too.

## Contacts

You can reach me via:

- Discord server: https://discord.gg/jAdXhrr2
- Email: me [at] mrmidi.net
- LinkedIn: https://www.linkedin.com/in/mrmidi/

## References

- [Apple DriverKit Documentation](https://developer.apple.com/documentation/driverkit) - NB. Oficcial documentation on Apple Developer website is incomplete and sometimes outdated. Refer to header files in DriverKit SDK for more accurate information.
- [Apple PCIDriverKit Documentation](https://developer.apple.com/documentation/pcidriverkit) s- Same as above
- [System Extensions and DriverKit](https://developer.apple.com/videos/play/wwdc2019/702/) — WWDC 2019 session introducing DriverKit and system extensions.
- [Modernize PCI and SCSI drivers with DriverKit](https://developer.apple.com/videos/play/wwdc2020/10670/) — Small but informative WWDC 2020 session about modernizing PCI and SCSI drivers.
- [IEEE 1394-2008 Standard](https://standards.ieee.org/ieee/1394/4377/) — Latest edition. This is most complete reference about FireWire 
