# TX completion and finite DMA queue ownership

Source implementation, September 7, 2026. Not yet installed or hardware-validated.

## Problem and repair

The Instruments-associated capture reached a seal check at the exclusive mapped
frontier. Elapsed cycles had been used to invent absolute completion identities.
The subsequent descriptor-status repair removed that inference, but a cyclic
DMA chain could still revisit an old payload after the completion scan and
before software returned ownership or replaced its descriptor.

The descriptor slab is still reused modulo its capacity. The **reachable DMA
program is now finite**, with a zero branch at its published tail:

1. Prime publishes a zero-terminated chain. The cycle-loss skip address remains
   self-linked; it does not branch to another packet.
2. Refill counts contiguous OUTPUT_LAST completions only within the mapped
   region. It retains the newest completed descriptor and its payload until its
   successor completes. That descriptor may still be hardware's continuation
   anchor after a tail stop.
3. Older completions pass seal verification before completionCursor releases
   ownership. Their descriptors cannot be reached again from the live queue.
4. Refill prepares a detached batch in those reusable slots. Its tail is zero.
   All payloads and descriptors are published, followed by a DMA publication
   barrier, before one aligned branch-word store links the old tail to it.
5. A successful append issues WAKE even when a sampled active bit was set:
   hardware may have prefetched the old zero branch. Stopped/dead contexts are
   not restarted by this operation.

A software stall can therefore let hardware stop at the published tail, but
cannot make it replay retired payloads. A failed partial batch remains
unreachable. Retaining the continuation anchor prevents a subsequent refill
from overwriting hardware's resume point before its successor completes.

The observed pending-descriptor margin excludes the completed anchor; retained
ownership must not masquerade as DMA lead. Allocation and published queue
capacity are unchanged. Retirement now holds one completed packet, so this is
not a claim of unchanged scheduling margins or measured RTL.

## Behavioral sources

Read-only, behaviorally cross-checked against
`references/linux-ohci-firewire-low-level-stack/ohci.c`:

- 954–982: retain `ctx->last`; reclaim earlier descriptor storage only after
  advancing to the next completed program.
- 1105–1107 and 1126–1141: initialize new descriptors to zero, publish them before
  updating the previous program's branch.
- 2918–2922 and 3302–3306: completion status and unconditional status-update bit.
- 3250–3256: self-linked cycle-loss skip address.
- 3471–3476: wake when flushing appended isochronous work.

No reference implementation was copied. The transport remains payload-opaque.

## Scope and remaining work

If a scan finds the entire mapped region consumed, TX still takes the existing
fault-stop path. It retains the terminal anchor instead of guessing how to reuse
it. This is queue exhaustion, not proof of producer starvation or a macOS bug.
Coordinated producer/TX restart remains open; the current recovery handler can
restart TX with its producer disabled. This change does not repair the separate
IT interrupt / AT completion-delivery failure.

Next hardware check: install the verified build, play ordinary audio, capture
any fault before resetting, and inspect completion/mapped frontiers, pending
margin, exhaustion and wake behavior. Then exercise the Instruments workload.
Do not interpret a host descriptor emulator as evidence of controller-specific
prefetch/coherency behavior or of unchanged latency.

## Verification

The full host suite completed with no failures: 1,927 passed and seven skipped
(1,934 registered). DriverKit/app build passed without warnings. After the
pending-margin counter correction, 59 targeted tests passed again. Targeted tests
exercise the actual generated branch graph during a refill stall, detached
batch publication order, terminal-anchor retention, failed-batch isolation and
WAKE while active. Four isolated mutations were killed: restoring the cyclic
prime tail, recycling the newest completion, removing the append publication
barrier and waking only when idle. Mutation sources/binaries were built under
`/tmp`; production files were not modified for mutation testing.
