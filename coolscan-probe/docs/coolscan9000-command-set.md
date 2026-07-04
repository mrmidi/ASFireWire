# CoolScan 9000 ED — SCSI command set (mapped from SANE coolscan3)

This document maps the SCSI command set a CoolScan scanner uses, based
on a review of the SANE backend `coolscan3.c` (3191 lines, GPL). All commands
are sent as raw CDBs through `SBP2Session.sendSCSI(...)` in this probe — that is the
only "glue" missing between ASFireWire's SBP-2 transport and a working scanner.

> **Source:** `sane-backends/backend/coolscan3.c`. The backend builds CDBs with
> `cs3_parse_cmd(s, "hex hex …")` and fills in values with `cs3_pack_byte/word/long`.
> **All multi-byte fields are big-endian** (SCSI convention) — critical for our port.

---

## ⚠️ Three findings that affect the 9000 specifically

1. **coolscan3 does NOT support the LS-9000 explicitly.** The type enum stops at `CS3_TYPE_LS8000`,
   and an unknown product string yields `SANE_STATUS_UNSUPPORTED`. Product matching is plain
   string comparison on 16-character strings (`"LS-8000 ED      "`).
   **The 9000 is nearly identical to the 8000** (both medium format, same command family), so
   the port adds an `LS-9000` branch inheriting 8000 behavior. **The go/no-go probe's
   INQUIRY gives us the exact product string the 9000 reports** — we match on that.

2. **Digital ICE is not implemented in SANE.** The backend only delivers the raw
   **infrared 4th channel** (`n_colors = 4`, color code `0x09`). The actual dust removal
   is not done here (VueScan does it in software). Our MVP delivers the IR channel raw;
   the ICE algorithm is a separate, later step.

3. **Multi-sampling is averaged host-side.** With `samples_per_scan > 1` the backend reads
   `xfer_len_in *= samples_per_scan` bytes (N whole frames) and **averages them in software**.
   The scanner thus delivers N raw passes; we do the averaging.

---

## Opcode overview

| Opcode | CDB length | Command | Direction | Use |
|--------|-----------|----------|---------|------|
| `0x00` | 6 | TEST UNIT READY | none | Status polling (`scanner_ready`) |
| `0x03` | 6 | REQUEST SENSE | in | Fetch status/error bits |
| `0x12` | 6 | INQUIRY (std + EVPD pages) | in | Identification + capability pages |
| `0x15` | 6 | MODE SELECT(6) | out | Set base resolution unit |
| `0x16` | 6 | RESERVE UNIT | none | Reserve the scanner |
| `0x17` | 6 | RELEASE UNIT | none | Release the scanner |
| `0x1a` | 6 | MODE SENSE(6) | in | Read modes |
| `0x1b` | 6 | SCAN | none | Start scan (lists color channels) |
| `0x24` | 10 | SET WINDOW | out | Scan window (per color) — **the big one** |
| `0x25` | 10 | GET WINDOW | in | Read back window/exposure |
| `0x28` | 10 | READ(10) | in | Fetch image data |
| `0x2a` | 10 | WRITE(10) | out | LUT download + frame boundaries |
| `0xc0` | 6 | (vendor) status/phase | — | Phase check |
| `0xc1` | 6 | (vendor) EXECUTE / page inquiry | varies | Trigger execution; read Nikon pages |
| `0xe0` | 10 | (vendor) SET | out | Focus, autofocus, load/eject/reset |
| `0xe1` | 10 | (vendor) GET | in | Read focus etc. |

---

## Concrete CDBs (verified against coolscan3)

All examples are exact byte sequences from the backend. `‹word›`=2 bytes BE,
`‹long›`=4 bytes BE.

### Status / lifecycle
```
TEST UNIT READY   00 00 00 00 00 00                      (none)
RESERVE UNIT      16 00 00 00 00 00                      (none)
RELEASE UNIT      17 00 00 00 00 00                      (none)
EXECUTE (trigger) c1 00 00 00 00 00                      (none)   // after set-param cmd
LOAD medium       e0 00 d1 00 00 00 00 00 0d 00 + 13B    (out)
EJECT medium      e0 00 d0 00 00 00 00 00 0d 00 + 13B    (out)
RESET             e0 00 80 00 00 00 00 00 0d 00 + 13B    (out)
```

