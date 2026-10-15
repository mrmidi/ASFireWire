# MOTU 828mk3, 48 kHz: passive-observer timing captures

Derived from contributor mrmidi-2026-10-10/sph/. Original archive supplied by the
maintainer; full captures remain outside Git at
`/Users/mrmidi/Downloads/mrmidi-2026-10-10/`. The capture author is the PR #191
contributor; their original README documents the following sessions. No PCM,
GUIDs, register dumps or control transactions are included here.

| Session | Captured | Active audio host | Passive observer |
| --- | --- | --- | --- |
| `vendor-start-0.5s` | 2026-07-30 | OS X El Capitan 10.11.6, official MOTU driver | Second Mac, ASFW IR observer on the same bus |
| `vendor-sampled-90s` | 2026-08-22 | OS X El Capitan 10.11.6, official MOTU driver | Same passive observer setup |
| `asfw-start-0.5s`, `asfw-sampled-90s` | 2026-10-10 | MacBook M3, macOS 27.0.1, contributor ASFW fork 0.4.0 at `93c1ff3` | Linux Mint, firewire-cdev; bus manager/root/IRM, no audio or device control |

Each CSV contains only DATA rows: `cycle,sph`. `cycle` is the unwrapped bus
cycle number (`wire_sec * 8000 + wire_cycle`, extended across the observer's
8-second timer wrap); `sph` is the first sample's wire SPH as eight hex digits.
Decode SPH as `((sph >> 12) & 0x1fff) * 3072 + (sph & 0xfff)`, modulo one
second (24,576,000 ticks). Each DATA packet carries eight frames. SPHs for
subsequent frames and audio bytes were not captured and are not reconstructed.

`manifest.json` records SHA-256 of each original CSV, its first and last
observed cycles, total packet count, DATA count and whether it is continuous.
The continuous files contain exactly 4096 observed packets; absent DATA rows
inside that declared range mean observed NO-DATA, including startup silence.
The sampled files contain thirty separate 128-packet bursts about three
seconds apart. Their gaps are intentional sampling gaps, never NO-DATA or
missing packets to be interpolated. Directions start/end at different cycles;
pair only DATA on the identical observed bus cycle.

Measured official-driver steady target: 6656 ticks (13 frames) in 2044 paired
DATA packets; 97.41% within one tick of the modal offset. The startup session
settles at 13 frames plus 21 ticks. Its first TX DATA lead is 9216 ticks and
its observed TX lead spans [9216,12288). The device's adjacent packet SPHs
advance by 4095, 4096 or 4097 ticks, with anti-correlated quantisation. These
are measurements of this interface/session; the production common policy
uses 13 frames with best effort across V1–V3 and supported rates.

Run the offline measurement regressions with:

```sh
python3 -m unittest discover -s tools/motu -p 'test_*.py'
python3 tools/motu/sph_wire_capture.py \
  documentation/fixtures/MOTU/828mk3-48k/vendor-sampled-90s_host-to-device_ch33.csv \
  documentation/fixtures/MOTU/828mk3-48k/vendor-sampled-90s_device-to-host_ch34.csv
```

The continuous device-to-host file also feeds the actual RX observer and TX
producer in `MotuTxCaptureGolden.VendorStartupRxThroughRealProducer`; see
`tests/golden/tx/motu-828mk3/README.md`. Capture timing alone cannot establish
audible playback or real DMA/host scheduling behavior. Register session goldens
are separate future work requiring a simulated MOTU device; the original
`fixtures/`, `raw/` and startup choreography remain available outside Git.
