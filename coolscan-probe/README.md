# CoolScanProbe

"Proof of life" for driving a **Nikon CoolScan 9000 ED** (FireWire / SBP-2) on
**macOS Tahoe** via the [ASFireWire](https://github.com/mrmidi/ASFireWire) stack.

The tool talks to the ASFireWire dext through its DriverKit user client and
exercises the whole chain end-to-end:

```
open dext → enumerate the bus → find SBP-2 target → login → INQUIRY → TEST UNIT READY
```

A successful `INQUIRY` reporting `Nikon … LS-9000` is the **go/no-go point**
for the entire project. That proves the transport layer (OHCI → async → SBP-2 → SCSI passthrough),
and everything beyond is porting the CoolScan command set (reference:
[SANE coolscan3](http://www.sane-project.org/man/sane-coolscan3.5.html)) on top of
`SBP2Session.sendSCSI(...)`.

## How it relates to ASFireWire

The tool does **not** touch the driver — it only consumes the generic
SCSI passthrough API that already exists in `ASFWDriver/UserClient/Handlers/SBP2Handler`.
Selector numbers, wire formats and service names are mirrored directly from the ASFireWire source:

| Step | ASFireWire selector | Source |
|------|--------------------|-------|
| Enumerate devices | 16 `GetDiscoveredDevices` | `DeviceDiscoveryWireFormats.hpp` |
| Create session | 53 `CreateSBP2Session` | `SBP2Handler.hpp` |
| Login | 54 `StartSBP2Login` | `SBP2SessionRegistry` |
| Login status | 55 `GetSBP2SessionState` | `LoginState` enum |
| Send SCSI CDB | 59 `SubmitSBP2Command` | `SBP2CommandWireFormats.hpp` |
| Fetch result | 60 `GetSBP2CommandResult` | — |

## Prerequisites

1. **The ASFireWire dext must be built and loaded.** Per ASFireWire's README:
   build/sign via Xcode with a paid Apple account + entitlement, **or** a free
   account + **SIP disabled**. Recommended:
   ```sh
   systemextensionsctl developer on
   ```
   Verify it is loaded:
   ```sh
   systemextensionsctl list
   ```
2. **Hardware:** Apple TB→FW adapter (+ TB3→TB2 if needed) → FW800→FW400 to the
   CoolScan 9000. Scanner powered on.
3. The bus must have come up in ASFireWire (Self-ID / topology in the log).

## Build and run

```sh
swift build
.build/debug/CoolScanProbe
```

No entitlements are needed for *the probe itself* beyond it not being sandboxed
(a plain CLI tool is not).

## Interpreting the output

- `Found N device(s)` with a line marked `SBP-2 ✅` → bus and discovery work.
- `SBP-2 login OK` → the login/ORB/fetch-agent machinery in ASFireWire works against the 9000.
- `🎉 INQUIRY OK … vendor=«Nikon» product=«LS-9000 …»` → **the transport layer is proven.**

### If `IOServiceOpen` is denied (`KERN_PROTECTION_FAILURE`)
Then the dext restricts which clients may open the user client. With SIP off +
`systemextensionsctl developer on` this is normally not a problem. If it
happens anyway, the fallback is to run the same logic from the **ASFW app** (which already
has access) — the code here is deliberately split so `SBP2Session`/`SCSI` can be glued straight in.

## Roadmap

- [x] Transport probe (this one): discovery + login + INQUIRY + TEST UNIT READY
- [ ] CoolScan 9000 SCSI command set (mode select/sense, window, resolution,
      focus/autofocus, multi-sampling, IR channel for Digital ICE) — port from `coolscan3`
- [ ] Strip-wise reading of image data → TIFF
- [ ] Simple frontend (CLI/app)

## License

`coolscan3` is GPL; code ported from it makes derived tools GPL (fine for
personal use). ASFireWire currently has **no LICENSE file** — this tool
does not copy ASFireWire code, it consumes the dext at runtime.
