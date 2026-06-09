# Capturing the CoolScan 9000's real SET BOUNDARY / SET WINDOW sequence

## Why
On Tahoe (via ASFireWire) the whole SBP-2/SCSI transport is proven — login, INQUIRY,
EVPD 0xC1, RESERVE, MODE SELECT (byte-fixed), scanner-ready, data-OUT delivery all
work. The **only** unsolved piece is the proprietary 35mm/medium-format **frame
geometry**: the 9000 rejects *every* host-defined `SET BOUNDARY` we construct
(`0x5/0x26 INVALID FIELD IN PARAMETER LIST`, or `0x24` when 12 frames overflow the
transfer), and `SET WINDOW` is gated behind a valid boundary. There is **no public
SCSI spec** for the 8000/9000 (NDA-only), SANE's coolscan3 doesn't support the 9000,
and its frame math is LS-30-specific. So we stop guessing and **capture the exact
bytes from VueScan** (which has the NDA spec), on a pre-Tahoe Mac that still has
FireWire, then port them into the probe.

## What we need from the capture
The exact **CDB + data-out payload** (the data is where the geometry lives) for a
single 35mm-frame scan, in order:
- `SET BOUNDARY` (`2a 00 88 …`) + payload — the real 12-frame coordinates
- `SET WINDOW` (`24 …`) per colour + the 58-byte descriptor
- `SCAN` (`1b …`), `READ(10)` (`28 …`)
- anything between: `SET FOCUS` (`e0 …`), frame-select, `SEND LUT` (`2a 00 03 …`)

## Prereqs (pre-Tahoe Mac)
- 9000 powered, **FH 35mm strip holder inserted** (so the device reports its 12 frames).
- TB→FW adapter chain (same as the Tahoe box) → 9000.
- VueScan installed; Source = the FireWire 9000, Media = the 35mm strip holder.
- **SIP off** (DTrace needs it): `csrutil status` → "disabled".

## Method: DTrace SCSITaskLib (see `scsitask-trace.d`)
VueScan does NOT use IOFireWireSBP2Lib (verified on Sequoia 15.7.4: vmmap shows no
FireWire lib in the process). The SBP-2 LUN is bridged in-kernel
(IOFireWireSerialBusProtocolTransport → IOSCSIPeripheralDeviceNub) and VueScan opens
a **SCSITaskUserClient** on the nub (IORegistry: `IOUserClientCreator = pid …, VueScan`).
So we trace SCSITaskLib's CDB/buffer setters instead. (`sbp2-trace.d` is kept for
apps that do use the SBP-2 plugin directly, e.g. old Nikon Scan.) Steps:

1. `pgrep -x VueScan` to get the pid (launch VueScan first).
2. `sudo dtrace -Z -qs scsitask-trace.d -p "$(pgrep -x VueScan)" -o capture.txt`
3. In VueScan: **Preview** the strip (triggers frame detect + SET BOUNDARY), then
   **Scan ONE frame** at a low resolution. Then Ctrl-C the dtrace.
4. Inspect `capture.txt` — find the `[CDB …] 2a 00 88 …` (SET BOUNDARY) and its
   following `[DATA-OUT …]`, and the `24 …` (SET WINDOW) + its 58-byte data.

**If no probes fire**, discover the real symbols (VueScan may name them differently):
```
sudo dtrace -ln 'pid$target::*ommand*:entry'  -p "$(pgrep -x VueScan)"
sudo dtrace -ln 'pid$target::*ORB*:entry'      -p "$(pgrep -x VueScan)"
sudo dtrace -ln 'pid$target::*FireWire*:entry' -p "$(pgrep -x VueScan)"
```
and point `sbp2-trace.d`'s probes at the real names. Exact IOFireWireSBP2Lib
signatures are in this repo at `docs/IOFireWireFamily/`.

### Fallbacks
- **VueScan log**: check VueScan's Output/Prefs for a raw-command log (uncertain it
  dumps CDBs, but free to try first).
- **DYLD interpose**: re-sign VueScan to drop library-validation, then
  `DYLD_INSERT_LIBRARIES` a dylib that wraps the IOFireWireSBP2Lib ORB setters.
  Heavier; only if DTrace can't see the data.

## What we already know (to recognise the right packets)
- With the holder in, the device reports **per-frame box 4000×5904 du**, `n_frames=12`,
  frame pitch ≈ `36219/6 ≈ 6036` (≈ coolscan3 `frame_offset=resy_max·1.5+1=6001`),
  `boundaryX-1/boundaryY-1 = 9999/13859`.
- `SET BOUNDARY` wire shape (from coolscan3, byte-confirmed):
  `2a 00 88 00 00 03 [len:3B] 00` + `[len:2B][n][n]` + n×`[yOff:4][0:4][yEnd:4][xBound:4]`.
- `SET WINDOW`: `24 00 00 00 00 00 00 00 3a 00` + 58-byte descriptor. **Our descriptor
  is byte-exact to coolscan3 already** — so the capture's real job is the SET BOUNDARY
  **frame coordinates** + the exact pre-scan command order.

## After the capture
Paste the `SET BOUNDARY`/`SET WINDOW`/`SCAN` lines (CDB + data) back to a Claude
session on the Tahoe box. It ports them into `CoolScan.setBoundary` / `setWindow`
(and the pre-scan order into `scanFrame`), and we run a real scan through the probe.
See memory `coolscan-9000-transport-state` for full context.
