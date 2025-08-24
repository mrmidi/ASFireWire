# ASFireWire

DriverKit OHCI FireWire Host Controller for macOS

## Overview

ASFireWire implements a modern DriverKit-based FireWire OHCI host controller driver for macOS. This driver targets Thunderbolt-to-FireWire adapters and native FireWire controllers to enable FireWire device support on modern macOS systems.

## Target Hardware

- Primary target: `pci11c1,5901` (Agere/LSI FW800 controller on TB2→FW800 adapters)
- IEEE 1394 OHCI-compliant controllers

## Project Structure

- **ASFireWire**: SwiftUI application for driver management
- **ASOHCI**: DriverKit system extension implementing the OHCI host controller

## Build Requirements

- Xcode 14+
- macOS 13+ SDK
- Valid Apple Developer ID for system extension signing

## Development Status

🚧 **In Active Development**

- [x] Basic DriverKit extension skeleton
- [x] SwiftUI management application
- [ ] PCI device matching and binding
- [ ] OHCI controller initialization
- [ ] DMA buffer management
- [ ] Interrupt handling
- [ ] Isochronous transfer support

## Notes

This driver is designed for development and testing of FireWire audio interfaces and other professional FireWire devices on modern Mac hardware.
