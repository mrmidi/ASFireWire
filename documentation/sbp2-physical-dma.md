# SBP-2 physical DMA validation

The review of `0f1993a2` identified two confirmed lifetime/locking defects:
the bus-reset interrupt retained the hardware gate across callbacks that re-enter
the gate, and command buffers were unmapped before timeout recovery stopped the
target. These are addressed together with the filter admission and diagnostics
needed to validate physical DMA on hardware.

## Correctness changes

- Release the MMIO scope before reset notifications. Read the generation from
  `SelfIDCount[23:16]`; offset `0x06C` is reserved, not a generation register.
- Explicitly clear both physical filters at reset and shutdown. Clear them and
  close the access gate in the same critical section before PCI detach.
- Admit only local nodes 0 through 62 in the current generation, outside reset.
  Verify filter writes by readback, including a second generation/reset check.
- Count physical-access leases per node, so one LUN cannot disable another LUN.
- Keep the data buffer, page table and command ORB through timeout recovery.
  Release them after successful LUN and fetch-agent reset. If recovery fails,
  revoke the node before freeing resources and request a bus reset. An active
  command released by its client follows the same revocation rule. Revocation
  fences all LUNs on that node until reset; other nodes retain their filters.
- Reject software requests into physical memory with `TYPE_ERROR`. Log anomalies
  at counts 1, 2, 4, 8, etc. A successful transfer cannot silently use software.
- Restrict driver-owned software ORB/status/page-table ranges to the target node.
  Check the source again under the range lock when serving the request.
- Resume interrupted login/reconnect only after discovery supplies a fresh route.
- Program `PhyUpperBound` for 4 GiB, validate the entire DMA segment against the
  32-bit limit, and reject address-range overlap.
- Bound `max_payload` by link speed, local `max_rec`, and the configured limit;
  round down to a supported power of two.
- Use cached-buffer zeroing for physical ranges. Retain uncached-safe stores for
  software response buffers. Use the canonical `rcvPhyPkt` bit, `1 << 10`.
- Read posted-write fault addresses before acknowledging `postedWriteErr`.

## Behavioral sources

Local references are read-only and excluded from Git. No source code was copied.

| Behavior | Local reference |
| --- | --- |
| Generation check and node filter | `linux-ohci-firewire-low-level-stack/ohci.c:2600-2631` |
| Physical upper bound | `linux-ohci-firewire-low-level-stack/ohci.c:2348`, `core.h:289` |
| Software low-memory rejection | `linux-ohci-firewire-low-level-stack/core-transaction.c:1414-1431` |
| Posted-write fault read order | `linux-ohci-firewire-low-level-stack/ohci.c:2121-2125` |
| PHY receive bit | `linux-ohci-firewire-low-level-stack/ohci.h:57` |
| Reset before ORB unmap/cancellation | `linux-ohci-firewire-low-level-stack/sbp2.c:1544-1553` |
| Payload limits | `linux-ohci-firewire-low-level-stack/sbp2.c:1169-1176` |
| Rediscovery before filter configuration | `IOFireWireFamily.kmodproj/IOFireWireFamily.kmodproj/IOFireWireController.cpp:4066-4097` |
| LUN and agent reset before task completion | `IOFireWireSerialBusProtocolTransport/IOFireWireSerialBusProtocolTransport.cpp:1553-1556,2025-2048` |

The newly downloaded Apple reference is commit
`ad9f879946570c786a80c731c8e9baaa490994ed` from
<https://github.com/apple-oss-distributions/IOFireWireFamily>.
Linux `sbp2.c` was downloaded from
<https://github.com/torvalds/linux/blob/master/drivers/firewire/sbp2.c>;
SHA-256: `e93cb3cceb15cb8c15692565947411c1583c25a78d62430bc2e5a40a003135ef`.

## Hardware checks still required

Validation on 2026-10-20: 3,805 host tests passed; six existing tests were skipped.
The unsigned Debug DriverKit target built successfully for `x86_64` and `arm64e`.
XcodeGen regeneration is unchanged. The full app build is blocked by the missing
Metal Toolchain. No driver was installed and no hardware transfer was run.

Host tests cannot establish DART coherence, drain an actual OHCI physical
response unit, or verify scanner firmware recovery. Filter readback confirms
register state; it does not prove that a preceding posted DMA write has drained.

1. Capture `[PhysicalDMA]`, `CommandExecutor`, `Posted write error` and
   `LogConfig initialized` messages. Confirm one successful arm for the current
   target, zero software-request anomalies, and zero posted-write/DART faults.
2. Scan the same frame/settings as the baseline. Measure throughput and compare
   image integrity. Exercise direct descriptors, page tables, transfers above
   1 MiB, and the current maximum transfer size. Exercise data-out commands too.
3. Reset during transfer, then change topology so the target gets a new node ID.
   Confirm filter clear, rediscovery and re-arm for the new ID, then full-speed
   transfers. Repeat across sleep/wake and target power cycling.
4. Cancel repeatedly, close the client during READ, disconnect during READ,
   and stop the driver during READ. Exercise timeout/LUN-reset failure if it can
   be induced safely. Check for crashes, leaked tasks and corrupt next commands.
5. With a multi-LUN target, log out one idle LUN while another transfers. Its
   filter must remain armed. An unconfirmed active abort fences the shared node
   and requires bus recovery.
6. Report command/byte counts, reset counts and exactly which recovery paths ran.
   A successful normal scan alone does not validate teardown or timeout paths.

The legacy Apple `PhysicalUnitBlocksOnReads` workaround is keyed to a link-driver
property. No equivalent property or demonstrated affected ASFW controller was
identified, so no speculative hardware quirk was introduced.

```sh
/usr/bin/log show --last 30m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[PhysicalDMA]" OR eventMessage CONTAINS "CommandExecutor" OR eventMessage CONTAINS "Posted write error" OR eventMessage CONTAINS "LogConfig initialized"'
```
