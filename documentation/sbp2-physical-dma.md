# SBP-2 command data over OHCI physical DMA

Command data (the buffer a READ fills or a WRITE drains) is carried by the
OHCI physical response unit. The target reads and writes the buffer's own IOVA
directly; no software runs per packet. Apple (`IOFireWireSBP2ORB.cpp:1002-1041`)
and Linux (`sbp2.c` `sbp2_map_scatterlist`, `ohci.c` `ohci_enable_phys_dma`)
carry SBP-2 data the same way. ORBs, page tables, status FIFOs and the login
response stay in the software-served `0xFFFF:xxxx` window.

For HBA tasks the buffer is the kernel's own: the SCSI shim maps the task
buffer for our PCI device before `UserProcessParallelTask` (`fBufferIOVMAddr`,
one segment, below 4 GiB because `UserGetDMASpecification` asks for 32-bit
addresses) and keeps it until `ParallelTaskCompletion`. `CommandRequest::dataIOVA`
hands that address to SBP-2 and nothing is copied. `UserGetDataBuffer` is never
called; per the SDK it invalidates the mapping. The raw user client has no
mapping of its own and gets a bounce buffer in the same address space.

## Mechanism

- `AddressSpaceManager::AllocatePhysicalRange` allocates a 32-bit IOVA bounce
  buffer (cacheable mapping, no shadow copy) for callers without a mapping.
- `HardwareInterface::EnablePhysicalRequests(node, generation)` sets the node's
  bit in `PhysicalRequestFilter` after checking that `SelfIDCount` still shows
  that generation and no `busReset` is pending (`ohci.c:2615`). Nothing clears a
  single node: the filter is cleared as a whole at the bus-reset interrupt and
  at async teardown. `PhysicalUpperBound` is programmed once at controller init
  (`ohci.c:2348`).
- `LoginSession` enables the target's node when a login or reconnect completes
  (`sbp2.c:1146`, `:1198`). A stale generation at that point means a reset is in
  flight; the session goes `Suspended` and reconnects after rediscovery.
- Requests below `PhysicalUpperBound` that still reach software are answered
  `TYPE_ERROR` (`core-transaction.c` `handle_low_memory`): they came from a node
  without the filter bit, or with a transaction type the physical unit does not
  take. A transfer can therefore not silently fall back to the software path.

## Buffer lifetime

The target may DMA into the buffer until it says it has stopped (SBP-2 §10.4).
`CommandExecutor` tracks this as a phase (`Idle`, `Active`, `Recovering`) and
frees the ORB, page table and buffer only at:

| Edge | Why the target is done |
|---|---|
| Status block for the ORB | the command completed |
| LUN-reset status after a transport failure | the task set is aborted (§10.4.3) |
| Bus reset | the node ID and the filter bit are gone with the generation |
| Logout status after the client released the session | logout aborts the task set (§8.4) |

An HBA task's buffer is reused by the kernel at `ParallelTaskCompletion`, which
the bridge issues at release. The dext teardown path clears the physical filter
before the bridge shuts down (`ExecuteRuntimeTeardown`), so no target can reach
a completed task's buffer; the other release cause is a device that has left
the bus.

A command released by its client is parked on the retiring session and freed
with it. If the logout is rejected or never answered, the target's claim is
unresolved and a bus reset is requested; the reset is then the edge that frees
the command, and the only reset the release path ever asks for. AGENT_RESET has
no status (§6.4.2); its outcome does not change the lifetime. A LUN reset that
the target never fetches keeps the existing escalation to a bus reset, which is
then the edge that frees the buffer.

## Exposure

A node with its filter bit set can read and write any address below
`PhysicalUpperBound` that the OHCI's IOMMU context maps: command buffers, but
also DMA rings, the Self-ID buffer, the Config ROM image and audio buffers.
Source node IDs are not authenticated. This is the same exposure as Apple's and
Linux's SBP-2 stacks, limited to nodes we logged in to, for the generations we
were logged in.

## Hardware evidence

- LS-9000 (software path → physical): no regression in scan, cancel, unplug.
- Hasselblad H4D-50MS: capture 22.9 → 48.0 MB/s (PR #213 test run).