### Identification
```
INQUIRY (std)     12 00 00 00 ‹len› 00                   (in, len bytes)
INQUIRY (EVPD)    12 01 ‹page› 00 ‹len› 00               (in)   // page 0xC1 = Nikon capabilities
```
`scanner_ready` = loop of TEST UNIT READY (with REQUEST SENSE for status bits) until
the relevant bits are 0, 120 s timeout.

### MODE SELECT — set base resolution unit
Byte-exact from `cs3_mode_select`. The parameter list is a well-formed SCSI list:
4-byte header (block-descriptor-length = `0x08` at **offset 3**) + 8-byte block
descriptor + mode page (code `0x03`, length `0x06`) with `unit_dpi` at offset 16–17.
```
CDB (6B):    15 10 00 00 14 00                 (param length 0x14 = 20)
param (20B): 00 00 00 08 | 00 00 00 00 00 00 00 01 | 03 06 00 00 ‹unit_dpi:word› 00 00
             └ header ──┘ └ block descriptor ────┘ └ mode page 0x03/len 0x06 ───────┘
```
⚠️ `0x08` MUST be at offset 3 (block-descriptor-length), not offset 4. An earlier
hand transcription shifted `0x08` to offset 4 and dropped the trailing `00 00` → the 9000 responds
CHECK CONDITION `0x5/0x26` (INVALID FIELD IN PARAMETER LIST). coolscan3 sends RESERVE
UNIT (`16 …`) in `sane_open()` *before* MODE SELECT.

### Focus
```
SET FOCUS    e0 00 c1 00 00 00 00 00 09 00 00 ‹focus:long› 00 00 00 00   (out)
READ FOCUS   e1 00 c1 00 00 00 00 00 0d 00                                (in, 13B)
             // focus = ((b1<<8|b2)<<16) | (b3<<8|b4)  from the response
AUTOFOCUS    e0 00 a0 00 00 00 00 00 09 00 00 ‹focusx:long› ‹focusy:long› (out)
```

### SET WINDOW (0x24) — 58-byte window descriptor, sent **per color**
CDB (10B): `24 00 00 00 00 00 00 00 3a 00` (byte8 `0x3a`=58 transfer; byte9 `0x80` for
LS40/4000/50/5000, otherwise `0x00`). Then 58 bytes:

| Offset | Field | Value |
|--------|------|-------|
| 0–7 | header | `00 00 00 00 00 00 00 32` (0x32=50 = descriptor length) |
| 8 | color id | `cs3_colors[color]` (R/G/B/IR = 1/2/3/9) |
| 9 | — | `00` |
| 10–11 | resx | ‹word› (device units) |
| 12–13 | resy | ‹word› |
| 14–17 | x-offset | ‹long› |
| 18–21 | y-offset | ‹long› |
| 22–25 | width | ‹long› |
| 26–29 | height | ‹long› |
| 30–32 | brightness/contrast | `00 00 00` |
| 33 | image composition | `05` |
| 34 | bit depth | `real_depth` (8/14/16) |
| 35–47 | — | 13× `00` |
| 48 | multiread/ordering | `(samples_per_scan-1) << 4` |
| 49 | averaging + pos/neg | `0x80 | (negative ? 0 : 1)` |
| 50 | scan kind | `01` normal / `20` AE / `40` AE+WB |
| 51 | scanning mode | `02` single / `10` multi |
| 52 | color interleaving | `02` |
| 53 | (AE) | `ff` |
| 54–57 | exposure | ‹long› (×10 ns) — or `00 00 00 00` for IR |

### SCAN (0x1b) — after all windows are set
```
RGB    1b 00 00 00 03 00 01 02 03        (none)   // 3 colors + color codes
RGBI   1b 00 00 00 04 00 01 02 03 09     (none)   // 4 colors incl. IR (0x09)
```

### GET WINDOW (0x25) — read back exposure, per color
```
25 01 00 00 00 ‹color› 00 00 3a 00       (in, 58B)
// exposure = bytes[54..57] as long
```

### READ image data (0x28)
```
28 00 00 00 00 00 ‹xfer:3B BE› 00        (in, xfer bytes)
// xfer_len_in *= samples_per_scan before reading (multi-sample = N frames)
```

### LUT download (0x2a, for normal scan) — per color
```
2a 00 03 00 ‹color› 01 ‹2*n_lut:3B BE› 00 + LUT data(‹word› per point)   (out)
```

### Frame boundaries (0x2a, medium-format multi-frame — important for 8000/9000)
```
2a 00 88 00 00 03 ‹(4+n_frames*16):3B BE› 00 + boundary data             (out)
```

