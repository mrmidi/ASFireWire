# VueScan → CoolScan 9000: decoded capture (2026-06-09, Sequoia 15.7.4)

Source: `capture.txt` (DTrace `scsitask-trace.d` on VueScan 9.8.54, ARM64, one
Preview + one low-dpi 35mm frame scan, FH-835S holder). All multi-byte fields
big-endian. Static-wrapper duplicates already deduped here.

## Headline findings

1. **VueScan never sends SET BOUNDARY (`2a 00 88`)** — the command our probe was
   blocked on. Frame positioning is done entirely with per-window **SET WINDOW
   offsets**. Drop setBoundary from the scan path.
2. **VueScan never sends MODE SELECT (`15`)** — no unit-dpi setup. Window offsets
   are in device units (1/4000″) by default; resolution is in dpi in the window.
3. Scans are **4 windows in one pass**: window IDs `01 02 03 09` = R, G, B, **IR**
   — one SCAN delivers all four channels (Digital-ICE IR comes for free).
4. `SET WINDOW` and `READ(10)` CDBs carry **control byte 0x80** (CDB byte 9) —
   ours sent 0x00. Copy exactly.
5. Every command needed is already proven working in our probe/dext (data-in,
   data-out, no-data) — this is purely a byte-level porting job.

## Full per-scan sequence (replicate in this order)

```
(TUR/INQUIRY polling, noise)
e1 00 c1 00 00 00 00 00 0d 00            data-IN  13 B   read frame/holder info (likely source of the window offsets)
e0 00 a0 00 00 00 00 00 09 00            data-OUT  9 B   payload 00 00 00 0b 68 00 00 0f 60
                                                          = set AF point (x=0x0b68=2920, y=0x0f60=3936 du)
c1 00 00 00 00 00                        no-data         execute autofocus (takes time — poll TUR)
e1 00 c1 00 00 00 00 00 0d 00            data-IN  13 B
e1 00 c1 00 00 00 00 00 09 00            data-IN   9 B   read AF result (focus position)
c0 00 00 00 00 00                        no-data         abort/idle
e1 00 c1 00 00 00 00 00 0d 00            data-IN  13 B
e0 00 c1 00 00 00 00 00 09 00            data-OUT  9 B   payload 00 00 00 00 eb 00 00 00 00
                                                          = set focus position (0xeb=235; caps range 0–450)
c1 00 00 00 00 00                        no-data         apply focus
24 00 00 00 00 00 00 00 3a 80  ×4        data-OUT 58 B   SET WINDOW, window IDs 01,02,03,09 (see below)
2a 00 03 00 <id> 01 00 80 00 00  ×4      data-OUT 32768 B SEND LUT per window id (01,02,03,09):
                                                          16384 entries × 2 B identity ramp 0000,0001,…,3fff
1b 00 00 00 04 00                        data-OUT  4 B   SCAN, payload = window id list 01 02 03 09
28 00 00 00 00 00 <len:3B> 80            data-IN         READ(10) image data, repeat until done
c0 00 00 00 00 00                        no-data         end-of-pass abort/idle
```

The second pass (preview re-run) was identical with AF point x=0x0b62. The final
scan pass skipped autofocus, only set focus (`e0 00 c1`, 0xeb) directly.

## SET WINDOW: CDB + 58-byte payload

CDB: `24 00 00 00 00 00 00 00 3a 80` (length 0x3a=58, **control 0x80**).
Payload = 8-byte header `00 00 00 00 00 00 00 32` (descriptor length 50) + 50 B:

| off | bytes (preview) | meaning |
|-----|-----------------|---------|
| 0 | `01`/`02`/`03`/`09` | window ID: R/G/B/IR |
| 1 | `00` | reserved |
| 2–3 | `02 9a` | X resolution = 666 dpi |
| 4–5 | `02 9a` | Y resolution = 666 dpi |
| 6–9 | `00 00 00 a5` | ULX = 165 du (preview); scan: `00 00 00 f9` = 249 |
| 10–13 | `00 00 00 cb` | ULY = 203 du (preview); scan: `00 00 01 2b` = 299 |
| 14–17 | `00 00 0f a0` | width = 4000 du (preview); scan: `0e b2` = 3762 |
| 18–21 | `00 00 17 10` | height = 5904 du (preview); scan: `16 44` = 5700 |
| 22–24 | `00 00 00` | brightness/threshold/contrast |
| 25 | `05` | image composition = 0x05 (multi-channel/16-bit mode) |
| 26 | `10` | bits per pixel = 16 |
| 27–40 | `00 …` | halftone/RIF/bit-order/compression + vendor byte 40: all zero (**14 bytes**) |
| 41–45 | `81 01 02 02 ff` | vendor block (constant across all windows/passes) |
| 46–49 | per window | **per-channel exposure**: preview R=40000, G=100000, B=140000, IR=0; scan R=240000, G=600000, B=840000, IR=0 (exactly 6× preview; R:G:B ≈ 1:2.5:3.5) |

