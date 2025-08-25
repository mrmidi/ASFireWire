# ASFireWire DriverKit Project

Experimental DriverKit-based OHCI IEEE 1394 (FireWire) controller bring-up.

## Current Status (Commit f76162c)
- Core bring-up: PCI enable, HC soft reset, LPS + Posted Writes, BusOptions (ISC|CMC set), provisional NodeID, LinkEnable.
- Interrupts: Minimal mask (SelfIDComplete, BusReset, PHY, RegAccessFail, MasterEnable) with BusReset coalescing.
- Self-ID: 32-bit DMA buffer armed; parser implements IEEE 1394 alpha tag-run filtering and node/port decode.
- Cycle Timer: Deferred until after first Self-ID completion.
- PHY Access: Serialized register access helper (`ASOHCIPHYAccess`) using `IORecursiveLock`, currently polling.
- One-shot PHY Scan: Executes after first Self-ID to log basic port status (read-only for now).

## Pending Technical Tasks
(See detailed `docs/init/checklist.md` and TODO items tracked externally.) High-priority upcoming:
1. programPhyEnable gating around grouped PHY register accesses.
2. Complete OSAction in interrupt handler (ensure `action->Complete`).
3. Reorder DMA teardown (Stop) – complete/release DMA before unmap/buffer release.
4. Add `kOHCI_LinkControl` read alias constant; use for readbacks.
5. Trim duplicate "Other IRQ bits" logging noise.
6. C7 docs update (checklist, sequence, logging instrumentation for PHY scan).
7. C8 stabilization (quiet window / reset debounce) design + implementation.
8. C10 controller state dump (register snapshot post-bring-up / post-stable bus).

## Repository Layout (Selected)
- `ASOHCI/` : Core OHCI driver sources (`ASOHCI.cpp`, `OHCIConstants.hpp`, `PhyAccess.*`, `SelfIDParser.*`).
- `ASFireWireApp.swift` : Companion app / harness (Debug UI scaffolding).
- `docs/init/` : Initialization documentation (checklist, sequence, parity, logging instrumentation, etc.).
- `docs/` : Broader reference & roadmap docs.

## Build Notes
- Targets require DriverKit SDK (Xcode 15 / macOS 14+ environment expected).
- Self-ID buffer uses 32-bit DMA IOVA; ensure controller BAR0 is mapped and bus mastering enabled.
- Logging via `os_log` plus custom bridge log ring (retrievable through driver interface `CopyBridgeLogs`).

## Next Steps (Short Horizon)
| Task | Rationale |
|------|-----------|
| Add programPhyEnable wrap | Improve reliability of PHY register cycles (avoid read timeouts) |
| ISR action completion | Prevent potential dispatch back-pressure / dropped IRQs |
| DMA teardown reorder | Avoid use-after-release risk with active DMA command |
| LinkControl alias | Clearer semantics for read vs write-1-to-set address reuse |
| Logging trim | Reduce redundant lines for cleaner trace analysis |

## Contributing / Experimentation
This project is exploratory; interface stability is not guaranteed. Validate changes with verbose logging enabled and capture full bring-up trace for regressions.

## License
See root project license (if present) or `APPLE_LICENSE` in the parent workspace for original Apple material references. Newly authored code here is provided without additional license headers unless explicitly added later.
