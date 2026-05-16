# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ASFW is a macOS DriverKit-based FireWire (IEEE 1394) driver restoring FireWire functionality removed in macOS Tahoe (26). It uses PCIDriverKit for user-space OHCI controller access and AudioDriverKit for CoreAudio integration.

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

**Version management:**
```bash
./bump.sh patch     # Bump patch version and regenerate DriverVersion.hpp
./bump.sh refresh   # Regenerate version header only
```

## Architecture

### ASFWDriver Subsystems

The driver is organized around these functional layers (all under `ASFWDriver/`):

| Directory | Responsibility |
|-----------|---------------|
| `Hardware/` | OHCI MMIO register layout, interrupt/event definitions |
| `Bus/` | Bus reset handling, Self-ID decode, topology, gap count optimization, generation tracking |
| `Async/` | Full async TX/RX pipeline: commands, DMA contexts (AT/AR), descriptor rings, label allocation, transaction tracking |
| `Isoch/` | Isochronous TX (working) and RX (WIP): OHCI DMA descriptors, AM824/CIP encoding, SYT timestamps, AudioDriverKit integration |
| `ConfigROM/` | Config ROM build, staging, reading/scanning from devices |
| `Discovery/` | FireWire device and unit enumeration (`FWDevice`, `FWUnit`) |
| `IRM/` | Isochronous Resource Manager: bandwidth and channel allocation |
| `Protocols/AVC/` | AV/C command layer: FCP transport, Music Subunit, stream formats, PCR space |
| `Controller/` | Controller state machine and lifecycle |
| `UserClient/` | DriverKit user-client interface (`.iig`), request handlers, wire serialization formats |
| `Shared/` | Shared rings, DMA memory manager, payload handles |
| `Common/` | `FWCommon.hpp`, barrier utilities |
| `Logging/` | Structured logging |

Key entry points:
- `ASFWDriver/ASFWDriver.iig` / `ASFWDriver.cpp` — driver class and `Start`/`Stop`
- `ASFWDriver/UserClient/Core/ASFWDriverUserClient.iig` — user-client interface
- `ASFWDriver/Isoch/Audio/ASFWAudioDriver.iig` — AudioDriverKit engine

### Isochronous Audio Pipeline

```
CoreAudio → AudioRingBuffer → PacketAssembler → IsochTransmitContext → OHCI IT DMA → FireWire Bus
FireWire Bus → OHCI IR DMA → IsochReceiveContext → StreamProcessor → AM824Decoder → CoreAudio
```

Audio device publication: `AVCDiscovery` creates `ASFWAudioNub` with discovered capabilities; `ASFWAudioDriver` matches on the nub and registers `IOUserAudioDevice` with CoreAudio HAL.

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

### Reference Material (internal, not public)

- `docs/linux/` — Linux `firewire-ohci` driver (authoritative for descriptor layout)
- `docs/IOFireWireFamily/` — Apple's original FireWire kext source
- `docs/IOFireWireAVC/` — Apple's AV/C protocol implementation
- `docs/ohci/` — OHCI specification

## Critical Rules and Gotchas

**Endianness:** OHCI descriptor headers are little-endian; IEEE 1394 wire payloads are big-endian. Use `ToBusOrder`/`FromBusOrder` (defined in `FWCommon.hpp`) explicitly. Never assume.

**IT descriptor layout:** The `OUTPUT_MORE_IMMEDIATE` skip address lives at offset `0x08` (Branch Word), **not** `0x04`, despite some OHCI 1.1 diagrams. Follow Linux `firewire-ohci` + Apple validated behavior. See `ASFWDriver/Isoch/README.md`.

**Constants:** All OHCI hardware register constants go in `ASFWDriver/Hardware/OHCIConstants.hpp` (single source of truth). Never define them in `.cpp` files or class headers.

**OHCI timing:** Context stop/quiesce requires polling with timeout and escalating delays (5µs → 255µs). Do not assume immediate hardware response.

