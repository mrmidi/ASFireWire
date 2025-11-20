# Async Completion Strategy Guide

This note explains the hardware/driver guarantees for each completion strategy the FireWire async stack uses. The goal is to keep all async transactions predictable and to clearly spell which phase signals finality.

---

## Strategy Summary Table

| Strategy | Description | AT completion | AR response | Typical use cases |
| --- | --- | --- | --- | --- |
| `CompleteOnAT` | Unified transactions that finish as soon as an AT ack arrives with `ack_complete`. | ✅ completes | ❌ ignored | Write quadlet/block (non-deferred), PHY commands that rely on AT ack semantics (
| `CompleteOnPHY` | PHY control packets (tCode `0xE`). Complete on the AT-level ack but treat the packet as link-local; AR packets must be handled separately. | ✅ completes | ❌ ignored, routed elsewhere | PHY packet command acknowledgments | 
| `CompleteOnAR` | Split transactions that need actual payload or lock responses. | ⚠️ stores ack and waits | ✅ required | Read quadlet/block, lock operations |
| `RequireBoth` | Deferred writes or any operation where AT ack signals acceptance but AR response finalizes the transaction. | ⚠️ transitions to `AwaitingAR` | ✅ required | Write block with `ack_pending`, deferred notify flows |


## Traits and Compile-Time Concepts

The completion strategy header exposes helper traits to make intent obvious:

* `RequiresARResponse(CompletionStrategy)` – true for strategies that never finish without an AR response (`CompleteOnAR`, `RequireBoth`).
* `ProcessesATCompletion(CompletionStrategy)` – true for strategies that observe AT acks (`CompleteOnAT`, `CompleteOnPHY`, `RequireBoth`).
* `CompletesOnATAck(CompletionStrategy)` – true if AT completion should finalize the command immediately (`CompleteOnAT`, `CompleteOnPHY`).

Concepts allow compile-time guarantees:

* `ARCompletingTransaction<T>` – strategy requires AR response, so `OnATCompletion()` must defer and `OnARResponse()` completes.
* `ATCompletingTransaction<T>` – AT ack completes the command.
* `PHYCompletingTransaction<T>` – completes on AT but explicitly forbids AR dependencies.

These concepts help command implementations inherit the right flow (see `CompletionBehavior.hpp`).

## Strategy-to-tCode Mapping

`StrategyFromTCode(uint8_t tCode, bool expectsDeferred)` maps IEEE 1394 transaction codes to completion strategies:

* **Read quadlet/block (`tCode` 0x4/0x5)** → `CompleteOnAR`.
* **Lock (`tCode` 0x9)** → `CompleteOnAR`.
* **Write quadlet (`tCode` 0x0)** → `CompleteOnAT`, unless `expectsDeferred` is true.
* **Write block (`tCode` 0x1)** → `CompleteOnAT` by default, `RequireBoth` when deferred.
* **Stream (`tCode` 0xA)** → treated as `CompleteOnAR` if a response is expected.
* **PHY packet (`tCode` 0xE)** → `CompleteOnPHY`.

Static asserts in the header guarantee this mapping stays correct.

## Practical Notes

* `CompleteOnPHY` transactions behave like `CompleteOnAT`, but they are geared specifically for PHY traffic (AT ack finishes the command while AR packets with the same `tCode` are routed to PHY-specific listeners).
* AT-tracking logic must differentiate `CompleteOnPHY` from other strategies so that AR packets do not advance those transactions.
* Deferred writes that require both paths should leverage `DualPathCommand` so `OnATCompletion()` moves to `AwaitingAR`, and AR notifications output final data.
* PHY command contract users must set `completionStrategy = CompleteOnAT` inside their metadata because the async engine currently lacks an explicit PHY completion state machine; the AT ack is the only accepted completion signal.

Keep this guide updated alongside protocol changes to keep everyone aligned on the async engine lifecycle.