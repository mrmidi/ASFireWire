# Asynchronous Receive (AR) DMA — Design Notes and Implementation Guide

This document is a DriverKit‑friendly, Apple‑style white paper for implementing the OHCI 1394 Asynchronous Receive (AR) path in ASFireWire. It captures design intent, spec anchors, behavioral references from Linux (rewrite only), and concrete guidance for our `ASOHCIARContext`.

## Overview

AR DMA delivers incoming IEEE‑1394 request/response packets from the link into host memory. The controller uses descriptor programs to fill buffers and optionally raise interrupts when buffers are consumed. We implement Buffer‑Fill mode with a simple, reliable recycle strategy that is compatible with DriverKit constraints and Thunderbolt‑attached OHCI bridges.

Goals:
- Spec‑faithful: OHCI 1.1 compliant with 1394a timing nuances.
- DriverKit‑friendly: 32‑bit IOVA via `IODMACommand`, no busy‑waits, robust against resets.
- Apple‑style: clean layering, strong logging, predictable lifecycle (Start/Stop), separation of concerns.
- Evolvable: start with header‑peek logging, then grow into CSR/UC routing and user‑client delivery.

Non‑Goals (initially): isochronous streams, full transaction engine, user‑space copying.

## References

- OHCI 1394 OHCI 1.1 Specification
  - §3 Common DMA Controller Features (context registers, list management)
  - §5.7 Link state sequencing
  - §8 Asynchronous Receive DMA
    - §8.1 AR DMA Context Programs (INPUT_MORE)
    - §8.6 Interrupt behavior
  - §11 Self‑ID Receive (for sequencing around resets)
- IEEE 1394‑2008 (header layout, tCodes, Self‑ID format)
- Linux `ohci.c` (behavioral reference only; reimplemented in our style)
  - `ar_context_run()`, `ar_context_link_page()`, `context_stop()`

License note: Linux sources are used strictly as behavioral references. All code is rewritten in project style; no GPL code is mixed in.

## OHCI AR Summary

- Two AR contexts: Request and Response (base offsets typically 0x200/0x220).
- Program model: one or more INPUT_MORE descriptors form a linear or circular list.
- Completion: hardware updates residual count (`resCount`) and `xferStatus`, and optionally raises interrupts (ARRQ/ARRS, RqPkt/RsPkt).
- Buffer‑Fill mode: hardware writes packet(s) into the provided buffer until `reqCount` bytes are consumed or packet ends.

### Context Registers (per AR context)

- ContextControlSet/Clear: run/active/wake bits and status snapshot
- CommandPtr: descriptor program pointer (quadlet address >> 4 plus Z)

See `OHCIConstants.hpp` for offsets/masks and §3.1 of OHCI 1.1 for semantics.

## DMA Descriptors (INPUT_MORE)

Structure: `OHCI_ARInputMoreDescriptor` (16 bytes, 16‑byte aligned)

- cmd = 0x2 (INPUT_MORE)
- key = 0x0 (AR only)
- i   = 0x3 (interrupt on completion) or 0x0 to throttle
- b   = 0x3 (branch control)
- reqCount = buffer size (quadlet multiple)
- dataAddress = 32‑bit IOVA of buffer
- branchAddress/Z = next block pointer and count
- resCount/xferStatus updated by hardware on completion

Program forms:

- Linear: last descriptor `Z=0, branchAddress=0`; controller stops at end. Software re‑arms on interrupt.
- Circular: last descriptor points to the first (`Z=1, branchAddress = first >> 4`); controller runs continuously.

We start linear (simpler to reason about) and re‑arm on completion, then can evolve to circular once recycling is battle‑tested.

## DriverKit Mapping and Memory Model

- All AR resources use 32‑bit IOVA via `IODMACommand::Create` with `maxAddressBits=32`.
- Buffers: `IOBufferMemoryDescriptor` (direction In), optional CPU mapping for header‑peek.
- Descriptor chain: `IOBufferMemoryDescriptor` (coherent pool), DMA‑mapped once; program fields use DMA addresses.
- MMIO: use `IOPCIDevice::MemoryRead32/MemoryWrite32(barIndex, ...)` with the correct BAR.

## Initialization Flow (ASOHCIARContext)

