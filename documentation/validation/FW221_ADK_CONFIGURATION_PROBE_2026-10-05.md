# FW-221: initial ADK configuration-probe runtime evidence

Runtime: macOS 26.6.2 (25G83), lab dext 1.2/4, source e0b53e9f.
The user activated the lab and confirmed smooth idle Audio MIDI Setup changes,
including 48 kHz / 16 channels and 192 kHz / 8 channels.

The agent then used a silent IOProc against the exact virtual probe UID.
No physical endpoint was changed. The probe's original 192 kHz setting was
restored after both sweeps.

| Requested rate | Nominal-property sweep | Physical-stream-format sweep | Channels each direction | ZTS period |
|---|---|---|---|---|
| 48000 | Passed | Passed | 16 | 12288 |
| 96000 | Passed | Passed | 12 | 24576 |
| 44100 | Passed | Passed | 16 | 12288 |
| 32000 | Passed | Passed | 16 | 12288 |
| 88200 | Passed | Passed | 12 | 24576 |
| 176400 | Passed | Passed | 8 | 49152 |
| 192000 | Passed | Passed | 8 | 49152 |
| Return to 48000 | Passed | Passed | 16 | 12288 |

Both sweeps used one Start call, retaining the original IOProc throughout.
Each requested configuration matched nominal rate, both virtual stream rates,
both channel counts and ZTS period, then delivered at least 20 more callbacks.
All property requests returned success. Device ID 123, output ID 124 and input
ID 125 stayed fixed. Restoration to 192 kHz was confirmed with resumed IO.

A separate 192 kHz clock watch measured 191999.993 Hz, worst residual 0.53
frames, zero backward jumps and zero forward re-anchors. The five-second IO
probe observed 2029 callbacks of 512 frames; median interval 2666.5 microseconds.

Limits: this is synthetic, silent HAL behavior, not FireWire or physical
continuity. Internal stage logs did not appear in persisted unified logging;
callback-stage ordering has not been independently reconstructed. Failed
projection, reset/unplug, competing requests and abort injection remain untested
at runtime. An attempted virtual-format layout request was rejected; virtual
format is the host conversion setting, while the physical-format sweep passed.