> ⚠️ An earlier version of this table had the vendor block one byte early
> (40–44 + a trailing `00` at 45). The raw capture tail row is
> `30: 00 81 01 02 02 ff` + be32 exposure in **all 11 captured windows** —
> byte 40 is zero. `0x81` at byte 40 is rejected with CHECK CONDITION 5/26
> (proven on HW). Two more HW-proven constraints: resolution below the 0xC1
> caps minimum (666 dpi) → 5/26, and width/height go on the wire as **raw
> device units** (VueScan sends 4000 even when not a multiple of the pitch).

Notes:
- Preview window (165,203)+4000×5904 = exactly the 0xC1 caps frame box at a
  holder-specific offset → the offset likely comes from the `e1 00 c1 … 0d` read
  (13 B). Our probe can read it (data-IN works) and dump it.
- Resolution 666 dpi in both passes because the test scan was deliberately low-dpi.
  4000 must snap per caps (4000/2000/1333/1000/800/666… = 4000/N).

## READ(10) geometry (validates pixel layout)

- Preview pass: 10×522144 + 1×21312 = 5,242,752 B = 666 px × 984 lines × 4 ch × 2 B.
  width = 4000 du @666 dpi → 666 px; height 5904 du → 983.5 → 984 lines. ✔
- Scan pass: 9×521664 + 1×70224 = 4,765,200 B = 627 px × 950 lines × 4 ch × 2 B.
  3762 du → 626.7 → 627 px; 5700 du → 949.1 → 950 lines. ✔
- So: bytes_per_line = width_px × 8 (RGBI, 16-bit). VueScan reads ~98–104 whole
  lines per READ (~510 KB chunks). Whether the 4 channels are line-sequential or
  pixel-interleaved within a line: confirm from first real probe data.
- READ CDB transfer length [6–8] in bytes, control byte 0x80.

## EVPD page 0xC8 = the frame table (decoded on HW 2026-06-10, not in capture)

`12 01 c8 00 c5 80` (VueScan polls it ~200×; the DTrace capture has no DATA-IN,
so this was decoded from our own probe): header `06 c8 00 c1 0c` = PDT/page/
page-length 0xc1/**frame count 12**, then one 16-byte record per frame:
`{ULX:be32, ULY:be32, width:be32, height:be32}` in device units. With the
12-frame 120-holder: two columns (x=165, x=5834) × six rows (y=203, 6188,
12172, 18156, 24140, 30125), each 4000×5904. **Record 1 = (165,203)+4000×5904
= exactly the captured SET WINDOW box** — this page is where VueScan gets its
window geometry. Caps byte 75 (`nFrames`) matches the record count.

## SEND LUT

CDB `2a 00 03 00 <window-id> 01 00 80 00 00` — [4]=window id, [6–8]=0x008000=32768 B.
Payload: 16384 × 16-bit BE identity ramp (0x0000…0x3fff) — a 14-bit-deep gamma
table. Sent per window id (01,02,03,09) after SET WINDOW, before SCAN.

## Porting checklist (Tahoe box, coolscan-probe)

1. Delete setBoundary + modeSelect from the scan flow.
2. setWindow: control byte 0x80; payload exactly as the table (incl. `81 01 02 02 ff`
   vendor block + exposure tail); windows 01,02,03,09.
3. sendLUT(id): 32768 B identity ramp, CDB above.
4. SCAN with payload `01 02 03 09`.
5. READ(10) with control 0x80, whole-line chunks (bytes_per_line = w_px×8).
6. New focus helpers: setAFPoint (e0/a0), executeAF (c1), readFocus (e1/09),
   setFocus (e0/c1) — or v1: skip AF, send setFocus(235)+c1 like the captured scan.
7. First read `e1 00 c1 … 0d` (13 B) and dump — likely holder/frame offsets.
8. Expect ~5 MB per frame at 666 dpi — with the 2048-B inline cap that is ~2560
   reads; the structureOutputDescriptor work (or larger proven-safe chunks) becomes
   relevant for full-res scans (4000 dpi ≈ 189 MB/frame).
