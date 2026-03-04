# IFireWireBus Contract

This document defines the behavioral contract for `ASFW::Async::IFireWireBusOps` /
`ASFW::Async::IFireWireBus`.

## Completion semantics

- For every submitted operation (`ReadBlock`, `WriteBlock`, `Lock`), the completion callback is
  invoked **exactly once** with the final result.
- `Cancel(handle) == true` guarantees that the callback is invoked **exactly once** with
  `AsyncStatus::kAborted` and an empty payload.

## Reentrancy / scheduling

- Completions must **not** be invoked inline on the submit path (no callback re-entrancy from
  within `ReadBlock`/`WriteBlock`/`Lock`).
- Completions must **not** be invoked inline on the cancel path (no callback re-entrancy from
  within `Cancel`).

## Generation guard

- `FW::Generation` is a caller-provided guard used to reject stale operations.
- If the supplied generation does not match the current bus generation, the operation must
  complete with `AsyncStatus::kStaleGeneration` and an empty payload.

## Buffer lifetimes

- `WriteBlock(data)` and `Lock(operand)` input buffers only need to remain valid until the call
  returns (the driver copies them before returning).
- Callback payload spans are valid only for the duration of the callback invocation.

