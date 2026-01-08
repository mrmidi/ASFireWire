# ACK-Driven Completion Refactor (Async Transport)

**Scope:** Async transaction completion for AT/AR paths (AV/C FCP writes surfaced the bug).  
**Problem:** Static tCode-based completion strategy rejected valid AR responses and missed ack-driven behavior, causing timeouts.  
**Status:** Tests in `CompletionRefactorPlanTests` cover the new behavior.

## Background

IEEE 1394 async writes can complete in two ways, chosen at runtime by the target:
- `ack_complete` → unified transaction (no AR required)
- `ack_pending` → split transaction (AR required)

ASFW previously picked a completion path at transaction creation based on tCode (e.g., block write → CompleteOnAT). When a target sent an optional AR confirmation for a write, `OnARResponse` rejected it because the state had not been moved to `AwaitingAR`, leading to timeouts.

## Fix Summary

1) **ACK is authoritative**
- `ack_complete` completes immediately only for write-like ops.
- Reads/locks (or `CompleteOnAR` strategy) still transition to `AwaitingAR` even with `ack_complete`.
- `ack_pending` always forces `AwaitingAR`.

2) **Race-safe AR acceptance**
- AR is accepted in any non-terminal state (`ATPosted`, `ATCompleted`, `AwaitingAR`); only terminal states ignore.
- Atomic latch (`TryMarkCompleted`) prevents double completion (AT vs AR).

3) **Logging**
- Uses `ASFW_LOG_V*` (category `Async`): V1 for completions/timeouts, V2 for transitions/ack paths, V3 for race notes.

4) **Host testability**
- DriverKit stubs supply `IOLock`/mach time/byte-swap on host.
- New gtests: `CompletionRefactorPlanTests` cover ack_complete win, ack_pending → AR, AR-before-AT race, read requires AR, busy deadline extension.

## Behavioral Matrix (post-fix)

| tCode | ACK | Action |
| --- | --- | --- |
| Write (0x0/0x1) | `ack_complete` | Complete on AT unless strategy demands AR |
| Write (0x0/0x1) | `ack_pending` | Transition to `AwaitingAR`; AR completes |
| Read/Lock (0x4/0x5/0x9) | any ACK | Always require AR (data/old value) |
| Busy (`0x4-0x6`) | N/A | Extend deadline (+200ms) for retry path |

## Files/Tests

- Core logic: `Async/Track/TransactionCompletionHandler.hpp`
- Tests: `tests/CompletionRefactorPlanTests.cpp` (built via `tests/CMakeLists.txt`)
- Host stubs: `tests/mocks/DriverKit/IOLib.h`, `tests/mocks/DriverKit/IOReturn.h`

## Validation

```
cmake -S tests -B out_test_refactor -DCMAKE_BUILD_TYPE=Debug
cmake --build out_test_refactor --target CompletionRefactorPlanTests
out_test_refactor/CompletionRefactorPlanTests
```

All current cases pass (see test log). Add more cases as protocols expand. 
