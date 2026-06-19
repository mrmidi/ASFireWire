# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) and other ai agents when working with code in this repository.

## Project Overview

ASFW is a macOS DriverKit-based FireWire (IEEE 1394) driver restoring FireWire functionality removed in macOS Tahoe (26). It uses PCIDriverKit for user-space OHCI controller access and AudioDriverKit for CoreAudio integration. TODO: MIDIDriverKit and SCSIControllerDriverKit. 

Two components:
- **ASFWDriver/** — C++23 DriverKit driver extension (dext)
- **ASFW/** — Swift 6 control app and installer (required to install the dext)

## Build Commands

**Primary build (Xcode — required for signing and producing `.dext`):**
```bash
./build.sh                        # Quiet build (errors/warnings only)
./build.sh --verbose              # Full xcodebuild output
./build.sh --no-bump              # Skip version bump
./build.sh --config Release       # Release build
```

**Generate `compile_commands.json`** (for clangd, static analysis):
```bash
./build.sh --commands
# or via CMake:
cmake -S . -B build && cmake --build build --target compile_commands
```

**C++ unit tests** (no hardware/DriverKit needed):
```bash
./build.sh --test-only                        # Build + run all C++ tests
./build.sh --test-only --test-filter Pattern  # Run tests matching a regex

# Or directly with CMake/CTest:
cmake -S tests -B build/tests_build
cmake --build build/tests_build -- -j$(sysctl -n hw.ncpu)
ctest --test-dir build/tests_build -V
ctest --test-dir build/tests_build -V -R TopologyManager   # single test suite
```

**Swift/XCTest tests:**
```bash
./build.sh --swift-test-only
./build.sh --swift-coverage       # With LCOV export for SonarCloud
```

## Architecture

> The driver is layered. Read the **layer-boundary rule** in *Critical Rules* before
> moving code between subsystems — the boundaries below are load-bearing, not cosmetic.

### Layering (high → low)

```
CoreAudio / HAL
      │
  Audio/            ← AudioDriverKit stack: CoreAudio semantics, format, clock
      │
  Audio/Wire        ← content framing: IEC 61883 / CIP / AM824 (builds the CIP-headered stream)
      │               (CIP spans DV/MPEG/audio — provisional home; must NOT move into transport)
  ── seam ──        ← fully-framed isoch packets + neutral control block (via Audio/Ports)
      │
  Isoch/ Async/     ← FireWire transport: OHCI DMA, descriptors, isoch packets.
  Bus/ Hardware/      Payload-opaque — knows neither audio nor CIP. + bus policy + raw MMIO
      │
  OHCI controller (PCIDriverKit) → FireWire bus
```

### ASFWDriver Subsystems (all under `ASFWDriver/`)

**Transport / bus mechanism** — payload-agnostic; must not know about audio:
| Directory | Responsibility |
|-----------|---------------|
| `Hardware/` | OHCI MMIO register layout, interrupt/event definitions |
| `Phy/` | PHY packet formats |
| `Async/` | Async AT/AR pipeline: `Commands`, `Contexts`, `Core`, `Engine`, `Interfaces`, `Rx`, `Tx`, `Track` |
| `Isoch/` | OHCI **isochronous transport only**: `Core`, `Config`, `Memory`, `Receive`, `Transmit` |
| `Shared/` | Shared DMA primitives: `Rings`, `Memory`, `Completion`, `Contexts`, `Hardware`, `Isoch` |

**Bus policy / discovery:**
| Directory | Responsibility |
|-----------|---------------|
| `Bus/` | Reset, Self-ID, topology, gap-count, generation: `BusManager`, `CSR`, `IRM`, `Role`, `Timing` |
| `ConfigROM/` | Config ROM build/stage/read/scan: `Local`, `Remote`, `Parse`, `Store`, `Common` |
| `Discovery/` | FireWire device + unit enumeration (`FWDevice`, `FWUnit`) |
| `Controller/` | Controller state machine and lifecycle |
| `Protocols/` | `AVC` (FCP, Music Subunit, stream formats, PCR), `SBP2`, `Ports` (register/PCR IO) |

**Audio (AudioDriverKit stack)** — owns content format + CIP/61883 framing; must not reach into OHCI/transport mechanics:
| Directory | Responsibility |
|-----------|---------------|
| `Audio/DriverKit/` | `ASFWAudioDriver.iig` + `ASFWAudioNub.iig` — CoreAudio HAL side |
| `Audio/Wire/` | Content framing: `IEC61883`, `CIP`, `AMDTP`, `AM824`, `RawPcm24In32`. Builds the CIP-headered stream handed to transport. CIP spans DV/MPEG/audio — **provisional home, not transport** |
| `Audio/Runtime/` | Timing/buffer geometry: `HostClockAnchor`, `PlaybackRingRange` |
| `Audio/Ports/` | Seam interfaces: `IAmdtpTxSlotProvider`, `ICycleTimeline`, `IDiagSink` |
| `Audio/Engine/`, `Audio/Core/`, `Audio/Model/`, `Audio/Config/`, `Audio/Protocols/` | Engine wiring, runtime model, config |

**Composition / cross-cutting:**
| Directory | Responsibility |
|-----------|---------------|
| `Service/` | `DriverContext` (the `ServiceContext`/DI root), `LocalRequestWiring` |
| `Scheduling/` | `Scheduler`, `WatchdogCoordinator` (timer/dispatch) |
| `DeviceProfiles/` | Device capability profiles: `Audio`, `Common` |
| `Diagnostics/` | `DiagnosticsService`, `ControllerMetrics`, metric sinks |
| `Debug/` | `AsyncTraceCapture`, `BusResetPacketCapture` |
| `Snapshot/` | State snapshot |
| `UserClient/` | DriverKit user-client: `Core`, `Handlers`, `Storage`, `WireFormats` |
| `Logging/` | Structured logging |
| `Common/` | `FWCommon.hpp`, barrier utilities |
| `Testing/` | Host-test stubs (`FakeDMAMemory`, `HostDriverKitStubs`, `TestHooks`) |
| `Version/` | Generated `DriverVersion.hpp` |

Key entry points:
- `ASFWDriver/ASFWDriver.iig` / `ASFWDriver.cpp` — driver class and `Start`/`Stop`
- `ASFWDriver/UserClient/Core/ASFWDriverUserClient.iig` — user-client interface
- `ASFWDriver/Audio/DriverKit/ASFWAudioDriver.iig` + `ASFWAudioNub.iig` — AudioDriverKit engine + nub

### Audio Pipeline (direct-binding model)

```
TX:  CoreAudio → shared output buffer (IOBufferMemoryDescriptor) → Audio/Wire encode
       (RawPcm24In32 → AM824/CIP/AMDTP) → IsochTransmitContext → OHCI IT DMA → FireWire bus
RX:  FireWire bus → OHCI IR DMA → IsochReceiveContext → directInputView_ writes shared input
       buffer + AudioTransportControlBlock → Audio stack reads → CoreAudio HAL
```

The **seam** is a `DirectBindingSource` / `directInputView_` view onto a shared
`AudioTransportControlBlock` (in an `IOBufferMemoryDescriptor`), crossed through `Audio/Ports`
interfaces. The two sides are **separate IOService objects on separate dispatch queues**
(`ASFWDriver-Default`, `ASFWAudioNub-Default`, `com.asfw.audio.dice`). Lifetime across that
seam is delicate — see FW-60 (cross-service UAF/teardown crashes) for what goes wrong when
the transport layer holds raw pointers into audio-owned memory.

Audio device publication: discovery creates `ASFWAudioNub` with discovered capabilities;
`ASFWAudioDriver` matches on the nub and registers `IOUserAudioDevice` with the CoreAudio HAL.

### Async Transaction Flow

Command → `ATContextBase` → descriptor builder → `DescriptorRing` → OHCI DMA → interrupt → `ARPacketParser` → `PacketRouter` → `TransactionManager` completion callback (via `LabelAllocator` tLabel matching).

### Config ROM Subsystem (`ASFWDriver/ConfigROM/`)

The Config ROM pipeline is split into small, single-purpose components:

| Component | Role |
|-----------|------|
| `ConfigROMBuilder` | Builds the local node's ROM image (quadlet array + CRC) |
| `ConfigROMStager` | Programs OHCI shadow registers and stages the local ROM (casts isolated in `MemoryMapView`) |
| `ROMReader` | Issues async **quadlet** reads against Config ROM address space at `0xFFFFF0000400` |
| `ROMScanner` | FSM-driven multi-node discovery; callback-based `Start()` completes once per generation request |
| `ConfigROMParser` | Pure parsing helpers (`ParseBIB`, `ParseTextDescriptorLeaf`, bounded scans) |
| `ConfigROMStore` | Thread-safe cache of discovered ROMs |

**Bus Info Block quadlet layout (TA 1999027 + IEEE 1212):**
```
Quadlet 0 (header):    [31:24] bus_info_length  [23:16] crc_length  [15:0] crc
Quadlet 1 (bus name):  0x31333934 ("1394")
Quadlet 2 (bus opts):  [31]irmc [30]cmc [29]isc [28]bmc [27]pmc
                       [23:16]cyc_clk_acc  [15:12]max_rec
                       [11:10]reserved  [9:8]max_ROM  [7:4]generation  [3]reserved  [2:0]link_spd
Quadlets 3–4:          GUID (hi, lo)
```

Use `ASFW::FW::DecodeBusOptions(q2)` / `EncodeBusOptions(d)` / `SetGeneration(q2, gen)` from `FWCommon.hpp`. **Never** access bus options bits directly. The old `BIBFields` namespace had every position wrong (it read from quadlet 0 instead of quadlet 2).

**Text descriptor leaf layout (IEEE 1212-2001 Figure 28):**
```
+0: [leaf_length:16][crc:16]
+1: [descriptor_type:8][specifier_ID:24]  — must be 0x00000000 for minimal ASCII
+2: [width:8][character_set:8][language:16] — must be 0x00000000 for minimal ASCII
+3..: ASCII characters, big-endian packed, NUL-terminated
```
`typeSpec` is at `+1`, **not** `+2`. Stop parsing at the first NUL byte.

**`ROMScanner` one-shot completion guard:**
`CheckAndNotifyCompletion()` is called from async callback sites. It fires the per-scan completion exactly once when all nodes reach `Complete`/`Failed` and `InflightCount() == 0`. It uses the `ROMScannerCompletionManager` latch (reset by `Start()` / `Abort()`) to prevent double-firing: queued `ScheduleAdvanceFSM()` dispatches can arrive after the first completion, see the same terminal state, and try to signal again.

**`EnsurePrefix` pattern:**
When `OnRootDirComplete` needs data beyond the root directory (leaves, unit dirs), it calls `EnsurePrefix(nodeId, requiredTotalQuadlets, completionCallback)` which transparently grows `node.partialROM.rawQuadlets` via additional async reads. The completion lambda chains further `EnsurePrefix` calls for nested structures (text leaves, descriptor directories, unit directory entries). Always call `ScheduleAdvanceFSM()` at the end of `EnsurePrefix` callbacks, never `AdvanceFSM()` directly (re-entrancy guard).

**`ROMReader` header-first mode:**
Pass `count=0` to `ReadRootDirQuadlets()` to enable autosize: the reader issues a 4-byte header read first, extracts `entry_count` from bits **[31:16]** of the directory header (not `[15:0]` — that's the CRC field), then reads the exact number of entries. Capped at 64 entries.

### Reference Material (`references/` — local-only, gitignored)

In-tree reference stacks live under `references/` (gitignored — **not** committed). They are the
authoritative behavioral sources for the wire-compat doctrine. **Because the dir is gitignored, do
not assume any reference is present** — `ls references/` first; if the one you need is missing, tell
the user, name the upstream, and offer to fetch it (see the *References are read-only* rule).

| `references/` dir | What it is | Upstream |
|-------------------|-----------|----------|
| `linux-ohci-firewire-low-level-stack/` | Linux low-level FireWire/OHCI driver (authoritative for OHCI mechanism + descriptor layout) | github.com/torvalds/linux `drivers/firewire` |
| `linux-sound-firewire-stack/` | Linux ALSA FireWire audio drivers (AMDTP/CIP, device families) | github.com/torvalds/linux `sound/firewire` |
| `alsa-userspace-control-protocols-impl/` | ALSA userspace control protocols (TCAT/DICE, vendor control) | github.com/alsa-project/snd-firewire-ctl-services |
| `libffado-2.5.0/` | FFADO userspace FireWire audio stack | ffado.org/files/libffado-2.5.0.tgz |
| `IOFireWireFamily.kmodproj/` | Apple's original FireWire kext (authoritative for policy/ordering) | apple-oss-distributions/IOFireWireFamily |
| `IOFireWireAVC/` | Apple's AV/C protocol kext | apple-oss-distributions/IOFireWireAVC (AVC-434) |
| `IOFireWireSBP2/` | Apple's SBP-2 kext | apple-oss-distributions/IOFireWireSBP2 (SBP2-452) |
| (fetch if needed) | Apple SBP transport | apple-oss-distributions/IOFireWireSerialBusProtocolTransport (261) |

The `ADKVirtualAudioLab/` (committed) is a closed, hardware-free AudioDriverKit lab — the
runtime-truth source for ADK lifecycle/ownership questions (see its `README.md`, O1–O3). It is
immune by construction to the cross-service bugs, so use it to *answer* ADK behavior, not to
template the FireWire side.

## Critical Rules and Gotchas

**Layer boundaries are load-bearing — don't reach across them.**
- The **FireWire core / transport** (`Hardware/`, `Async/`, `Isoch/`, `Bus/`) moves bytes and manages the bus. It carries fully-framed isochronous packets **opaquely**: it must **not** know that the payload is audio, and must **not** parse or build CIP / IEC 61883 / AM824. No CoreAudio concepts, no content-format knowledge, no pointers into audio-owned memory.
- **CIP / IEC 61883 / AM824 framing is content knowledge, not transport.** It currently lives under `Audio/Wire/` and builds the CIP-headered stream handed to transport. Because CIP spans all 61883 content types (DV, MPEG, audio), its final home is undecided — but it must never migrate *into* transport. (This is a different boundary from the audio↔transport one; don't conflate them.)
- The **audio stack** (`Audio/`) owns CoreAudio semantics, format, and clock. It must not reach into OHCI/transport mechanics (DMA, descriptors, MMIO).
- The **seam** is a fully-framed isoch packet stream plus a **neutral, lifetime-owned contract** (buffer view + control block) crossed through `Audio/Ports/` — not raw cross-service pointers. **FW-60 is what happens when this is violated**: the transport layer dereferenced an audio-owned control block across the boundary → use-after-free on teardown.

**Endianness:** OHCI descriptor headers are little-endian; IEEE 1394 wire payloads are big-endian. Use `ToBusOrder`/`FromBusOrder` (defined in `FWCommon.hpp`) explicitly. Never assume.

**IT descriptor layout:** The `OUTPUT_MORE_IMMEDIATE` skip address lives at offset `0x08` (Branch Word), **not** `0x04`, despite some OHCI 1.1 diagrams. Follow Linux `firewire-ohci` + Apple validated behavior. See `ASFWDriver/Isoch/README.md`.

**Constants:** All OHCI hardware register constants go in `ASFWDriver/Hardware/OHCIConstants.hpp` (single source of truth). Never define them in `.cpp` files or class headers.

**OHCI timing:** Context stop/quiesce requires polling with timeout and escalating delays (5µs → 255µs). Do not assume immediate hardware response.

**DMA coherency:** Call `OSSynchronizeIO`/`IoBarrier` after writing descriptors before waking hardware. Read descriptor status fields before acting on completion data.

**Cross-service lifetime / teardown ordering.** The core and audio sides are separate IOServices on separate queues. Teardown (uninstall, disconnect, hot-unplug, recovery/restart) must quiesce dependent queues and drop cross-seam views **before** freeing buffers or detaching hardware. No code may issue MMIO after `hardware->Detach()`, and no view may outlive the mapping it points into. See FW-60.

**IIG files:** `.iig` interface files require Xcode's IIG preprocessor to generate `.iig.cpp`. CMake builds exclude these; production builds must use Xcode.

**Test isolation:** All C++ tests compile with `ASFW_HOST_TEST` defined, which stubs out DriverKit APIs. Logic tested this way cannot cover actual hardware interaction.

**Wire compatibility is the correctness bar.** ASFW is general-purpose but typically tested only against audio hardware. For untestable device classes/topologies, "correct" means *behaves like the in-tree reference stacks* (`references/linux-*` = Linux, authoritative for OHCI mechanism; `references/IOFireWireFamily.kmodproj/` = Apple, authoritative for policy/ordering). Spec is the floor, the references are the ceiling. Internal architecture is free; observable **bus behavior must conform**. Only deviate from the references with hardware in hand — "cleaner than the reference" is an untested behavior. This is sharpest at the bus-policy layer (root/cycle-master/reset/gap), which is global state affecting every device at once.

**References are read-only behavioral sources — do not copy code.** `references/` holds GPL (Linux, libffado), LGPL, and APSL (Apple) source. Copying any of it verbatim imports its license onto the dext (Linux = GPLv2 — a hard no). Use references to learn *wire/behavioral* truth, then write fresh, modern C++ following DriverKit idioms. Apple's IOFireWire is C++98-era — correct for *behavior*, wrong to copy 1:1 in 2026; Linux is C/Rust/old C++ — same. If a non-trivial fragment is genuinely adapted (not just behavior), add a header attribution + the source's license and confirm license compatibility first.

**Check references before implementing anything wire-observable** — not just device-specific audio features. This covers OHCI mechanism (DMA/descriptors/contexts), bus policy (reset/gap/IRM/BM/topology), protocol logic (AVC, SBP-2, Config ROM, CSR), *and* device-class features (Alesis, MOTU, RME, DICE/TCAT, …). `ls references/` — is there a Linux/FFADO/ALSA/Apple source covering this? If yes, read it and cite `file:line`. If **not**, tell the user, name the likely upstream, and **warn that the implementation will be synthesized and is probably wrong on the wire** without a reference. Offer to `git clone`/`curl` it into `references/` and ask the user to confirm the source. If the behavior depends on a spec and cannot be clarified from Linux, Apple, FFADO, or other local references, ask the user to confirm or manually check the relevant spec; do not guess.

**Ground truth per question type** (don't hallucinate, don't guess a `§`):
- **API surface** (DriverKit / AudioDriverKit / PCIDriverKit signatures, enums, entitlements) → `ctx7` / `find-docs` / SDK headers. The trap: `ctx7` will confidently answer about a *single method in isolation* while omitting the surrounding contract (call ordering, what must precede/follow, ownership, concurrency, post-`Stop` behavior). A right per-method answer is **not** a right *lifecycle* answer.
- **Behavioral contract** (lifecycle, ownership, ordering, concurrency) → validate empirically against `ADKVirtualAudioLab` or working reference kexts — never from recall.
- **Paywalled specs** (IEEE 1394 / OHCI / IEC 61883) → **not** available via `ctx7`/`find-docs`. Source from in-repo docs or ask the user. If you don't know the exact section, say so and ask — never invent one.
- **Wire behavior** (Linux / libffado) → cite the in-tree ref you cross-checked, e.g. `// cross-validated with Linux amdtp-stream.c:412`.

**Math is an assumption, not a guarantee.** Geometry, buffer sizes, and timing derivations assume constants the real world doesn't honor — HW and macOS clocks drift, packets and audio frames drop. A derivation exact on paper still needs slack (safety frames, lead/cushion). Validate timing/buffer math with the simulators in `tools/*.py` and design for the degraded case, not the ideal one.

**General complexity.** ASFW is an extremely complex project — the audio stack alone touches OHCI / IEEE 1394, IEC 61883-1, IEC 61883-6, CoreAudio, and stream/buffer geometry at once. A fix that looks local to one field often breaks another. Trace a change through every layer it touches before committing.

**Hardware testing is expensive** (rebuild, install, connect/disconnect). Reserve quick HW checks for when a theory is a blocker and only HW can settle it. Otherwise design the full solution first, verify against tests + the reference stacks, and batch HW verification at the end.

**Fallback and legacy support.** Avoid double paths when implementing or fixing behavior; they often lead to days of debugging. Prefer to validate the new approach, then remove the superseded path and any dead code. If the old code is obviously wrong, delete it. If the migration boundary is unclear, warn or ask before leaving both paths alive.

**Instrumentation.** Features and fixes should be traceable, but do not add IO or noisy logging to hot paths. Design instrumentation alongside the feature, suggest the relevant `log stream` command or predicate, and ask the user for runtime state when needed (for example: driver running, audio playing, device connected). Otherwise the trace may prove nothing. Gate hot-path telemetry **anomaly-only once the happy path is confirmed**: keep draining any telemetry ring so it never overflows, but emit a log line only on a real fault (e.g. `[PayloadWriter]` logs only on deficit/withoutPkt/raced/`written != visited`; `[TxPrepRange]` only on `stoppedShort`/`frameShort`), leaving one coarse liveness/margin heartbeat (`[TxPrep]`). A clean run then prints only the heartbeat, and any other line means a regression.

**The agent's Bash sandbox cannot read the unified log.** `log show` / `log stream` return **zero lines silently** under the sandbox (not an error — every predicate looks "empty"), so the agent cannot capture dext logs itself. Confirm the dext is alive with `systemextensionsctl list` (that works), but for the actual trace **hand the user a ready `log stream … | grep -m N … > file` command to run via the `!` prefix**, then read the file. DriverKit dexts have no real os_log categories (`os_log_create` is unavailable — everything is `OS_LOG_DEFAULT` from `kernel`), so the category lives only in the message-text prefix: filter on `eventMessage CONTAINS "[Tag]"`, and pass `--info --debug` or the lines (logged below default level) won't appear.

**Commit and git history.** Keep history traceable. If changes are getting large, warn the user that it is better to commit the current work first; otherwise unrelated logic shifts can become hard to repair or reason about.

## Code Patterns

- **Error handling:** `std::expected<T, E>` — no exceptions in driver code. Mark all error-returning functions `[[nodiscard]]`.
- **CRTP** for compile-time context role enforcement (AT Request vs AT Response, etc.).
- **RAII** for all resources — IOLock wrappers, DMA buffers, etc.
- **`std::span`** for non-owning array views; no raw pointer arithmetic unless interfacing with C APIs.
- **`constexpr`/`static_assert`** for compile-time invariant checking — one wrong bit shift causes silent bus errors.
- **Cite specs in comments** (e.g. `// OHCI §7.2.3`, `// IEC 61883-6 §6.2`) — see *Ground truth per question type*. Never invent a section number.

## Swift App (ASFW/)

Uses Swift 6 strict concurrency. All cross-actor data must be `Sendable`. Use `actor` isolation correctly. The app is required to install DriverKit extensions via `systemextensionsctl`.
