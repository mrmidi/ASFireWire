# SBP-2 Session/Command Port — Component Breakdown & DICE-API Adaptation Map

**Linear:** FW-54 (epic) → FW-56 (this step). **Branch:** `sbp2-session-port`.
**Source of truth for behavior:** PR #19 (`SBP2LoginSession.cpp` 1847 lines, `SBP2SessionRegistry.cpp` 799 lines) + its four test files.
**Source of truth for idiom:** DICE (`Protocols/SBP2/*` foundation, already hardened — see FW-55).

This is a **re-implementation**, not a merge. #19's monoliths are decomposed the way DICE split `ConfigROM` (Builder/Stager/Reader/Scanner/Parser/Store). The session layer was written against #19's ORB API, which differs from DICE's — §3 maps every call.

**Object model (decided):** components are plain modern-C++ classes (POCO), matching DICE's existing POCO foundation (`SBP2CommandORB`/`SBP2ManagementORB`/`AddressSpaceManager` are all plain classes, host-tested under `ASFW_HOST_TEST`). They are **not** IIG/`IOService` objects — that would break the gtest conformance oracle and diverge from the foundation. The only DriverKit-native surface is a single thin driver-level `IOTimerDispatchSource`+`OSAction` owner, injected into the POCO components as `ISessionScheduler` (§7a). "Modernize" is honored via C++ idioms + native primitives *behind* the interfaces, not by IOService-ifying every object.

---

## 1. `SBP2LoginSession.cpp` (1847 lines, 60 methods) → 2 components

New directory: `Protocols/SBP2/Session/`.

> **Decomposition correction (from reading the code):** an earlier draft proposed
> 4 components (separate `LoginOrbExchange` + `UnsolicitedStatusSink`).
> `OnStatusBlockRemoteWrite` switches on `LoginState` and routes directly into
> login/reconnect/logout completion — all sharing `state_`, `loginGeneration_`,
> and the timers. Splitting those out scatters shared mutable state across class
> boundaries (worse, not better). The one genuinely clean seam is the post-login
> **command plane** (`FetchAgent`). So the login side is **2 cohesive components**.

### 1a. `LoginSession` — login lifecycle state machine (management plane)
Owns `LoginState`, login/reconnect IDs, target info, the login/reconnect/logout/
status-FIFO address ranges, and status routing. Cohesive: every item below
reads/writes `state_` + `loginGeneration_` + the timers.

| Methods folded in |
|---|
| `Configure`, `Login`, `Logout`, `Reconnect`, `HandleBusReset`, `SetState` |
| ranges: `Allocate{Login,LoginResponse,StatusBlock,Reconnect,Logout}…`, `Allocate`/`DeallocateResources` |
| `BuildLoginORB`, `BuildReconnectORB`, `BuildLogoutORB`, `RefreshCommandBlockAgentAddresses` |
| `On{Login,Reconnect,Logout}WriteComplete`, `On{Login,Reconnect,Logout}Timeout` |
| `OnStatusBlockRemoteWrite`, `ProcessStatusBlock`, `Complete{Login,Reconnect,Logout}FromStatusBlock` |
| `EnableUnsolicitedStatus` + `OnUnsolicitedStatusEnableComplete`, `WriteBusyTimeout` + `OnBusyTimeoutComplete` |
| accessors: `CommandBlockAgent`, `ReconnectHoldSeconds`, `TargetInfo`, `MaxPayloadSize`, `State`, `LoginID`, `Generation` |
| timers via injected `ISessionScheduler` (§7a); **`SetTimeoutQueue` + owned-queue machinery deleted** (§4) |

Owns the status-FIFO range (read by both login completion *and* unsolicited/command
status — both are `LoginSession` transitions). Its remote-write callback captures
`weak_from_this()`. Backing is an `IOBufferMemoryDescriptor` via
`AddressSpaceManager`, never a raw buffer (§7).

### 1b. `FetchAgent` — command plane (post-login ORB submission)
Doorbell, ORB chaining, fetch-agent reset. The engine `CommandExecutor` (§2b)
drives. Self-contained: depends only on `SBP2CommandORB` (✓ step 1), the bus, and
the scheduler — `LoginSession` hands it the command-block-agent address +
node/generation once login succeeds.

| Methods folded in |
|---|
| `SubmitORB`, `AppendORB`, `AppendORBImmediate`, `RingDoorbell` |
| `MakeORBKey` (×2), `ClearORBTracking`, `outstandingORBs_` / `chainTailORB_` state |
| `StartSubmittedORBTimer`, `FailSubmittedORB`, `FailPendingImmediateORBs` |
| `OnFetchAgentWriteComplete`, `OnDoorbellComplete`, `ResetFetchAgent`, `OnAgentResetComplete` |
| `SubmitManagementORB` (drives an `SBP2ManagementORB` for task-mgmt) |

---

## 2. `SBP2SessionRegistry.cpp` (799 lines, 30 methods) → registry + executor + slim record