**DMA coherency:** Call `OSSynchronizeIO`/`IoBarrier` after writing descriptors before waking hardware. Read descriptor status fields before acting on completion data.

**IIG files:** `.iig` interface files require Xcode's IIG preprocessor to generate `.iig.cpp`. CMake builds exclude these; production builds must use Xcode.

**IIG subclassing rules:**
- `Create()` and `init()` are **not inherited** — each subclass must declare its own as `LOCALONLY`.
- `IMPL(ClassName, Method)` is only for methods **defined in that class's own `.iig`** — it generates `ClassName_Method_Args` which IIG only emits for the defining class.
- **Overriding a parent's method** uses plain C++ (`kern_return_t ClassName::Method(...)`), never `IMPL`.
- **No `SUPERDISPATCH`** in child overrides — call the equivalent setter directly (e.g. `SetSampleRate()`, `SetControlValue()`) to confirm the change to the HAL.
- **No `virtual` keyword** in override declarations in `.iig` — use `kern_return_t Foo() override;` not `virtual kern_return_t Foo() override;`.
- **`init()` first param type matters** — check the SDK header: `IOUserAudioDevice::init()` takes `IOUserAudioDriver*`, not `IOService*`. Wrong type → compile error in `super::init()` call.
- **Forward declarations are not enough** — if `ControllerCore.hpp` only forward-declares `IAVCDiscovery`, calling any method on it requires the full `#include "Protocols/AVC/IAVCDiscovery.hpp"` in the `.cpp`.

**Test isolation:** All C++ tests compile with `ASFW_HOST_TEST` defined, which stubs out DriverKit APIs. Logic tested this way cannot cover actual hardware interaction.

## Code Patterns

- **Error handling:** `std::expected<T, E>` — no exceptions in driver code. Mark all error-returning functions `[[nodiscard]]`.
- **CRTP** for compile-time context role enforcement (AT Request vs AT Response, etc.).
- **RAII** for all resources — IOLock wrappers, DMA buffers, etc.
- **`std::span`** for non-owning array views; no raw pointer arithmetic unless interfacing with C APIs.
- **`constexpr`/`static_assert`** for compile-time invariant checking — one wrong bit shift causes silent bus errors.
- Reference OHCI spec sections in comments, e.g., `// OHCI §7.2.3`.

## Swift App (ASFW/)

Uses Swift 6 strict concurrency. All cross-actor data must be `Sendable`. Use `actor` isolation correctly. The app is required to install DriverKit extensions via `systemextensionsctl`.

## Development Environment (macOS Sequoia, no Tahoe)

Tests run on macOS Sequoia — no Tahoe machine available. Hardware testing requires a separate setup (Tahoe + TB3→FW adapter + MOTU device).

**cmake** is installed via Homebrew and NOT on the default PATH. Always prefix or export:
```bash
export PATH="/opt/homebrew/bin:$PATH"
cmake -S tests -B build/tests_build
cmake --build build/tests_build -- -j$(sysctl -n hw.ncpu)
ctest --test-dir build/tests_build
```

**Running a single test suite:**
```bash
ctest --test-dir build/tests_build -V -R IsochRxDmaRing
```

## CodeGraph MCP

The project is indexed with CodeGraph (local SQLite graph of all symbols). Index lives in `.codegraph/codegraph.db`. The MCP server is configured in `../.mcp.json` (one level above ASFireWire, in the FireWire project root).

## ⛔ BEZWZGLĘDNY ZAKAZ: grep / find / Bash do szukania kodu

**NIGDY nie używaj `grep`, `find`, ani Bash do eksploracji kodu.** To jest bezwzględna reguła bez wyjątków.

Jedyne dozwolone narzędzia do lokalizacji symboli i plików:
- `codegraph_search` — znajdź klasy, metody, pliki po nazwie
- `codegraph_context` — kontekst dla zadania (entry points + related symbols)
- `codegraph_callers` — kto wywołuje dany symbol
- `codegraph_callees` — co wywołuje dany symbol
- `codegraph_node` — pełny kod konkretnego węzła
- `codegraph_impact` — co zostanie dotknięte zmianą symbolu

