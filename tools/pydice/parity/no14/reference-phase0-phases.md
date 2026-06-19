# Phase 0 Reference Parity Checklist

- Source log: `no14.txt`
- Filters: Config ROM skipped, initial IRM compare-verify skipped, Self-ID skipped, CycleStart skipped, PHY Resume skipped, WrResp skipped
- Session window: last `BusReset` before final `GLOBAL_ENABLE = 1` (063:1845:2949 → 069:2717:0647)
- Generation target: unknown (FireBug does not encode generation directly)
- Checklist items: 86

## Bus Reset
Summary: session begins at the last bus reset before final enable

- [ ] 001 `BusReset` `Bus Reset` `-` — session begins at the last bus reset before final `GLOBAL_ENABLE = 1`

## DICE Layout Discovery
Summary: read section layout: global=380B, tx_stride=70q, rx_stride=70q

- [ ] 002 `Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B read request
- [ ] 003 `BRresp` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B GLOBAL_OFF=10q (0x28B) | GLOBAL_SIZE=95q (0x17cB) | TX_OFF=105q (0x1a4B) | TX_SIZE=142q (0x238B) | RX_OFF=247q (0x3dcB) | RX_SIZE=282q (0x468B)
- [ ] 010 `Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B read request
- [ ] 011 `BRresp` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B GLOBAL_OFF=10q (0x28B) | GLOBAL_SIZE=95q (0x17cB) | TX_OFF=105q (0x1a4B) | TX_SIZE=142q (0x238B) | RX_OFF=247q (0x3dcB) | RX_SIZE=282q (0x468B)
- [ ] 016 `Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B read request
- [ ] 017 `BRresp` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B GLOBAL_OFF=10q (0x28B) | GLOBAL_SIZE=95q (0x17cB) | TX_OFF=105q (0x1a4B) | TX_SIZE=142q (0x238B) | RX_OFF=247q (0x3dcB) | RX_SIZE=282q (0x468B)

## Global State Read
Summary: read global state clock=R48000, Internal, notify=CLOCK_ACCEPTED, rate=48000 Hz

- [ ] 014 `Qread` `ffff.e000.007c` `GLOBAL_STATUS` `-` — read request
- [ ] 015 `QRresp` `ffff.e000.007c` `GLOBAL_STATUS` `0x00000201` — locked=True, R48000
- [ ] 028 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 029 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B partial(16B) OWNER=node 0xffc0 notify@0x000100000000 | NOTIFY=0x00000020 | NAME_HEAD='2orP'

## Owner Claim
Summary: owner claim CAS old=0xffff000000000000 new=0xffc0000100000000

- [ ] 004 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `104B` — 104B read request
- [ ] 005 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `104B` — 104B OWNER=No owner | NOTIFY=0x00000020 | CLOCK=R48000, Internal | ENABLE=False | STATUS=locked=True, R48000 | RATE=48000Hz
- [ ] 018 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 019 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B partial(16B) OWNER=No owner | NOTIFY=0x00000020 | NAME_HEAD='2orP'
- [ ] 020 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 021 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 022 `LockRq` `ffff.e000.0028` `GLOBAL_OWNER` `16B` — 16B CAS.old=0xffff000000000000 | CAS.new=0xffc0000100000000
- [ ] 023 `LockResp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 024 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 025 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=node 0xffc0 notify@0x000100000000

## Clock Select
Summary: write GLOBAL_CLOCK_SELECT = R48000, Internal

- [ ] 026 `Qwrite` `ffff.e000.0074` `GLOBAL_CLOCK_SELECT` `0x0000020c` — R48000, Internal

## Completion Wait
Summary: async write FW notification address = 0x00000020

- [ ] 027 `Qwrite` `0001.0000.0000` `FW notification address` `0x00000020` — 0x00000020

## Stream Discovery
Summary: discover streams (TX_SIZE) => TX channel 1, s400; RX channel 0