### 2a. `SessionRegistry` — identity & lifecycle only (no command state)
| Methods |
|---|
| `CreateSession`, `StartLogin`, `GetSessionState`, `ReleaseSession`, `ReleaseOwner` |
| `OnBusReset`, `RefreshTargets` |
| `FindByHandle` (×2), `FindByHandleForOwner` (×2), `ResolveUnit` |
| `HasSessionForTargetLocked` (dup-target reject, `afcbd9f`), `RetireSessionLocked`, `EraseRetiredSessionLocked`, `SetReleaseLogoutCallbackLocked` |
| testing seams: `GetSessionForTesting`, `GetSessionWeakForTesting` |

Hardening to preserve: owner validation (`8b64806`), release order = sessions **before** address ranges (`9ca0d8e`).

### 2b. `CommandExecutor` — command plane (lifts the god-object out of the record)
Owns everything command-related currently bloating `SBP2SessionRecord`.

| Methods | Owned state (moved out of record) |
|---|---|
| `SubmitInquiry`, `GetInquiryResult` | `commandORB` (`unique_ptr<SBP2CommandORB>`) |
| `SubmitCommand`, `GetCommandResult` | `commandPageTable` (`unique_ptr<SBP2PageTable>`) |
| `SubmitTaskManagement`, `IsSupportedTaskManagementFunction` | `managementORB` (`unique_ptr<SBP2ManagementORB>`) |
| `FailActiveCommandLocked` (`f8b0403`, `45a5609`) | `activeCommandRequest`, `pendingCommandResult`, `commandReady`, `commandInFlight`, `commandBufferHandle` |
| `CleanupCommandResources`, `CleanupManagementResources`, `BuildCommandFlags` | |

### 2c. `SessionRecord` — slim value type
`{ handle, owner, guid, romOffset, shared_ptr<LoginSession> session, SBP2SessionState state }`. Command guts → `CommandExecutor` (keyed by handle).

---

## 3. DICE-API adaptation map (every #19 ORB call → DICE)

### `SBP2CommandORB`
| #19 call (in session layer) | #19 API | DICE API | Action |
|---|---|---|---|
| `orb->SetCommandBlock(cdb)` *(return checked)* | `[[nodiscard]] bool` (rejects oversized CDB) | `void` | **Add `[[nodiscard]] bool SetCommandBlock`** to DICE — real hardening (`1d2ac01`), bounds-check CDB ≤ `maxCommandBlockSize_`. |
| `orb->IsValid()` | `bool` (`orbHandle_!=0`) | **absent** | **Add `IsValid()`** to DICE. DICE ctor calls `AllocateResources()` but swallows the bool → failed alloc is currently undetectable. Required. |
| `orb->PrepareForExecution(node, speed, payloadLog)` *(return checked)* | `[[nodiscard]] kern_return_t` | `void` (same 3 args) | **Change DICE return to `kern_return_t`**; propagate alloc/write failure (`1d2ac01`). Args already match. |
| `chainTailORB_->SetNextORBAddress(hi, lo)` *(return checked)* | `[[nodiscard]] kern_return_t` | `void` | **Change DICE return to `kern_return_t`** (fails if ORB not allocated). |
| `orb->SetToDummy()` | `[[nodiscard]] kern_return_t` | `void` | Change DICE return to `kern_return_t` for symmetry (used in fetch-agent chain teardown). |
| `orb->StartTimer(workQueue_, timeoutQueue)` | 2 queues | `StartTimer(queue)` 1 queue | **Adapt call site** → `StartTimer(workQueue_)`. Single-queue model (§4). |
| `orb->SetMaxPayloadSize(...)` | dropped in #19 | present in DICE | keep DICE; executor may set it. No change. |
| `SetFlags`, `SetTimeout`, `SetCompletionCallback`, `SetDataDescriptor`, `GetORBAddress`, `IsAppended`, `GetFlags`, fetch-agent retry getters | identical | identical | no change |

### `SBP2ManagementORB`
| #19 call | DICE | Action |
|---|---|---|
| `orb->SetTimeoutQueue(q)` | **absent** | **Drop the call.** DICE arms timeout on `workQueue_` after the write ACK (already correct, see FW-55). |
| `SetWorkQueue`, `SetTimeout`, `SetCompletionCallback`, `SetFunction`, `SetLoginID`, `SetTargetORBAddress`, `SetManagementAgentOffset`, `SetTargetNode`, `Execute`, `GetFunction`, `InProgress` | present | no change |