`grep`/`find`/Bash są dopuszczalne **wyłącznie** gdy CodeGraph nie zwróci wyniku po 2 próbach, i tylko z wyraźną adnotacją `# fallback: CodeGraph nie znalazł`.

**ZAWSZE CodeGraph jako pierwszy krok.** Czytanie pliku bez wcześniejszego `codegraph_search` / `codegraph_context` = błąd procesu.

**WAŻNE — zawsze przekazuj `projectPath`:** MCP serwer jest skonfigurowany w `../.mcp.json` (katalog `FireWire/`), więc jego CWD to `FireWire/`, nie `ASFireWire/`. Bez explicit `projectPath` CodeGraph szuka bazy w złym katalogu i zwraca "not initialized". Każde wywołanie musi mieć:
```
projectPath: "/Users/cube666/Documents/FireWire/ASFireWire"
```

**Re-index after adding/moving files:**
```bash
export PATH="$HOME/.npm-global/bin:/opt/homebrew/opt/node@22/bin:$PATH"
NODE_OPTIONS="--max-old-space-size=4096" codegraph index -f -q .
```

**Current index stats:** 603 files · 11 209 nodes · 20 570 edges (497 C++, 66 Swift).


Wytyczne Behawioralne
Kompromis: Niniejsze wytyczne stawiają ostrożność i precyzję ponad szybkość. Przy trywialnych zadaniach — kieruj się własnym osądem.

1. Pomyśl, zanim zaczniesz kodować

Nie zakładaj. Nie ukrywaj dezorientacji. Przed implementacją: - Jasno określ założenia. Jeśli masz wątpliwości — zapytaj. - Jeśli istnieje wiele interpretacji — przedstaw je, nie dokonuj wyboru po cichu. - Jeśli istnieje prostsze podejście — powiedz o tym. Sprzeciwiaj się, gdy jest to uzasadnione. - Jeśli coś jest niejasne — zatrzymaj się. Nazwij to. Zapytaj.

2. Prostota przede wszystkim

Minimalna ilość kodu, która rozwiązuje problem. Żadnych spekulacji: - Brak funkcji wykraczających poza to, o co proszono - Brak abstrakcji dla kodu jednorazowego użytku - Żadnej „elastyczności" ani „konfigurowalności", o którą nie proszono - Żadnej obsługi błędów dla niemożliwych scenariuszy - Jeśli napisałeś 200 linii, a wystarczyłoby 50 — napisz od nowa

Zadaj sobie pytanie: „Czy doświadczony inżynier (senior) uznałby to za zbyt skomplikowane?" — jeśli tak, uprość.

3. Zmiany chirurgiczne

Dotykaj tylko tego, co musisz: - Nie „poprawiaj" sąsiedniego kodu bez pytania - Dopasuj się do istniejącego stylu - Niepowiązany martwy kod: wspomnij — nie usuwaj

Gdy Twoje zmiany tworzą „osierocone" elementy: - Usuń importy/zmienne/funkcje, które stały się nieużywane przez Twoje zmiany - Nie usuwaj sam wcześniej istniejącego martwego kodu (Poinformuj o nim wyraźnie), usuń wtedy gdy zostaniesz o to poproszony


## Test Stub Quirk — DMA Alignment

`HardwareInterface::AllocateDMA` in `tests/HardwareInterfaceStub.cpp` allocates the virtual buffer with **at least 4096-byte (page) alignment** (`effectiveAlign = max(4096, requested)`). This is intentional: the mock IOVA counter starts at `0x20000000` (page-aligned), so `DMAMemoryManager::AlignCursorToIOVA(4096)` only produces correct virtual-address alignment if `slabVirt_` is also page-aligned. Without this, the IOVA cursor aligns but the VA does not, breaking `IsochDMAMemoryManagerTest.PayloadSlicingAndPageAlignment`.
