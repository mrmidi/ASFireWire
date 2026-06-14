# SBP-2 Session/Command Port — Component Breakdown & DICE-API Adaptation Map

**Linear:** FW-54 (epic) → FW-56 (this step). **Branch:** `sbp2-session-port`.
**Source of truth for behavior:** PR #19 (`SBP2LoginSession.cpp` 1847 lines, `SBP2SessionRegistry.cpp` 799 lines) + its four test files.
**Source of truth for idiom:** DICE (`Protocols/SBP2/*` foundation, already hardened — see FW-55).

This is a **re-implementation**, not a merge. #19's monoliths are decomposed the way DICE split `ConfigROM` (Builder/Stager/Reader/Scanner/Parser/Store). The session layer was written against #19's ORB API, which differs from DICE's — §3 maps every call.

---

## 1. `SBP2LoginSession.cpp` (1847 lines, 60 methods) → 4 components

New directory: `Protocols/SBP2/Session/`.

### 1a. `LoginSession` — orchestrator + state machine
Owns `LoginState`, login/reconnect IDs, target info, and the three sub-components. Pure coordination; no wire I/O of its own.

| Method (from #19) | Notes |
|---|---|
| `Configure`, `Login`, `Logout`, `Reconnect`, `HandleBusReset` | public entry points |
| `SetState`, `CommandBlockAgent`, `ReconnectHoldSeconds`, `TargetInfo` | state/accessors |
| `SetWorkQueue` | keep; **`SetTimeoutQueue` dropped** (see §4) |

### 1b. `LoginOrbExchange` — management plane
The login/reconnect/logout management ORBs and the status-block that completes them. Produces a parsed `Wire::LoginResponse`.

| Methods folded in |
|---|
| `AllocateLoginORBAddressSpace`, `AllocateLoginResponseAddressSpace`, `AllocateReconnectORBAddressSpace`, `AllocateLogoutORBAddressSpace`, `AllocateResources`/`DeallocateResources` (its share) |
| `BuildLoginORB`, `BuildReconnectORB`, `BuildLogoutORB` |
| `OnLoginWriteComplete`, `OnReconnectWriteComplete`, `OnLogoutWriteComplete` |
| `OnLoginTimeout`, `OnReconnectTimeout`, `OnLogoutTimeout` |
| `CompleteLoginFromStatusBlock`, `CompleteReconnectFromStatusBlock`, `CompleteLogoutFromStatusBlock`, `ProcessStatusBlock` |
| timers: `StartLoginTimer`/`StartReconnectTimer`/`StartLogoutTimer`/`CancelLoginTimer` → collapse onto DICE `SBP2DelayedDispatch` (§4) |

### 1c. `FetchAgent` — command plane (post-login ORB submission)
Doorbell, ORB chaining, fetch-agent reset. This is the engine the `CommandExecutor` (§2b) drives.

| Methods folded in |
|---|
| `SubmitORB`, `AppendORB`, `AppendORBImmediate`, `RingDoorbell` |
| `MakeORBKey` (×2), `ClearORBTracking`, `outstandingORBs_` / `chainTailORB_` state |
| `StartSubmittedORBTimer`, `FailSubmittedORB`, `FailPendingImmediateORBs` |
| `OnFetchAgentWriteComplete`, `OnDoorbellComplete` |
| `ResetFetchAgent`, `OnAgentResetComplete` |
| `SubmitManagementORB` (drives a `SBP2ManagementORB` for task-mgmt) |

### 1d. `UnsolicitedStatusSink` — status FIFO + busy timeout
Owns the status-FIFO address range and its remote-write callback (lifetime-guarded via the weak-ownership pattern; the callback captures `weak_from_this()`). Routes parsed status blocks back to `LoginOrbExchange` (login completion) and `FetchAgent` (command completion).

| Methods folded in |
|---|
| `AllocateStatusBlockAddressSpace`, `OnStatusBlockRemoteWrite` (dispatch/route) |
| `EnableUnsolicitedStatus`, `OnUnsolicitedStatusEnableComplete` |
| `WriteBusyTimeout`, `OnBusyTimeoutComplete` |

> **Open design point:** the status FIFO is shared between login completion and unsolicited/command status. Decision: `UnsolicitedStatusSink` *owns* the range and exposes a routing callback; `LoginOrbExchange` registers a "waiting for login status" handler with it rather than owning a second range. Confirm during impl.

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

1. CommandORB foundation additions (§5) + tests → `SBP2ORBTests` green.
2. `SessionRecord` slim type + `SessionRegistry` (identity/lifecycle) → port `SBP2SessionRegistryTests` (registry half).
3. `LoginSession` core + `LoginOrbExchange` → port `SBP2LoginSessionTests`.
4. `FetchAgent` + `UnsolicitedStatusSink`.
5. `CommandExecutor` → remaining `SBP2SessionRegistryTests` (command half).
6. `tests/CMakeLists` targets registered; full host suite green; checkpoint commit.

> Tests come from #19 but were written against #19's API — adapt assertions to the decomposed components and DICE call shapes as each piece lands.