### `LoginSession` internal (our new component — we define the API)
| #19 surface | Action |
|---|---|
| `SetTimeoutQueue`, `EnsureTimeoutQueue`, `ReleaseOwnedTimeoutQueue`, `EffectiveTimeoutQueue` | **Delete.** Single-queue model collapses owned-timeout-queue machinery (§4). |
| `SubmitDelayedCallback(delayMs, …)` | Re-express on `SBP2DelayedDispatch::DispatchAfterCompat` (what DICE's ManagementORB already uses). |

---

## 4. Single-Default-queue simplification

Per the dext's single-`Default`-queue confinement, #19's separate **timeout queue** abstraction is dead weight. DICE's `SBP2ManagementORB` already proves the pattern: arm a delayed callback on `workQueue_` guarded by a `timerGeneration_` counter + `lifetimeToken_` weak_ptr. The whole `*TimeoutQueue*` family (`SetTimeoutQueue`/`EnsureTimeoutQueue`/`ReleaseOwnedTimeoutQueue`/`EffectiveTimeoutQueue` + the owned `IODispatchQueue`) is **removed**, not ported. ~80 lines of #19 evaporate.

Lifetime safety for async callbacks: each component is `enable_shared_from_this`; callbacks capture `weak_from_this()` and bail on `expired()` (the `2b0ddca` guard), matching DICE's `lifetimeToken_` idiom.

---

## 5. Foundation additions FW-56 needs (small, in DICE's CommandORB)

These are the only foundation touches; they are genuine hardening, not signature taste:
1. `[[nodiscard]] bool IsValid() const noexcept { return orbHandle_ != 0; }`
2. `[[nodiscard]] bool SetCommandBlock(std::span<const uint8_t>)` — bounds-check vs `maxCommandBlockSize_`.
3. `PrepareForExecution` / `SetNextORBAddress` / `SetToDummy` → return `kern_return_t`.

Each gets a unit test in `tests/protocols/SBP2ORBTests.cpp` (which already exists on DICE).

---

## 6. Build/test order within FW-56 (each green before next)

Revised to a dependency-correct **bottom-up** order (the registry constructs a
`LoginSession`, so it cannot precede it):

0. ✅ `ISessionScheduler` interface + virtual-clock fake (§7a). Production timer wiring (IOTimerDispatchSource+OSAction) lands in FW-58.
1. ✅ CommandORB foundation additions (§5) → `SBP2ORBTests` green.
2. `FetchAgent` (command plane; needs CommandORB ✓ + scheduler ✓ + bus) + focused tests.
3. `LoginSession` (state machine + status routing; uses scheduler; drives FetchAgent) → port `SBP2LoginSessionTests`.
4. `SessionRecord` slim type + `SessionRegistry` (identity/lifecycle; constructs `LoginSession`) → port registry half of `SBP2SessionRegistryTests`.
5. `CommandExecutor` (drives FetchAgent; owns command ORB/page-table/inflight) → command half of `SBP2SessionRegistryTests`.
6. `tests/CMakeLists` targets registered; full host suite green; checkpoint commit.

> Tests come from #19 but were written against #19's API — adapt assertions to the decomposed components and DICE call shapes as each piece lands.

---

## 7. DriverKit primitives — use native, not hand-rolled

Guidance (approved): lean on DriverKit's own facilities rather than reinventing them.

### 7a. Timers → `IOTimerDispatchSource` + `OSAction` (not the IOSleep hack)

The current `SBP2DelayedDispatch::DispatchAfterCompat` schedules a timeout as
`queue->DispatchAsync(^{ IOSleep(delayMs); work(); })`. That **blocks the queue
thread for the whole delay** — on the dext's single `Default` queue, every armed
timeout stalls the queue. Do **not** propagate this into the four new components.

The dext already has the correct idiom: `Scheduling/WatchdogCoordinator` uses
`IOTimerDispatchSource::Create(queue, &timer)` driven by an `OSAction`
(`AsyncWatchdogTimerFired`, `TYPE()`-declared in `ASFWDriver.iig`). Follow it.

**Constraint:** an `OSAction` target must be an IIG `TYPE()`-declared method on a
DriverKit class. The SBP-2 session components are plain C++ (POCO), not
`IOService`s, so they cannot host `OSAction` callbacks directly. Therefore:

- Components depend on an **injected scheduler interface** — e.g.
  `ISessionScheduler { CancelableToken ScheduleAfter(uint64_t ns, fn); void Cancel(token); }`
  — never on `DispatchAfterCompat` directly.
- **Production** backing: an `IOTimerDispatchSource`+`OSAction` owned at the
  driver/controller level, routing fires back into the component (wired in FW-58).
  The generation-counter + `weak_from_this()` guards still apply per fire.
- **Host tests**: a fake scheduler with a manually-advanced virtual clock — makes
  the timeout/reconnect/busy-replay paths deterministically testable (no real time).

This keeps the POCO components testable *and* gets real, non-blocking DriverKit
timers in production. The `SBP2DelayedDispatch` shim stays only as the host-test
fallback path if convenient; production goes through the scheduler interface.

### 7b. Memory → `IOBufferMemoryDescriptor` via `AddressSpaceManager`

Every SBP-2 address range (login/response/status-FIFO/reconnect/logout ORBs,
command ORB, page table) is allocated through `AddressSpaceManager`, which already
backs each range with `OSSharedPtr<IOBufferMemoryDescriptor>` +
`OSSharedPtr<IODMACommand>` + `CreateMapping`. Components hold **handles**, not
buffers; they never allocate device-visible memory by hand. `OSSharedPtr` /
`OSAction` lifetimes are RAII-managed.

### 7c. Async transactions → existing bus async API

Login/reconnect/logout writes, fetch-agent writes, and doorbell rings go through
the established `Async::IFireWireBus` async write API (as #19 does), whose
completions are already `OSAction`-driven inside `AsyncSubsystem`. No new
transport primitive is introduced at the SBP-2 layer.
