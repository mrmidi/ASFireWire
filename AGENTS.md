# ASFireWire Project - Agent Work Documentation

## Project Overview

ASFireWire is a modern DriverKit-based FireWire OHCI host controller driver for macOS, targeting Thunderbolt-to-FireWire adapters and native FireWire controllers.

## Project Structure

### Working Directory
- **ASFireWire/** - Main project directory containing our implementation
  - **ASFireWire/** - SwiftUI application for driver management
  - **ASOHCI/** - DriverKit system extension implementing OHCI host controller
  - **README.md** - Project documentation
  - **AGENTS.md** - This file

### Reference Materials Directory (Root Level)

The root folder `IOFireWireFamily-IOFireWireFamily-490/` contains comprehensive reference materials:

#### Apple Source References
- **IOFireWireFamily.kmodproj/** - Apple's original IOFireWire kext source code for reference
- **IOFireWireLib.CFPlugInProj/** - User-space FireWire library implementation. We are not aimed to replicate this functionality.
- **Info-IOFireWireFamily.plist** - Original kext configuration

#### OHCI Specification & Linux Implementation References
- **ohci_11.pdf** - Official OHCI 1394 specification document
- **ohci.c** - Linux OHCI driver implementation (behavioral reference only)
- **ohci.h** - Linux OHCI register definitions and constants  
- **init_ohci1394_dma.c** - Linux DMA initialization routines (behavioral reference only)

#### Modern DriverKit API References
- **AppleHeaders/** - Current DriverKit API headers and definitions
  - **IOPCIDevice.h** - PCI device interface
  - **IOPCIDevice.iig** - PCI device DriverKit interface
  - **PCIDriverKit.h** - PCI driver framework

#### Documentation
- **docs/** - Project documentation directory
  - Allowed to create new or modify existing documentation as needed

## Legal Compliance & License Notes

⚠️ **IMPORTANT**: License compatibility requirements:

- **Linux Sources (GPL 2.0)**: Used ONLY for behavioral reference and implementation patterns
- **No Direct Code Copying**: Cannot mix GPL 2.0 with Apple Public Source License 1.1
- **Rewrite Requirement**: All code must be rewritten in Apple/project style
- **Reference Only**: Linux sources provide implementation guidance, not copy-paste material

## Development Workflow

### Reference Usage Priority
1. **DriverKit APIs**: Use AppleHeaders/ for all modern API implementations
2. **OHCI Specification**: Use ohci_11.pdf for official hardware behavior
3. **Linux Implementation**: Use ohci.c/h for behavioral patterns (rewrite required)
4. **Apple Kext Source**: Use IOFireWireFamily for Apple-specific patterns

### Commit Rules
**Build → Test → Commit Local → Remote only on explicit request**

1. **Build Phase**: Ensure clean compilation with no errors/warnings
2. **Test Phase**: Verify basic functionality (driver loading, device detection)
3. **Local Commit**: Commit to local git repository with descriptive messages
4. **Remote Push**: Only push to GitHub when explicitly requested

### Implementation Guidelines
- Use comprehensive logging for debugging (os_log)
- Follow Apple coding conventions and DriverKit patterns
- Reference OHCI spec for hardware behavior verification
- Test incrementally with each milestone
- Document all implementation decisions

## Current Development Status

**Phase 1: PCI Device Matching and Basic MMIO**
- [x] Dual personality Info.plist configuration
- [x] PCI device matching entitlements
- [x] Basic PCI device handling implementation
- [ ] Build verification and testing
- [ ] Local commit

**Next Phases**:
- Phase 2: MSI Interrupt Handling
- Phase 3: DMA Buffer Management  
- Phase 4: OHCI Controller Initialization
- Phase 5: Context Ring Setup
- Phase 6: Isochronous Transfer Support

## Target Hardware

Primary: `pci11c1,5901` (Agere/LSI FW800 controller on TB2→FW800 adapters)
Secondary: IEEE 1394 OHCI-compliant controllers

## Development Tools & Debugging

### Core Development Tools
- **ioreg** - IOKit registry exploration for PCI devices and driver status
- **systemextensionsctl** - System extension lifecycle management
- **xcodebuild** - Project compilation and building
- **git** - Version control and commit management
- **gh CLI** - GitHub repository management
- **ripgrep (rg)** - Fast code searching and pattern matching

### Tool Installation Policy
If a required tool is not available, explicitly request user installation before proceeding with tasks that depend on it.

### IOKit Registry Exploration
```bash
# Check PCI devices and FireWire controllers
ioreg -l -w0 -r -c IOPCIDevice | grep -A10 -B5 "11c1\|1394\|firewire"

# Monitor driver personalities and matching
ioreg -l -w0 -r -c IOUserService | grep -A5 -B5 "ASOHCI\|ASFireWire"

# Examine specific device properties
ioreg -lw0 -p IOService -r -n "pci11c1,5901@0"
```

### System Extension Management
```bash
# List all system extensions
systemextensionsctl list

# Check extension status and staging
systemextensionsctl developer list

# Reset system extension state (if needed)
systemextensionsctl reset
```

### Driver Log Monitoring
Use the provided `log.sh` script for comprehensive driver debugging:
```bash
# Real-time log streaming
./log.sh

# Log to file for analysis
./log.sh --file
```

## Notes for Future Development

- Maintain strict separation between reference material and implementation
- All behavioral insights from Linux sources must be rewritten from scratch
- Use OHCI specification as the authoritative hardware reference
- Leverage Apple's DriverKit patterns for proper macOS integration
- Test thoroughly on actual hardware before each milestone commit
- Use ioreg and systemextensionsctl for status verification at each phase

## Build Commands Reference

```bash
# Build just the ASOHCI dext target
xcodebuild -project ASFireWire.xcodeproj -target ASOHCI -configuration Debug build

# Or build the entire project (app + dext)
xcodebuild -project ASFireWire.xcodeproj -scheme ASFireWire -configuration Debug build

# For cleaner output, you can also build specific architecture
xcodebuild -project ASFireWire.xcodeproj -target ASOHCI -configuration Debug -arch arm64 build
```