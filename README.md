# ASFireWire

DriverKit OHCI (IEEE 1394) Host Controller for modern macOS.

> Goal: Restore practical FireWire connectivity (especially for pro audio devices) on Apple Silicon / modern Intel Macs via DriverKit system extensions.

---

## Overview
ASFireWire comprises a SwiftUI management app and a DriverKit system extension implementing a minimal-but-growing OHCI (Open Host Controller Interface for IEEE‑1394) stack. Development proceeds in clearly defined bring‑up phases (C1+), each adding hardware parity, stability, or diagnostics.

## Target Hardware
Primary focus is on Thunderbolt‑to‑FireWire adapters and native PCIe FireWire controllers:
- `pci11c1,5901` (Agere/LSI FW800 – common in TB2→FW800 adapters)
- Generic IEEE‑1394 OHCI compliant controllers (probing carefully gated during early phases)

## Architecture (High Level)
- **ASFireWire (App)**: SwiftUI front-end for install/activation status, logging visibility, and future device enumeration UI.
- **ASOHCI (System Extension)**: DriverKit C++ code managing PCI device, MMIO registers, DMA resources, interrupts, PHY, and bus management.
- **Support Components**:
  - Self‑ID parser (filters tagged quadlet run, decodes node descriptors)
  - PHY access helper (serialized register access with lock + polling)
  - Logging bridge (structured bit decoding for interrupts & state dumps – expanding)

## Bring‑up Phase Progress
Legend: ✅ complete | 🛠 in progress | ⏭ planned

| Phase | Focus | Status | Notes |
|-------|-------|--------|-------|
| C1 | Basic DriverKit skeleton & PCI attach | ✅ | App + extension scaffolding |
| C2 | Minimal register mapping & sanity logging | ✅ | Initial MMIO + basic prints |
| C3 | Interrupt hookup & safe mask | ✅ | Limited mask, verified delivery |
| C4 | Bus reset handling (collapse noise) | ✅ | Coalesced BusReset events |
| C5 | Self‑ID DMA + parser foundation | ✅ | Tag run filtering implemented |
| C6 | Cycle timer deferral | ✅ | Enable after first stable Self‑ID |
| C7 | Serialized PHY access + initial scan | 🛠 | Helper present; add programPhyEnable gating & doc updates |
| C8 | Reset stabilization "quiet window" | ⏭ | Collect Linux refs, implement gating logic |
| C9 | Enhanced logging & noise reduction | 🛠 | Filtering duplicate IRQ bit lines planned |
| C10 | Controller state dump instrumentation | ⏭ | Define register snapshot set & formatting |

## Current Capability Snapshot
- Enumerates target PCI device and maps OHCI register space
- Handles and coalesces bus reset interrupts
- Performs Self‑ID DMA receive & parses valid tagged quadlet run
- One‑shot post‑Self‑ID PHY port scan (initial) with serialized register access
- Defers cycle timer enable until first valid Self‑ID completion

## Active / Near‑Term Tasks
Extracted from internal TODO tracking (subset):
1. Add `programPhyEnable` gating around grouped PHY accesses
2. Ensure ISR completes OSAction on all exit paths
3. Reorder DMA teardown in `Stop()` to avoid premature buffer release
4. Introduce `kOHCI_LinkControl` read alias constant (parity clarity)
5. Reduce duplicate "Other IRQ bits" logging (mask out already decoded bits)
6. Update docs for C7 (checklist + logging sequence)
7. Gather Linux references for C8 (quiet window strategy) & implement stabilization timer
8. Plan and implement C10 `DumpControllerState` (register snapshot at bring‑up + post‑stabilization)
9. Parity doc refresh after adding any new bit constants

## Roadmap (Selected)
- Short Term: Reliability (PHY gating, ISR completion), stabilization (quiet window), structured state dump.
- Medium Term: Transfer rings (ATREQ/ATRSP), async transaction helpers, improved error recovery.
- Longer Term: Isochronous transmit/receive support, higher-level device enumeration, user‑space service bridging, potential FireWire Audio class integration.

## Project Structure (Simplified)
```
ASFireWire/ (this folder)
  ASFireWire/            SwiftUI management app
  ASOHCI/                DriverKit OHCI system extension sources
  docs/                  Design notes & phase checklists
  AppleHeaders/          Curated DriverKit/PCI reference headers (for parity)
  test/                  (Future) Unit / harness tests for parsers & helpers
```

## Build Requirements
- Xcode 14+ (DriverKit support)
- macOS 13+ SDK (extensible system extensions environment)
- Apple Developer ID (system extension signing & notarization)

## Building & Installing (Developer Workflow)

1. **Using the automated build script** (recommended):

   ```bash
   ./build.sh              # Standard build with error/warning filtering
   ./build.sh --verbose    # Verbose build output
   ./build.sh --help       # Show help and options
   ```

   The build script will:
   - Bump the version number
   - Build the project with error/warning filtering
   - Save build logs to `./build/build.log`
   - Show a summary of errors and warnings found

2. **Manual Xcode build**:
   - Open the workspace / project in Xcode
   - Select the system extension target under the app's scheme (the app embeds/activates the extension)
   - Ensure a provisioning profile permitting DriverKit / System Extension entitlements
   - Build & Run the app; approve system extension in System Settings → Privacy & Security if prompted

3. **View logs**:
   - Build script output: `cat ./build/build.log`
   - Only errors: `grep 'error:' ./build/build.log`
   - Only warnings: `grep 'warning:' ./build/build.log`
   - Console.app filtering for bundle identifiers, or
   - `log stream --predicate 'subsystem == "com.example.ASFireWire"'` (replace with actual subsystem once finalized)

## Development Notes

- Emphasis on incremental parity with Linux `ohci1394` / related driver behavior for reset, Self‑ID, and PHY operations.
- Logging intentionally verbose during early phases; will be toned down as stability (C8) lands.
- Self‑ID parsing restricts to contiguous tagged quadlets (b31..30 == 10) matching spec guidance & Linux approach.

## Contributing / Feedback

Early stage; external contributions not yet accepted until core stabilization phases (through C10) are complete. Issues & discussion (design insights, hardware quirks) are welcome.

## Disclaimer

For development & research. Not production‑hardened; may hang hardware or require reboot in fault scenarios. Use only on test systems and with backed‑up data.

## License

Project source under the terms provided in `APPLE_LICENSE` (and any per‑file headers where applicable). Review before redistribution.

---
Last updated: (auto) pending tasks prior to implementing C7 gating & ISR completion.
