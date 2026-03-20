# Phase 0 Reference Parity Checklist

- Source log: `ref-full.txt`
- Filters: Config ROM skipped, initial IRM compare-verify skipped, Self-ID skipped, CycleStart skipped, PHY Resume skipped, WrResp skipped
- Session window: last `BusReset` before final `GLOBAL_ENABLE = 1` (074:3832:2172 → 076:5377:2584)
- Generation target: unknown (FireBug does not encode generation directly)
- Checklist items: 74

## Bus Reset
Summary: session begins at the last bus reset before final enable

- [ ] 001 `BusReset` `Bus Reset` `-` — session begins at the last bus reset before final `GLOBAL_ENABLE = 1`

## DICE Layout Discovery
Summary: read section layout: global=380B, tx_stride=70q, rx_stride=70q

- [ ] 004 `Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B read request
- [ ] 005 `BRresp` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B GLOBAL_OFF=10q (0x28B) | GLOBAL_SIZE=95q (0x17cB) | TX_OFF=105q (0x1a4B) | TX_SIZE=142q (0x238B) | RX_OFF=247q (0x3dcB) | RX_SIZE=282q (0x468B)

## Global State Read
Summary: read global state clock=R48000, Internal, notify=CLOCK_ACCEPTED, rate=48000 Hz

- [ ] 002 `Qread` `ffff.e000.007c` `GLOBAL_STATUS` `-` — read request
- [ ] 003 `QRresp` `ffff.e000.007c` `GLOBAL_STATUS` `0x00000201` — locked=True, R48000
- [ ] 016 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 017 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B OWNER=node 0xffc0 notify@0x000100000000 | NOTIFY=0x00000020 | CLOCK=R48000, Internal | ENABLE=False | STATUS=locked=True, R48000 | RATE=48000Hz

## Owner Claim
Summary: owner claim CAS old=0xffff000000000000 new=0xffc0000100000000

- [ ] 006 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 007 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B OWNER=No owner | NOTIFY=0x00000010 | CLOCK=R48000, Internal | ENABLE=False | STATUS=locked=True, R48000 | RATE=48000Hz
- [ ] 008 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 009 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 010 `LockRq` `ffff.e000.0028` `GLOBAL_OWNER` `16B` — 16B CAS.old=0xffff000000000000 | CAS.new=0xffc0000100000000
- [ ] 011 `LockResp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 012 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 013 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=node 0xffc0 notify@0x000100000000

## Clock Select
Summary: write GLOBAL_CLOCK_SELECT = R48000, Internal

- [ ] 014 `Qwrite` `ffff.e000.0074` `GLOBAL_CLOCK_SELECT` `0x0000020c` — R48000, Internal

## Completion Wait
Summary: async write FW notification address = 0x00000020

- [ ] 015 `Qwrite` `0001.0000.0000` `FW notification address` `0x00000020` — 0x00000020

## Stream Discovery
Summary: discover streams (TX_SIZE) => TX channel 1, s400; RX channel 0

- [ ] 018 `Qread` `ffff.e000.01a4` `TX_NUMBER` `-` — read request
- [ ] 019 `QRresp` `ffff.e000.01a4` `TX_NUMBER` `0x00000001` — 1
- [ ] 020 `Qread` `ffff.e000.03dc` `RX_NUMBER` `-` — read request
- [ ] 021 `QRresp` `ffff.e000.03dc` `RX_NUMBER` `0x00000001` — 1
- [ ] 022 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 023 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 024 `Qread` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 025 `QRresp` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0x00000001` — channel 1
- [ ] 026 `Qread` `ffff.e000.01b0` `TX[0] audio channels` `-` — read request
- [ ] 027 `QRresp` `ffff.e000.01b0` `TX[0] audio channels` `0x00000010` — 16
- [ ] 028 `Qread` `ffff.e000.01b4` `TX[0] MIDI ports` `-` — read request
- [ ] 029 `QRresp` `ffff.e000.01b4` `TX[0] MIDI ports` `0x00000001` — 1
- [ ] 030 `Qread` `ffff.e000.01b8` `TX[0] speed` `-` — read request
- [ ] 031 `QRresp` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400
- [ ] 032 `Bread` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B read request
- [ ] 033 `BRresp` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B TX[0] ch 0: 'IP 1' | TX[0] ch 1: 'IP 2' | TX[0] ch 2: 'IP 3' | TX[0] ch 3: 'IP 4'
- [ ] 034 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 035 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 036 `Qread` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 037 `QRresp` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0x00000000` — channel 0
- [ ] 038 `Qread` `ffff.e000.03f0` `RX[0] MIDI ports` `-` — read request
- [ ] 039 `QRresp` `ffff.e000.03f0` `RX[0] MIDI ports` `0x00000001` — 1
- [ ] 040 `Qread` `ffff.e000.03e8` `RX[0] seq start` `-` — read request
- [ ] 041 `QRresp` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0
- [ ] 042 `Qread` `ffff.e000.03ec` `RX[0] audio channels` `-` — read request
- [ ] 043 `QRresp` `ffff.e000.03ec` `RX[0] audio channels` `0x00000008` — 8
- [ ] 044 `Bread` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B read request
- [ ] 045 `BRresp` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B RX[0] ch 0: 'Mon 1' | RX[0] ch 1: 'Mon 2' | RX[0] ch 2: 'Line 3' | RX[0] ch 3: 'Line 4'
- [ ] 056 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 057 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 070 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 071 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)

## IRM Reservation
Summary: IRM reservations 5 lock ops

- [ ] 046 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 047 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x00001333` — 4915
- [ ] 048 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 049 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0xfffffffe` — 0xfffffffe
- [ ] 050 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 051 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 052 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4915, new=4595 | → allocate 320 (0x140) units
- [ ] 053 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4915 units
- [ ] 054 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0xfffffffe, new=0x7ffffffe | → allocate channel 0
- [ ] 055 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0xfffffffe
- [ ] 060 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 061 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x000011f3` — 4595
- [ ] 062 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 063 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0x7ffffffe` — 0x7ffffffe
- [ ] 064 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 065 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 066 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4595, new=4019 | → allocate 576 (0x240) units
- [ ] 067 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4595 units
- [ ] 068 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0x7ffffffe, new=0x3ffffffe | → allocate channel 1
- [ ] 069 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0x7ffffffe

## RX Programming
Summary: program RX[0] = channel 0, seq=0

- [ ] 058 `Qwrite` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0x00000000` — channel 0
- [ ] 059 `Qwrite` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0

## TX Programming
Summary: program TX[0] = channel 1, speed=s400

- [ ] 072 `Qwrite` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0x00000001` — channel 1
- [ ] 073 `Qwrite` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400

## Enable
Summary: write GLOBAL_ENABLE = True

- [ ] 074 `Qwrite` `ffff.e000.0078` `GLOBAL_ENABLE` `0x00000001` — True