1. Allocate N buffers of size S (default: N=4, S=4096) + CPU maps for header‑peek.
2. Map each buffer to 32‑bit IOVA with `IODMACommand::PrepareForDMA`.
3. Allocate descriptor chain and map to 32‑bit IOVA.
4. Build descriptors: set INPUT_MORE cmd, `reqCount=S`, `dataAddress=buffer IOVA`, chain via `branchAddress/Z`.
5. Program `CommandPtr` with descriptor chain DMA address (`>> 4`) and `Z=1` (points to next block) for the linear program start.
6. Set run bit in ContextControlSet.

### Bring‑up Sequencing (with Link)

Run AR contexts after interrupts are enabled and after LinkEnable. Mask/clear IRQs first (we do this in Phase 8/9), then start AR.

## Interrupt Handling and Recycle Strategy

Events of interest:
- DMA completion bits: ARRQ/ARRS (bits 2/3), RqPkt/RsPkt (bits 4/5)
- Bus events: BusReset/SelfIDComplete — AR should be stopped/restarted around these.

Minimal strategy implemented:
- On AR interrupt, scan descriptors; if `resCount < reqCount`, a buffer has data.
- Header‑peek: log the first up to 16 bytes (DriverKit‑safe logging, no sprintf).
- Recycle: set `resCount = reqCount`, clear `xferStatus`, leave addresses/counts unchanged.
- If the program ended (context inactive, or last descriptor Z=0 and controller stopped), re‑arm `CommandPtr` to the start, set `run`, then `wake`.

This keeps the ring running with a linear program and avoids starvation.

## Reset and Recovery

- On BusReset IRQ: stop AR contexts (clear run, wait inactive). Do not free buffers.
- After Self‑ID Complete (and post deferred cycle timer enable): restart AR contexts (re‑arm CommandPtr if needed).
- Clear/Mask: Follow the main interrupt handler’s masking rules; do not clear BusReset in bulk; ack it explicitly once processed.

## Logging and Diagnostics

- Context lifecycle: Initialize/Start/Stop logs with counts and sizes.
- Interrupt decode: use `ASOHCIInterruptDump` for human‑readable bit names.
- Header‑peek: emit single‑line hex for the first 16 bytes with context (ARRQ/ARRS, index, length).
- Guard noisy logs with compile‑time flags once stable.

Expected bring‑up logs (now):
- `ASOHCI: Initializing AR/AT DMA contexts`
- `ASOHCI: AR Request/Response context initialized and started`
- On traffic: `ARRQ RX[i] len=... peek: .. .. ..`

## Future Enhancements

- Circular descriptor program: last branch -> first for continuous run without re‑arm.
- Header parser: decode tCode/src/dst/len from 1394 header; route to CSR or user client.
- Filters: program AsReq/AsRsp filters precisely once topology is stable.
- Backpressure: dynamic `i` bit policy to reduce IRQ rate under load.
- Topology integration: use `ASOHCITopologyDB` to determine gap count and link policy before AR start.

## Implementation Checklist

- Buffers
  - [x] `IOBufferMemoryDescriptor` (In), CPU map for peek
  - [x] `IODMACommand` map to 32‑bit IOVA
- Descriptors
  - [x] Build INPUT_MORE chain (linear)
  - [x] DMA map chain to 32‑bit IOVA
- Context
  - [x] Program CommandPtr (base >> 4, Z=1)
  - [x] Start (run)
  - [x] Handle IRQ: detect fill, peek, recycle, re‑arm if ended, wake
  - [x] Stop on BusReset; restart after Self‑ID
- Logging
  - [x] Header peek logging without stdio formatting
  - [x] Use `ASOHCIInterruptDump` for event bits

## Validation Steps

1. Bring‑up: verify AR contexts start without errors; link produces no regAccessFail/unrecoverableError.
2. Traffic: generate any 1394 request (e.g., from analyzer or peer) → observe ARRQ/ARRS IRQs and header‑peek lines.
3. Resets: plug/unplug devices; confirm AR stops on BusReset and restarts after Self‑ID.
4. Stability: run for several minutes; verify no descriptor stalls (if seen, enable circular program or shorten buffers).

---

Document status: initial version (Aug 2025). Tracks current minimal AR ring with recycle + header‑peek and outlines next milestones.

