# Async Compare-and-Swap Flow

DriverKit users and internal subsystems share the same lock pipeline for IEEE 1394 compare-and-swap (extended tCode `0x02`). This note captures the major entry points, packet construction, and completion reporting inside `ASFW/ASFWDriver` so we have a single reference when debugging CSR lock behavior.

## 1. Entry from the User Client
- Method selector `17` in `ASFWDriverUserClient` routes to the transaction handler (`ASFW/ASFWDriver/UserClient/Core/ASFWDriverUserClient.cpp:252`).
- `TransactionHandler::AsyncCompareSwap` validates arguments, ensures the operand contains `compare||new` quadlets, and fills `LockParams` with destination node/CSR address plus the desired response length (`ASFW/ASFWDriver/UserClient/Handlers/TransactionHandler.cpp:250`).
- The handler calls the async subsystem with extended tCode `0x02` and captures a completion lambda that records the result in `TransactionStorage`.

## 2. LockParams and Operand Expectations
- `LockParams` (single source of truth for async lock configuration) lives in `ASFW/ASFWDriver/Async/AsyncTypes.hpp:328`. It carries the remote node ID, 48-bit CSR address, operand pointer/length, expected response length, and optional speed override.
- For compare-and-swap we always transmit 8 bytes (compare value + new value) and request a 4-byte response containing the old quadlet so the caller can determine whether the compare succeeded.

## 3. Submission through AsyncSubsystem
- `AsyncSubsystem::Lock` simply wraps the params in a `LockCommand` and runs the shared `AsyncCommand` pipeline (`ASFW/ASFWDriver/Async/AsyncSubsystem.cpp:694`).
- `PrepareTransactionContext` gates on bus-reset state, checks the OHCI NodeID valid bit, captures the current generation, and supplies the packet context (source node, generation, default S100 speed) for packet construction (`ASFW/ASFWDriver/Async/AsyncSubsystem.cpp:639`).
- `AsyncCommandImpl::Submit` registers the transaction, assigns a label, builds the packet header, allocates DMA for the operand, builds the descriptor chain, tags it with the handle, submits it to the AT request context, and arms the timeout/Tracking infrastructure (`ASFW/ASFWDriver/Async/Commands/AsyncCommandImpl.hpp:17`).

## 4. Packet Format and DMA Payload
- `LockCommand::BuildMetadata` identifies the transaction as `tCode 0x9` (Lock Request) and requests completion on the AR path; for compare-and-swap with an 8-byte operand we explicitly expect a 4-byte response payload (`ASFW/ASFWDriver/Async/Commands/LockCommand.cpp:8`).
- `PacketBuilder::BuildLock` produces the 16-byte OHCI internal header, inserting the tLabel, retry bits, destination node (bus bits patched from the local source), CSR address, operand length, and the extended tCode (`ASFW/ASFWDriver/Async/Tx/PacketBuilder.cpp:418`).
- `LockCommand::PreparePayload` copies the operand into a DMA buffer (host→device) so the OHCI AT context can fetch the compare/new quadlets without touching user memory (`ASFW/ASFWDriver/Async/Commands/LockCommand.cpp:48`).

## 5. AR Response Handling and Status Mapping
- AR packets are parsed in `RxPath`. The parser extracts tLabel/source/destination IDs and the rCode, fixes read-quadlet payload placement, and forwards an `RxResponse` to the tracking actor (`ASFW/ASFWDriver/Async/Rx/RxPath.cpp:420`).
- `Track_Tracking::OnRxResponse` builds a match key (node, generation, label) and defers to `TransactionCompletionHandler::OnARResponse` for the actual completion (`ASFW/ASFWDriver/Async/Track/Tracking.hpp:250`).
- `TransactionCompletionHandler::OnARResponse` verifies the transaction is in `AwaitingAR`, transitions it to `ARReceived`, translates `rcode == 0` into `kIOReturnSuccess`, invokes the stored response handler and frees the tLabel for reuse (`ASFW/ASFWDriver/Async/Track/TransactionCompletionHandler.hpp:229`).
- Each registered transaction already wraps the caller’s callback; when invoked it converts the `kern_return_t` into `AsyncStatus` (`kSuccess`, `kTimeout`, `kHardwareError`, etc.) and forwards the original handle value back up to the user (`ASFW/ASFWDriver/Async/Track/Tracking.hpp:132`).

## 6. User-Visible Completion and Storage
- The completion lambda created in the transaction handler runs `AsyncCompletionCallback`, which writes the status + response buffer into the user client’s ring (`TransactionStorage`) and fires the registered async notification (`ASFW/ASFWDriver/UserClient/Handlers/TransactionHandler.cpp:23`, `ASFW/ASFWDriver/UserClient/Core/ASFWDriverUserClient.cpp:326`).
- `TransactionStorage` keeps up to 512 bytes per entry so `GetTransactionResult` can later return the old quadlet(s) plus the status fields to the SwiftUI helper (`ASFW/ASFWDriver/UserClient/Storage/TransactionStorage.cpp:17`).

## 7. Other In-Driver Users
- Subsystems such as the AVC PCR manager reuse the exact same API: they assemble an 8-byte operand, call `AsyncSubsystem::Lock`, and confirm the response matches their expected “old” value before accepting the update (`ASFW/ASFWDriver/Protocols/AVC/PCRSpace.cpp:136`).
- The IRM stack interacts through `IFireWireBusOps::AsyncLockCompareSwap`, whose `AsyncSubsystemBusOps` implementation is just a thin wrapper around the async subsystem (`ASFW/ASFWDriver/IRM/AsyncSubsystemBusOps.hpp:66`).

With this sequence in mind, troubleshooting compare-and-swap boils down to (1) confirming `LockParams` and the operand buffer are correct, (2) ensuring the request survives the submit pipeline (no bus reset gate, descriptor builder ready), and (3) checking AR responses/transaction storage for the returned “old value.”