---

## Full scan sequence (`cs3_scan`)

> **OUTDATED for the 9000:** The VueScan capture (`capture/DECODED.md`) shows the
> real sequence uses neither SET BOUNDARY nor MODE SELECT, sends the SCAN list
> as data-OUT, and uses control byte 0x80 on SET WINDOW/READ. Follow DECODED.md;
> the list below is coolscan3's (partly incorrect) reconstruction.

1. `scanner_ready` (wait until document ready)
2. **convert_options** — compute geometry (host-side, no command)
3. **SET BOUNDARY** (`2a 00 88 …`) — frame boundaries (medium format)
4. **SET FOCUS** (`e0 00 c1 …`)
5. `scanner_ready`
6. **SEND LUT** (`2a 00 03 …`, only for normal scan)
7. **SET WINDOW** (`24 …`) — once per color (3 or 4)
8. **GET WINDOW** (`25 …`) — read exposure back
9. **SCAN** (`1b …`)
10. Loop: **READ(10)** (`28 …`) until all lines are fetched → assemble into image

Host-side post-processing: multi-sample averaging, bit shift (`shift_bits`), optional
LUT, and (later) ICE dust removal from the IR channel → write TIFF.

---

## How this maps to our probe

`SBP2Session.sendSCSI(cdb:direction:transferLength:outgoing:)` covers all the patterns:
- 6/10-byte CDB → `cdb` array
- data in (INQUIRY/READ/GET WINDOW) → `direction: .fromTarget`, `transferLength: N`
- data out (SET WINDOW/MODE SELECT/LUT/vendor SET) → `direction: .toTarget`, `outgoing: [...]`
- no data (TUR/SCAN/RESERVE/EXECUTE) → `direction: .none`

See `Sources/CoolScanProbe/CoolScan.swift` for the command builders (skeleton, ready for
testing once go/no-go has passed).

## Geometry model (from `cs3_convert_options`, used in the probe)

SET WINDOW expresses **offset/width/height in device units (1/`resx_max`″ = 1/4000″)** and
**resolution as absolute dpi**. coolscan3 sets the measurement unit with MODE SELECT
(`unit_dpi = resx_max`) *before* SET WINDOW. The computation:

```
pitchX        = resx_max / real_resx          (integer → res snaps to a divisor of 4000)
real_resx     = resx_max / pitchX             (e.g. 500→pitch 8→500; 1500→pitch 2→2000)
logical_width = (xmax - xmin + 1) / pitchX    (= number of output pixels)
real_width    = logical_width * pitchX        (= width in device units, sent in SET WINDOW)
real_xoffset  = xmin                          (device units)
```

READ(10) delivers a **byte stream**; per line all color planes follow one another
(line-sequential planar, order R,G,B[,IR]):

```
bytes_per_line = n_colors * (logical_width * bytes_per_pixel + odd_padding)
total          = bytes_per_line * logical_height
bytes_per_pixel = (depth > 8) ? 2 : 1     // 14-bit packed in 16-bit container
```

Implemented in `CoolScan.geometry(...)` / `CoolScan.scanFrame(...)`. Probe mode:
`CoolScanProbe scan [dpi]` (default 500 dpi) runs
**waitReady → MODE SELECT → SET WINDOW×color → SCAN → READ(10) loop** and saves the raw
stream + sidecar (`coolscan-…​.raw` / `.txt`) for offline inspection before we commit
to a TIFF writer.

## Open questions (require hardware to resolve)
- ~~Exact product string the 9000 reports~~ → confirmed (LS-9000 ED, go/no-go passed).
- ~~Contents of `0xC1` on the 9000~~ → mapped (see field table above).
- Status bit interpretation from REQUEST SENSE on the 9000 (when it is "not ready").
- **Is SEND LUT (`2a 00 03`) needed before READ?** The probe skips the LUT — if the scanner
  refuses to deliver data without it, that is the first thing to add.
- **Frame offset for the FH-835S:** the probe scans the whole `boundary` area (the whole
  aperture window) — we do not yet extract per-frame position. Fine for a first proof; a
  strip scan shows the frames and lets us calibrate the offset.
- **Actual byte/color layout** in the READ stream (interleave + byte order) —
  assumed line-sequential 16-bit BE; to be confirmed against the first real `.raw` before TIFF.
