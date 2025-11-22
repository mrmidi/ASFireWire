# AV/C Transaction Completion Analysis

**Status**: AV/C FCP commands timeout due to completion strategy mismatch  
**Date**: 2025-11-22  
**Context**: Block write transactions to FCP registers (`0xFFFFF0000B00`)

---

## Problem Statement

AV/C discovery fails with transaction timeouts when sending FCP commands (block writes to AV/C unit FCP command register). Transport layer is correct (packets sent/received properly on wire), but software completion logic rejects valid responses.

**Observable symptom** (tLabel=40 example):
```
01:42:55.134040 - Transaction state: ATPosted
01:42:55.134825 - AR response received: tCode=0x2 rCode=0x0 (SUCCESS)
01:42:55.134896 - ⚠️ Rejected: "Unexpected state ATPosted (expected AwaitingAR)"
01:42:55.336930 - ⏱️ Timeout fires (transaction never completed)
```

---

## Guiding principles (transport layer)

- **ACK is authoritative for unified vs split**; strategy is advisory and only encodes whether AR data is required (reads/locks) or optional (writes/PHY). ACK can always override the initial guess.
- **AR acceptance**: allow AR in `ATPosted`, `ATCompleted`, or `AwaitingAR` if the transaction is non-terminal and the generation/tLabel match; reject only if terminal or gen/tLabel mismatch.
- **RCode precedence**: reads/locks → AR is authoritative; writes → first completion path wins. If AT (ack_complete) wins and a later AR disagrees, log and ignore; if AR arrives first, let AR win.
- **Busy/timeouts**: `ack_busy_*` extends deadline/backoff; `ack_timeout` defers to the timeout path. Define and test retry/backoff policy (current default: +200ms).
- **FCP layering**: outgoing FCP write completion is transport-only; AV/C/FCP response is a separate incoming async write to the response register. Do not couple protocol timeouts to the outgoing write transaction.

---

---

## ASFW Architecture - Strategy Pattern Analysis

### Current Design Philosophy

ASFW implements a **compile-time strategy pattern** where completion behavior is determined at transaction creation time based on **tCode only**.

#### Three-Layer Architecture