- [ ] 006 `Bread` `ffff.e000.01a4` `TX_NUMBER` `512B` — 512B read request
- [ ] 007 `BRresp` `ffff.e000.01a4` `TX_NUMBER` `512B` — 512B TX_COUNT=1 | TX_STRIDE=70q (0x118B)
- [ ] 008 `Bread` `ffff.e000.03dc` `RX_NUMBER` `512B` — 512B read request
- [ ] 009 `BRresp` `ffff.e000.03dc` `RX_NUMBER` `512B` — 512B RX_COUNT=1 | RX_STRIDE=70q (0x118B)
- [ ] 030 `Qread` `ffff.e000.01a4` `TX_NUMBER` `-` — read request
- [ ] 031 `QRresp` `ffff.e000.01a4` `TX_NUMBER` `0x00000001` — 1
- [ ] 032 `Qread` `ffff.e000.03dc` `RX_NUMBER` `-` — read request
- [ ] 033 `QRresp` `ffff.e000.03dc` `RX_NUMBER` `0x00000001` — 1
- [ ] 034 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 035 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 036 `Qread` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 037 `QRresp` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0x00000001` — channel 1
- [ ] 038 `Qread` `ffff.e000.01b0` `TX[0] audio channels` `-` — read request
- [ ] 039 `QRresp` `ffff.e000.01b0` `TX[0] audio channels` `0x00000010` — 16
- [ ] 040 `Qread` `ffff.e000.01b4` `TX[0] MIDI ports` `-` — read request
- [ ] 041 `QRresp` `ffff.e000.01b4` `TX[0] MIDI ports` `0x00000001` — 1
- [ ] 042 `Qread` `ffff.e000.01b8` `TX[0] speed` `-` — read request
- [ ] 043 `QRresp` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400
- [ ] 044 `Bread` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B read request
- [ ] 045 `BRresp` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B TX[0] ch 0: 'IP 1' | TX[0] ch 1: 'IP 2' | TX[0] ch 2: 'IP 3'
- [ ] 046 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 047 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 048 `Qread` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 049 `QRresp` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0x00000000` — channel 0
- [ ] 050 `Qread` `ffff.e000.03f0` `RX[0] MIDI ports` `-` — read request
- [ ] 051 `QRresp` `ffff.e000.03f0` `RX[0] MIDI ports` `0x00000001` — 1
- [ ] 052 `Qread` `ffff.e000.03e8` `RX[0] seq start` `-` — read request
- [ ] 053 `QRresp` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0
- [ ] 054 `Qread` `ffff.e000.03ec` `RX[0] audio channels` `-` — read request
- [ ] 055 `QRresp` `ffff.e000.03ec` `RX[0] audio channels` `0x00000008` — 8
- [ ] 056 `Bread` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B read request
- [ ] 057 `BRresp` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B RX[0] ch 0: 'Mon 1' | RX[0] ch 1: 'Mon 2'
- [ ] 078 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 079 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 082 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 083 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)

## IRM Reservation
Summary: IRM reservations 4 lock ops

- [ ] 058 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 059 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x00001333` — 4915
- [ ] 060 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 061 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0xfffffffe` — 0xfffffffe
- [ ] 062 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 063 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 064 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4915, new=4595 | → allocate 320 (0x140) units
- [ ] 065 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4915 units
- [ ] 066 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0xfffffffe, new=0x7ffffffe | → allocate channel 0
- [ ] 067 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0xfffffffe
- [ ] 068 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 069 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x000011f3` — 4595
- [ ] 070 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 071 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0x7ffffffe` — 0x7ffffffe
- [ ] 072 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 073 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 074 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4595, new=4019 | → allocate 576 (0x240) units
- [ ] 075 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4595 units
- [ ] 076 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0x7ffffffe, new=0x3ffffffe | → allocate channel 1
- [ ] 077 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0x7ffffffe

## RX Programming
Summary: program RX[0] = channel 0, seq=0

- [ ] 080 `Qwrite` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0x00000000` — channel 0
- [ ] 081 `Qwrite` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0

## TX Programming
Summary: program TX[0] = channel 1, speed=s400

- [ ] 084 `Qwrite` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0x00000001` — channel 1
- [ ] 085 `Qwrite` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400

## TCAT Extended Discovery
Summary: read 1 TCAT regions: e020.0000

- [ ] 012 `Bread` `ffff.e020.0000` `TCAT ext section header` `72B` — 72B read request
- [ ] 013 `BRresp` `ffff.e020.0000` `TCAT ext section header` `72B` — 72B TCAT_SECT_0_OFFSET: 19q (0x4cB) | TCAT_SECT_0_SIZE: 4q (0x10B) | TCAT_SECT_1_OFFSET: 23q (0x5cB) | TCAT_SECT_1_SIZE: 2q (0x8B)

## Enable
Summary: write GLOBAL_ENABLE = True

- [ ] 086 `Qwrite` `ffff.e000.0078` `GLOBAL_ENABLE` `0x00000001` — True