- SET FOCUS / autofocus is skipped in v1 (the image may be soft, but pixels will arrive).
- Whether the 9000 needs extra init beyond the 8000 (e.g. medium holder/adapter detection).

## EVPD 0xC1 — Nikon capability page (hardware capture, LS-9000 ED rev 1.02)

Captured 2026-06-06 from a real CoolScan 9000 ED via ASFireWire. Header-first read:
`12 01 c1 00 05 00` returns 5-byte header; byte 3 = page_len = 0x57 (87) → total 91 bytes.

```
off  00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
00   06 c1 00 57 01 00 3b 00 0f 00 00 01 00 01 01 10
10   42 12 0f a0 0f a0 02 9a 00 00 27 0f 00 00 00 00
20   00 00 00 00 00 00 27 10 0f a0 0f a0 01 4d 00 00
30   36 23 00 00 00 00 00 00 00 00 00 00 36 24 00 00
40   00 00 00 00 00 00 00 53 00 53 00 00 00 00 01 c2
50   00 00 10 27 10 0c 03 00 53 00 1b
```

Header: byte0=0x06 PDT(scanner), byte1=0xC1 page, byte3=0x57 page_len(87).

### Field map (verified against coolscan3 `cs3_full_inquiry`)

coolscan3 reads the capability page with **absolute offsets into the full INQUIRY
response** (byte 0 = PDT). All values big-endian. Mapping each offset against the
captured 9000 dump above:

| Offset(s) | coolscan3 field | Formula | Captured bytes | Value | Meaning |
|-----------|-----------------|---------|----------------|-------|---------|
| 18–19 | `resx_optical` | `256·b18+b19` | `0f a0` | **4000** | Optical X resolution (dpi) |
| 20–21 | `resx_max` | `256·b20+b21` | `0f a0` | **4000** | Max X resolution (dpi) |
| 22–23 | `resx_min` | `256·b22+b23` | `02 9a` | **666** | Min X resolution (dpi) † |
| 36–39 | `boundaryx` | `(b36b37)≪16 \| b38b39` | `00 00 27 10` | **10000** | Max scan width (device px @ optical) |
| 40–41 | `resy_optical` | `256·b40+b41` | `0f a0` | **4000** | Optical Y resolution (dpi) |
| 42–43 | `resy_max` | `256·b42+b43` | `0f a0` | **4000** | Max Y resolution (dpi) |
| 44–45 | `resy_min` | `256·b44+b45` | `01 4d` | **333** | Min Y resolution (dpi) † |
| 58–61 | `boundaryy` | `(b58b59)≪16 \| b60b61` | `00 00 36 24` | **13860** | Max scan height (device px @ optical) |
| 75 | `n_frames` | `b75` | `00` | **0** | Frame count (0 = no holder inserted; dynamic) |
| 76–77 | `focus_min` | `256·b76+b77` | `00 00` | **0** | Min focus value |
| 78–79 | `focus_max` | `256·b78+b79` | `01 c2` | **450** | Max focus value |
| 82 | `maxbits` | `b82` | `10` | **16** | Max bit depth per channel |

† **resx_min=666 / resy_min=333 are surprising** for a "minimum" (CoolScans preview
far lower). The asymmetry (666 vs 333) and high value suggest the field may carry a
different semantic on the 9000, or be a minimum *full-quality* step. Non-critical for
SET WINDOW geometry — flagged, not relied on.

**Unparsed-but-present values** (not read by coolscan3, noted for completeness):
`b26–27 = 0x270f = 9999`; `b48–49 = 0x3623 = 13859` (one less than boundaryy);
`b71 = b73 = 0x53 = 83`; trailing `b83.. = 27 10 0c 03 00 53 00 1b`. The `0x2710`/
`0x3624` boundary values reappear here, consistent with a secondary copy or a
related limit. Left unmodelled until a need surfaces.

### Derived capability summary (CoolScan 9000 ED)

- **Optical resolution:** 4000 dpi (X and Y).
- **Max resolution:** 4000 dpi (no interpolation reported in-page).
- **Max scan area:** 10000 × 13860 device px @ 4000 dpi = **2.50″ × 3.465″ ≈ 63.5 × 88 mm**
  (consistent with the 6×9 cm medium-format bed).
- **Focus range:** 0 … 450.
- **Max bit depth:** 16 bits/channel.

These are now encoded in `CoolScan.Capabilities` and used to build a default
full-frame `Window` instead of hardcoded geometry. See `CoolScan.swift`.