**1. CompletionStrategy Enum** ([CompletionStrategy.hpp](file:///Users/mrmidi/DEV/FirWireDriver/ASFW/ASFWDriver/Async/Core/CompletionStrategy.hpp))

```cpp
enum class CompletionStrategy : uint8_t {
    CompleteOnAT = 0,     // Unified write, no AR needed
    CompleteOnPHY = 1,    // PHY packets
    CompleteOnAR = 2,     // Reads/locks, always need AR
    RequireBoth = 3       // Split transactions
};
```

Traits determine behavior:
```cpp
constexpr bool RequiresARResponse(CompletionStrategy strategy);
constexpr bool CompletesOnATAck(CompletionStrategy strategy);
```

**2. StrategyFromTCode()** - Static Decision Function (lines 170-192)

```cpp
constexpr CompletionStrategy StrategyFromTCode(uint8_t tCode, bool expectsDeferred = false) {
    switch (tCode) {
        case 0x4:  // Read quadlet
        case 0x5:  // Read block
        case 0x9:  // Lock
            return CompletionStrategy::CompleteOnAR;  // ✅ Correct - always need data

        case 0x0:  // Write quadlet
        case 0x1:  // Write block
            return expectsDeferred ? CompletionStrategy::RequireBoth
                                   : CompletionStrategy::CompleteOnAT;  // ❌ Wrong - can't know!

        default:
            return CompletionStrategy::CompleteOnAT;
    }
}
```

**Problem**: `expectsDeferred` is a **boolean hint**, not actual device behavior.

**3. TransactionCompletionHandler** - Runtime Execution ([TransactionCompletionHandler.hpp](file:///Users/mrmidi/DEV/FirWireDriver/ASFW/ASFWDriver/Async/Track/TransactionCompletionHandler.hpp))

Implements two callbacks:
- `OnATCompletion()` (lines 57-262): Handles AT descriptor completion
- `OnARResponse()` (lines 282-341): Handles AR response packet

### What ASFW Gets Right

**1. Clean separation of concerns**:
```cpp
// OnATCompletion processes ACK codes correctly
case 0x1:  // ack_pending
    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: ackPending");
    break;

case 0x0:  // ack_complete  
    postAction = PostAction::kCompleteSuccess;  // ← Correct!
    break;
```

**2. Proper read handling** (lines 119-124):
```cpp
if (txn->IsReadOperation() && txn->state() != TransactionState::AwaitingAR) {
    txn->TransitionTo(TransactionState::AwaitingAR, "OnATCompletion: read_legacy");
    return;  // Don't process ack code for reads
}
```

**3. Smart busy retry** (lines 183-194):
```cpp
case 0x4:  // ack_busy_*
    txn->SetDeadline(Engine::NowUs() + 200000);  // Extend deadline
    break;
```

### What ASFW Gets Wrong

#### Issue 1: Static Strategy Doesn't Match Runtime Reality

**Design assumption**:
```cpp
// At transaction creation (write block):
strategy = StrategyFromTCode(0x1, expectsDeferred: false);
// → CompleteOnAT
```

**Runtime reality** (your AV/C device):
```
AT completion: ack_complete (ACK=2) → says "no AR needed"
AR response:   tCode=0x2 arrives   → optional confirmation
```

Device behavior **doesn't match strategy**. Device sends AR even though strategy says `CompleteOnAT`.

#### Issue 2: AR Handler State Guard (lines 300-306)

```cpp
void OnARResponse(...) {
    if (txn->state() != TransactionState::AwaitingAR) {
        ASFW_LOG("⚠️ Unexpected state %s (expected AwaitingAR)", ToString(txn->state()));
        return;  // ❌ DISCARDS VALID RESPONSE
    }
}
```

**The race**:
1. Strategy = `CompleteOnAT` (based on tCode=0x1)
2. AT completes with `ack_complete` → should complete immediately (line 176-181)
3. But AT completion happens **after** AR arrival
4. AR arrives while still in `ATPosted` state → **rejected**
5. Timeout fires

#### Issue 3: No Completion Latch

Missing atomic guard against double-completion:
```cpp
// Needed but missing:
class Transaction {
    std::atomic<bool> completedByAR_{false};
    std::atomic<bool> completedByAT_{false};
};
```

If both AT and AR can complete (race condition), no protection exists.

### The Fundamental Flaw

**ASFW's assumption**:
> "We can determine completion path at creation time based on tCode"

**IEEE 1394 reality**:
> "Completion path is determined by ACK code, which arrives at runtime"

#### Why This Fails for Writes

Write transactions have **three possible outcomes**:

| ACK Code | Meaning | Completion Path | Known at Creation? |
|----------|---------|-----------------|-------------------|
| `ack_complete` | Unified write | Complete on AT immediately | ❌ No |
| `ack_pending` | Split transaction | Wait for AR response | ❌ No |
| `ack_busy_*` | Device busy | Retry after backoff | ❌ No |

**Cannot predict** which path device will choose **until ACK arrives**.

### Strategy Pattern vs ACK-Driven

| Aspect | ASFW Strategy Pattern | Linux/Apple ACK-Driven |
|--------|----------------------|----------------------|
| **Decision time** | Transaction creation (static) | ACK arrival (dynamic) |
| **Decision input** | tCode + hint | ACK code (actual device response) |
| **Write handling** | Assumes unified or split | Adapts to device choice |
| **AR acceptance** | Only in `AwaitingAR` state | Anytime before complete |
| **Correctness** | ❌ Breaks on optional responses | ✅ Handles all spec-legal behavior |

### Why Your Logs Show The Problem

```
01:42:55.134040 - State: ATPosted (strategy=CompleteOnAT from tCode)
01:42:55.134825 - AR response arrives (device sent optional confirmation)
01:42:55.134896 - ⚠️ Rejected: "Unexpected state ATPosted (expected AwaitingAR)"
01:42:55.336930 - ⏱️ Timeout fires
```

**What should happen**:
```
OnATCompletion(ack_complete):
  if (ackCode == 0x0) {
    CompleteTransaction(success);  // ← Immediate completion
  }

OnARResponse():  // Arrives later
  if (txn->isTerminal()) {
    return;  // Already completed, ignore
  }
```

**OR** (if AR beats AT):
```
OnARResponse():  // Arrives first
  if (!completedByAR_.exchange(true)) {
    CompleteTransaction(success);
  }

OnATCompletion(ack_complete):  // Arrives later
  if (completedByAR_.load()) {
    return;  // AR already won
  }
```

---

## Root Cause


### Apple's Strategy (IOFWAsyncCommand)

Apple uses **ACK-code-driven completion**, not static tCode-based completion.

From `IOFWAsyncCommand::gotAck()`:
```cpp
void IOFWAsyncCommand::gotAck(int ackCode)
{
    int rcode;
    setAckCode(ackCode);

    switch(ackCode) 
    {
        case kFWAckPending:
            return;  // Wait for AR packet (deferred/split transaction)
    
        case kFWAckComplete:
            rcode = kFWResponseComplete;
            break;   // Fall through to gotPacket() - COMPLETE IMMEDIATELY

        case kFWAckBusyX:
        case kFWAckBusyA:
        case kFWAckBusyB:
            return;  // Defer to timeout/retry logic
            
        case kFWAckTimeout:
            return;  // Defer to timeout
    
        default:
            rcode = kFWResponseTypeError;
    }

    gotPacket(rcode, NULL, 0);  // ← Completes transaction immediately
}
```

**Key insight**: For `ack_complete`, Apple **synthesizes** a response and completes the transaction without waiting for an AR packet.

### Current ASFW Strategy

Uses **static tCode-based completion strategy** set at transaction creation:

From `TransactionCompletionHandler.cpp`:
```cpp
CompletionStrategy StrategyFromTCode(uint32_t tCode, bool expectsDeferred) {
    switch (tCode) {
        case 0x0: // Quadlet read
        case 0x4: // Block read
        case 0x9: // Lock
            return RequireARResponse;
        
        case 0x1: // Block write
        case 0x5: // Quadlet write
            return expectsDeferred ? RequireBoth : CompleteOnAT;
        
        default:
            return RequireBoth;
    }
}
```

**Problem**: 
- Block write (tCode=0x1) defaults to `CompleteOnAT`
- But `CompleteOnAT` doesn't check ACK codes - assumes AT completion means "done"
- AR response handler **requires** state to be `AwaitingAR` before accepting packets
- Creates unbreakable race: AR arrives before state transitions

---

## Transport Behavior Observations

### IEEE 1394 Spec Compliance

Devices can respond to writes with:
1. **Link-layer ACK only** (ack_complete) - minimal response
2. **Link ACK + transport write response packet** (tCode=0x2) - verbose response
3. **Link ack_pending + transport response** - split transaction

**Your AV/C device sends BOTH**:
- Link ACK: `ack_complete` (ACK=2)
- Transport packet: Write response (tCode=0x2, rCode=0x0)

This is **legal** and **optional** per IEEE 1394-1995 §6.2.4.4.

### FireBug Trace Confirms

```
1. Block write request (tLabel=38): host → device, addr=0xFFFFF0000B00
2. Link ACK: ack_complete (immediate hardware acknowledgment)
3. Write response packet: tCode=0x2, rCode=0x0, tLabel=38 (optional transport confirmation)
4. FCP response: Separate block write device → host, addr=0xFFFFF0000D00 (protocol response)
```

Items 1-3 are **IEEE 1394 async transport**.  
Item 4 is **FCP protocol** (separate incoming write request).

---

## Completion Strategy Matrix

Correct completion logic requires **tCode × ACK code** decision matrix:

| tCode | Transaction Type | ack_complete | ack_pending | ack_busy/timeout |
|-------|-----------------|--------------|-------------|------------------|
| 0x1 | Block write | Complete on AT | Await AR | Retry on timeout |
| 0x5 | Quadlet write | Complete on AT | Await AR | Retry on timeout |
| 0x0 | Quadlet read | **Await AR** | Await AR | Retry on timeout |
| 0x4 | Block read | **Await AR** | Await AR | Retry on timeout |
| 0x9 | Lock/CAS | **Await AR** | Await AR | Retry on timeout |

**Critical difference**:
- **Writes** with `ack_complete` → Unified transaction, no AR needed
- **Writes** with `ack_pending` → Split transaction, AR required
- **Reads/Locks** → **Always** require AR (contains data/old value)

---

## Transaction FSM (transport)

```
New → ATPosted → (ATCompleted?) → AwaitingAR → Completed/Failed/TimedOut/Cancelled
                    ↘
                  Completed/Failed (AT path wins)
```

- Allowed AR states: `ATPosted`, `ATCompleted`, `AwaitingAR`; reject AR for terminal states or gen/tLabel mismatch.
- Allowed AT paths: `ack_complete` may finish immediately for writes; `ack_pending` forces `AwaitingAR`; reads/locks always transition to `AwaitingAR`.
- Debug builds should assert transition validity to catch future regressions.

---

## Current ASFW Implementation Issues

### Issue 1: AT Completion Doesn't Check ACK

`OnATCompletion()` should:
```cpp
if (ackCode == kFWAckComplete && tCode is write) {
    // Unified write - complete immediately
    CompleteTransaction(kIOReturnSuccess);
}
else if (ackCode == kFWAckPending) {
    // Split transaction - wait for AR
    TransitionTo(AwaitingAR);
}
```

Currently missing ACK-based branching.

### Issue 2: AR Handler Rejects Valid Responses

`OnARResponse()` requires state `== AwaitingAR`:
```cpp
if (txn->state() != TransactionState::AwaitingAR) {
    ASFW_LOG("Unexpected state %s (expected AwaitingAR)", ...);
    return;  // ← Discards valid response
}
```

Should accept AR in **any non-terminal state** if:
- Strategy requires AR, OR
- AR arrives before AT completion (device sent optional confirmation)

### Issue 3: No Completion Latch

Missing protection against double-completion when both AT and AR can complete:
```cpp
// Needed in Transaction class:
std::atomic<bool> completedByAR_{false};
std::atomic<bool> completedByAT_{false};

// In OnARResponse:
if (completedByAT_.exchange(true)) return;  // Already done

// In OnATCompletion:
if (strategy == CompleteOnAT && ackCode == ack_complete) {
    if (completedByAR_.exchange(true)) return;  // AR won the race
    CompleteTransaction();
}
```

---

## Wire-Level Evidence

### AT Descriptor Posted (tLabel=40)
```
[Async] Lock descriptor header copied: Q0=0x0000a110 Q1=0xffc2ffff 
        Q2=0xf0000b00 Q3=0x00080000
[Async] tCode=0x1 (block write) dest=0xFFC2 addr=0xFFFF:F0000B00 len=8
```

✅ Packet correctly formatted and sent.

### AR Response Received
```
[Async] AR/RSP NEW q0=0xFFC0A120 q1=0xFFC20000 → tCode=0x2, tLabel=40, rCode=0x0
[Async] 📥 OnARResponse: tLabel=40 nodeID=0xFFC2 gen=2 rcode=0x0 len=0
[Async] ⚠️ OnARResponse: Unexpected state ATPosted (expected AwaitingAR)
```

❌ Valid response discarded due to state guard.

### No AT Completion Interrupt Logged
```
[Controller] InterruptOccurred: intEvent=0x00700010
  → AR Request (0x00400000) + AR Response (0x00200000) + other (0x00100000)
  → NO reqTxComplete (0x00020000) or respTxComplete (0x00040000)
```

⚠️ AT completion interrupt either:
- Not fired by OHCI (hardware/ring issue)
- Fired but not logged (filtering issue)
- Processed before AR interrupt (ordering issue)

**Implication**: Relying solely on AT completion is brittle. AR fallback is essential.

---

## Fix Strategy

### Tier 1: Immediate Robustness (Required)

**A1. Relax AR state check**
```cpp
bool canAcceptAR = (txn->state() == AwaitingAR) ||
                   (txn->state() == ATPosted && !txn->isTerminal());
if (!canAcceptAR) return;
```

**A2. Add completion latch**
```cpp
class Transaction {
    std::atomic<bool> completedByAR_{false};
    std::atomic<bool> completedByAT_{false};
};

// OnARResponse:
if (completedByAT_.load() || completedByAR_.exchange(true)) return;

// OnATCompletion:
if (completedByAR_.load() || completedByAT_.exchange(true)) return;
```

### Tier 2: Correct Completion Logic

**B1. ACK-driven AT completion** (like Apple)
```cpp
void OnATCompletion(ackCode) {
    if (ackCode == ack_complete && isWriteTransaction()) {
        // Unified write - complete immediately
        if (!completedByAR_.exchange(true)) {
            CompleteTransaction(kIOReturnSuccess);
        }
    }
    else if (ackCode == ack_pending) {
        // Split transaction - transition to await AR
        TransitionTo(AwaitingAR);
    }
    else if (ackCode == ack_busy_* || ackCode == ack_timeout) {
        // Let timeout/retry handle it
        return;
    }
}
```

**B2. Strategy becomes ACK-responsive**
```cpp
// Initial strategy based on tCode (read vs write)
// Runtime adaptation based on ACK code
```

**B3. RCode merge policy**

- Reads/locks: AR is authoritative; AT path never finalizes status.
- Writes: first completion path wins. If AR arrives first, use AR rcode/status. If AT wins (ack_complete) and a later AR disagrees, log and ignore; treat disagreement as a warning-only anomaly.
- In debug builds, assert if `ack_complete` + AR error happens frequently (indicates device/link problems).

**B4. Busy/timeout policy**

- `ack_busy_*`: extend deadline/backoff (current default: +200ms) and retry policy remains at the upper layer.
- `ack_timeout`: let timeout handler fire; do not complete in AT path.
- Document the exact retry counts/backoff to keep tests deterministic.

### Tier 3: Architectural Cleanup

**C1. Decouple FCP from transport**

FCP response (item 4 in FireBug trace) should be handled as **incoming AR Request**, not tied to outgoing write transaction state:

```cpp
// Current (wrong):
// FCP command → blocking wait for FCP response via transaction state

// Correct:
// FCP command → transport completes on write ACK/response
// FCP response → arrives as AR Request write to 0xFFFFF0000D00
// FCP layer → matches response to pending command via tLabel/context
```

Already have the hooks:
```
[Async] RxPath AR/RQ: Async request packet (tCode=0x1, event=ack_pending) 
        - ignoring (not yet implemented)
```

Implement AR Request handler for local FCP response space.

**C2. FCP address map**

- Command register: `0xFFFFF0000B00` (host → device async write) → transport completion only.
- Response register: `0xFFFFF0000D00` (device → host async write) → handled as incoming async write; FCP layer matches by tLabel/cookie and runs AV/C state machine.
- Keep protocol timeouts/retries in FCP layer, independent of transport completion.

---

---

## Linux Implementation Validation

### Linux's Strategy (core-transaction.c)

Linux **also uses ACK-code-driven completion**, confirming this is the standard IEEE 1394 approach.

From `transmit_complete_callback()` (lines 150-189):
```c
static void transmit_complete_callback(struct fw_packet *packet,
                                       struct fw_card *card, int status)
{
    struct fw_transaction *t =
        container_of(packet, struct fw_transaction, packet);

    switch (status) {
    case ACK_COMPLETE:
        close_transaction(t, card, RCODE_COMPLETE, packet->timestamp);
        break;
    case ACK_PENDING:
    {
        t->split_timeout_cycle =
            compute_split_timeout_timestamp(card, packet->timestamp) & 0xffff;
        start_split_transaction_timeout(t, card);
        break;
    }
    case ACK_BUSY_X:
    case ACK_BUSY_A:
    case ACK_BUSY_B:
        close_transaction(t, card, RCODE_BUSY, packet->timestamp);
        break;
    case ACK_DATA_ERROR:
        close_transaction(t, card, RCODE_DATA_ERROR, packet->timestamp);
        break;
    case ACK_TYPE_ERROR:
        close_transaction(t, card, RCODE_TYPE_ERROR, packet->timestamp);
        break;
    default:
        close_transaction(t, card, status, packet->timestamp);
        break;
    }
}
```

**Key observations**:
1. **`ACK_COMPLETE`** → `close_transaction()` immediately with `RCODE_COMPLETE`
2. **`ACK_PENDING`** → Start split timeout timer, wait for response packet
3. **`ACK_BUSY_*`** → Close with RCODE_BUSY (retry at higher layer)
4. **No tCode checking** - completion is purely ACK-driven

### Linux vs Apple vs ASFW

| Implementation | Completion Trigger | ACK_COMPLETE | ACK_PENDING | tCode Dependency |
|----------------|-------------------|--------------|-------------|------------------|
| **Linux** | ACK code | Close immediately | Wait for packet | None |
| **Apple** | ACK code | `gotPacket(rcode, NULL, 0)` | Return (wait) | None |
| **ASFW** | tCode strategy | Stays in ATPosted ❌ | Partially handled | High ❌ |

**Unanimous verdict**: Both mature implementations (Linux kernel, Apple IOKit) use **pure ACK-driven completion**, not tCode-based strategies.

### Why Static tCode Strategy Fails

The ASFW approach of selecting strategy at transaction creation:
```cpp
// ASFW (incorrect)
CompletionStrategy StrategyFromTCode(uint32_t tCode, ...) {
    switch (tCode) {
        case 0x1: // Block write
            return CompleteOnAT;  // ← Assumes unified write
    }
}
```

Fails because:
1. **Write transactions are ambiguous** - can be unified OR split
2. **ACK code reveals the truth** - `ack_complete` = unified, `ack_pending` = split
3. **Can't know at creation time** - target device decides response type

### Linux's Split Transaction Handling

Linux uses timer-based split timeout (like Apple's deferred completion):
```c
case ACK_PENDING:
    // Compute when response is due
    t->split_timeout_cycle = compute_split_timeout_timestamp(...);
    // Start timer to fire if response doesn't arrive
    start_split_transaction_timeout(t, card);
    break;
```

This is equivalent to Apple's state transition to "wait for packet", but Linux uses timer infrastructure instead of explicit state enum.

---

## Comparison Summary


| Aspect | Apple IOFWAsyncCommand | Current ASFW |
|--------|----------------------|--------------|
| **Completion trigger** | ACK code (dynamic) | tCode (static) |
| **ack_complete handling** | Immediate completion | Ignored (stays in ATPosted) |
| **ack_pending handling** | Wait for AR packet | Partially handled |
| **AR response acceptance** | Anytime before completion | Only in AwaitingAR state |
| **Double-completion guard** | Implicit (completion check) | Missing |
| **FCP protocol** | AR Request handler | Not implemented |
| **Write response packets** | Accepted or ignored | Rejected (state guard) |

---

---

## Recommended Mitigation Strategy

### Hybrid Approach: ACK-Responsive Strategy Pattern

**Decision**: Preserve the strategy pattern architecture but make it **advisory not prescriptive**.

#### Why This Approach?

**Advantages of keeping strategy pattern**:
- ✅ Compile-time validation (reads always wait for AR)
- ✅ Clean separation of concerns
- ✅ Type-safe with C++20 concepts
- ✅ Explicit documentation of expected behavior

**What needs to change**:
- ❌ Strategy **suggests** initial path, ACK code **decides** final path
- ❌ AR handler accepts responses in any non-terminal state
- ❌ Add completion latch to prevent double-completion

### Implementation Plan

#### Phase 1: Immediate Fixes (Required for AV/C)

**A. Relax AR State Guard** - `TransactionCompletionHandler::OnARResponse()` (line 300)

```cpp
// BEFORE (wrong):
if (txn->state() != TransactionState::AwaitingAR) {
    ASFW_LOG("⚠️ Unexpected state");
    return;  // Reject
}

// AFTER (correct):
auto state = txn->state();
if (state == TransactionState::Completed ||
    state == TransactionState::Failed ||
    state == TransactionState::Cancelled ||
    state == TransactionState::TimedOut) {
    // Already terminal - AR is late/duplicate
    ASFW_LOG("AR response for terminal txn (state=%s), ignoring", ToString(state));
    return;
}

// Accept AR in ATPosted, ATCompleted, or AwaitingAR
ASFW_LOG("📥 AR response in state=%s, processing", ToString(state));
```

**Rationale**: AR can arrive before or after AT completion. Only reject if transaction already finished.

**B. Add Completion Latch** - `Transaction` class

```cpp
class Transaction {
private:
    std::atomic<bool> completedByPath_{false};  // Single atomic flag
    
public:
    // Returns true if this is the first completion
    bool TryMarkCompleted() {
        return !completedByPath_.exchange(true, std::memory_order_acq_rel);
    }
    
    bool IsCompleted() const {
        return completedByPath_.load(std::memory_order_acquire);
    }
};
```

Update completion sites:
```cpp
// OnATCompletion (line 178):
case 0x0:  // ack_complete
    if (txn->TryMarkCompleted()) {
        postAction = PostAction::kCompleteSuccess;
    }
    break;

// OnARResponse (before extraction):
if (!txn->TryMarkCompleted()) {
    ASFW_LOG("AR arrived but AT already completed, ignoring");
    return;
}
```

**Rationale**: Whichever path completes first "wins". Other path becomes no-op.

#### Phase 2: Strategy Semantics Update (Recommended)

**C. Rename Strategy Values** - `CompletionStrategy.hpp`

```cpp
enum class CompletionStrategy : uint8_t {
    // OLD NAMES (prescriptive):
    // CompleteOnAT, CompleteOnAR, RequireBoth
    
    // NEW NAMES (advisory):
    PreferATCompletion = 0,      // Writes: try AT first, accept AR
    RequireARResponse = 2,       // Reads/locks: always need AR
    AllowBothPaths = 3,          // Flexible: AT or AR can complete
    CompleteOnPHY = 1,           // Unchanged (no AR expected)
};
```

**D. Update Trait Functions**

```cpp
// Old (prescriptive):
constexpr bool CompletesOnATAck(CompletionStrategy strategy);

// New (advisory):
constexpr bool MayCompleteOnAT(CompletionStrategy strategy) {
    return strategy != CompletionStrategy::RequireARResponse;
}

constexpr bool MayCompleteOnAR(CompletionStrategy strategy) {
    return strategy != CompletionStrategy::CompleteOnPHY;
}

constexpr bool MustWaitForAR(CompletionStrategy strategy) {
    return strategy == CompletionStrategy::RequireARResponse;
}
```

**E. Update OnATCompletion Logic** (line 166-181)

```cpp
// Process ACK code (ACK is authoritative, not strategy)
switch (ackCode) {
    case 0x1:  // ack_pending
        // Device chose split transaction - MUST wait for AR
        ASFW_LOG("ackPending → force AwaitingAR (device chose split)");
        txn->TransitionTo(TransactionState::ATCompleted, "ackPending");
        txn->TransitionTo(TransactionState::AwaitingAR, "ackPending");
        break;

    case 0x0:  // ack_complete
        // Device chose unified transaction - MAY complete now
        if (strategy == CompletionStrategy::RequireARResponse) {
            // Read/lock - still need AR for data even with ack_complete
            ASFW_LOG("ackComplete but RequireARResponse → AwaitingAR");
            txn->TransitionTo(TransactionState::ATCompleted, "ackComplete_read");
            txn->TransitionTo(TransactionState::AwaitingAR, "ackComplete_read");
        } else {
            // Write - complete immediately
            ASFW_LOG("ackComplete → immediate completion");
            if (txn->TryMarkCompleted()) {
                postAction = PostAction::kCompleteSuccess;
            }
        }
        break;
        
    // ... busy/error cases unchanged
}
```

**Key change**: ACK code drives logic, strategy only used for read/write distinction.

#### Phase 3: Documentation Updates

**F. Update CompletionStrategy.hpp Comments**

```cpp
/**
 * @brief Completion strategy - ADVISORY completion path preference.
 *
 * IEEE 1394 transactions follow a two-phase protocol:
 * 1. AT (Transmit): ACK code indicates device's chosen path
 * 2. AR (Receive): Response packet (if device chose split or if data needed)
 *
 * CRITICAL: Strategy is ADVISORY - the actual completion path is determined
 * by the ACK code received at runtime. This enum provides:
 * - Compile-time validation (reads must wait for AR)
 * - Initial state setup
 * - Logging context
 *
 * But the final completion path is ALWAYS determined by:
 * - ACK code (ack_complete vs ack_pending)
 * - Transaction type (read vs write)
 *
 * Reference: Linux core-transaction.c transmit_complete_callback()
 *            Apple IOFWAsyncCommand::gotAck()
 */
```

### Migration Path

**Week 1**: Implement Phase 1 (A + B)
- Immediate fix for AV/C timeout issue
- Minimal code changes (~20 lines)
- Low risk (only relaxes guards)

**Week 2**: Implement Phase 2 (C + D + E)
- Semantic cleanup
- Aligns architecture with reality
- Moderate changes (~50 lines)

**Week 3**: Implement Phase 3 (F)
- Documentation clarity
- No code changes

### Alternative: Pure ACK-Driven (Not Recommended)

**Remove strategy pattern entirely**:
```cpp
// For every transaction:
OnATCompletion(ackCode) {
    switch (ackCode) {
        case ack_complete:
            if (isReadOperation()) wait_for_ar();
            else complete_now();
            break;
        case ack_pending:
            wait_for_ar();
            break;
    }
}
```

**Why not**:
- ❌ Loses compile-time validation
- ❌ Loses type safety (reads might forget to wait)
- ❌ Harder to audit for correctness
- ❌ Doesn't leverage C++20 features

**Hybrid is better**: Keep strategy for what it's good at (type safety), acknowledge ACK is authoritative.

---

## Next Steps


1. **Immediate**: Implement A1+A2 (relax AR check + completion latch)
2. **Short-term**: Implement B1 (ACK-driven AT completion logic)
3. **Medium-term**: Implement C1 (AR Request handler for FCP responses)
4. **Validation**: Add deterministic tests:
   - Write + `ack_complete`, no AR (AT wins).
   - Write + `ack_complete`, AR later success (winner-first, no double completion).
   - Write + `ack_pending`, AR success (AT→AwaitingAR → AR completes).
   - Write with AR arriving before AT (AR wins).
   - Write where AT wins success and AR later reports error (log-only, status unchanged).
   - Read/lock + `ack_complete` followed by AR (AR required).
   - Read/lock + `ack_pending` with missing AR (timeout path wins; late AR ignored).
   - Busy/timeout ACKs drive deadline/backoff per documented policy.

---

## References

- IEEE 1394-1995 §6.2.4.4: Write transaction response options
- IEEE 1394a-2000 §5.4.8.3: FCP (Function Control Protocol)
- Apple IOFireWireFamily: `IOFWAsyncCommand::gotAck()` completion logic
- FireBug bus trace: tLabel 38-40 (FCP command sequences)
