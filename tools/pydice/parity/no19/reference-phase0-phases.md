# Phase 0 Reference Parity Checklist

- Source log: `no19.txt`
- Filters: Config ROM included, initial IRM compare-verify skipped, Self-ID skipped, CycleStart skipped, PHY Resume skipped, WrResp skipped
- Session window: last `BusReset` before final `GLOBAL_ENABLE = 1` (049:4164:0139 → 055:4226:2044)
- Generation target: unknown (FireBug does not encode generation directly)
- Checklist items: 160

## Bus Reset
Summary: session begins at the last bus reset before final enable

- [ ] 001 `BusReset` `Bus Reset` `-` — session begins at the last bus reset before final `GLOBAL_ENABLE = 1`

## Config ROM Probe
Summary: probe 43 Config ROM reads

- [ ] 002 `Qread` `ffff.f000.0400` `ConfigROM +0x000` `-` — read request
- [ ] 003 `QRresp` `ffff.f000.0400` `ConfigROM +0x000` `0x0404a54b` — 0x0404a54b
- [ ] 004 `Qread` `ffff.f000.0404` `ConfigROM +0x004` `-` — read request
- [ ] 005 `QRresp` `ffff.f000.0404` `ConfigROM +0x004` `0x31333934` — 0x31333934 ('1394')
- [ ] 006 `Qread` `ffff.f000.0408` `ConfigROM +0x008` `-` — read request
- [ ] 007 `QRresp` `ffff.f000.0408` `ConfigROM +0x008` `0x0000b003` — 0x0000b003
- [ ] 008 `Qread` `ffff.f000.040c` `ConfigROM +0x00c` `-` — read request
- [ ] 009 `QRresp` `ffff.f000.040c` `ConfigROM +0x00c` `0x000a2702` — 0x000a2702
- [ ] 010 `Qread` `ffff.f000.0410` `ConfigROM +0x010` `-` — read request
- [ ] 011 `QRresp` `ffff.f000.0410` `ConfigROM +0x010` `0x00752966` — 0x00752966 ('\x00u)f')
- [ ] 012 `Qread` `ffff.f000.0400` `ConfigROM +0x000` `-` — read request
- [ ] 013 `QRresp` `ffff.f000.0400` `ConfigROM +0x000` `0x04040b5d` — 0x04040b5d
- [ ] 014 `Qread` `ffff.f000.0408` `ConfigROM +0x008` `-` — read request
- [ ] 015 `QRresp` `ffff.f000.0408` `ConfigROM +0x008` `0xe0ff8112` — 0xe0ff8112
- [ ] 016 `Qread` `ffff.f000.040c` `ConfigROM +0x00c` `-` — read request
- [ ] 017 `QRresp` `ffff.f000.040c` `ConfigROM +0x00c` `0x00130e04` — 0x00130e04
- [ ] 018 `Qread` `ffff.f000.0410` `ConfigROM +0x010` `-` — read request
- [ ] 019 `QRresp` `ffff.f000.0410` `ConfigROM +0x010` `0x02004713` — 0x02004713
- [ ] 020 `Qread` `ffff.f000.0414` `ConfigROM +0x014` `-` — read request
- [ ] 021 `QRresp` `ffff.f000.0414` `ConfigROM +0x014` `0x0006d223` — 0x0006d223
- [ ] 022 `Qread` `ffff.f000.0418` `ConfigROM +0x018` `-` — read request
- [ ] 023 `QRresp` `ffff.f000.0418` `ConfigROM +0x018` `0x0300130e` — 0x0300130e
- [ ] 024 `Qread` `ffff.f000.041c` `ConfigROM +0x01c` `-` — read request
- [ ] 025 `QRresp` `ffff.f000.041c` `ConfigROM +0x01c` `0x8100000a` — 0x8100000a
- [ ] 026 `Qread` `ffff.f000.0420` `ConfigROM +0x020` `-` — read request
- [ ] 027 `QRresp` `ffff.f000.0420` `ConfigROM +0x020` `0x17000008` — 0x17000008
- [ ] 028 `Qread` `ffff.f000.0424` `ConfigROM +0x024` `-` — read request
- [ ] 029 `QRresp` `ffff.f000.0424` `ConfigROM +0x024` `0x8100000e` — 0x8100000e
- [ ] 030 `Qread` `ffff.f000.0428` `ConfigROM +0x028` `-` — read request
- [ ] 031 `QRresp` `ffff.f000.0428` `ConfigROM +0x028` `0x0c0087c0` — 0x0c0087c0
- [ ] 032 `Qread` `ffff.f000.042c` `ConfigROM +0x02c` `-` — read request
- [ ] 033 `QRresp` `ffff.f000.042c` `ConfigROM +0x02c` `0xd1000001` — 0xd1000001
- [ ] 034 `Qread` `ffff.f000.0430` `ConfigROM +0x030` `-` — read request
- [ ] 035 `QRresp` `ffff.f000.0430` `ConfigROM +0x030` `0x0004d708` — 0x0004d708
- [ ] 036 `Qread` `ffff.f000.0434` `ConfigROM +0x034` `-` — read request
- [ ] 037 `QRresp` `ffff.f000.0434` `ConfigROM +0x034` `0x1200130e` — 0x1200130e
- [ ] 038 `Qread` `ffff.f000.0438` `ConfigROM +0x038` `-` — read request
- [ ] 039 `QRresp` `ffff.f000.0438` `ConfigROM +0x038` `0x13000001` — 0x13000001
- [ ] 040 `Qread` `ffff.f000.043c` `ConfigROM +0x03c` `-` — read request
- [ ] 041 `QRresp` `ffff.f000.043c` `ConfigROM +0x03c` `0x17000008` — 0x17000008
- [ ] 042 `Qread` `ffff.f000.0440` `ConfigROM +0x040` `-` — read request
- [ ] 043 `QRresp` `ffff.f000.0440` `ConfigROM +0x040` `0x8100000f` — 0x8100000f
- [ ] 044 `Qread` `ffff.f000.0444` `ConfigROM +0x044` `-` — read request
- [ ] 045 `QRresp` `ffff.f000.0444` `ConfigROM +0x044` `0x00056f3b` — 0x00056f3b
- [ ] 046 `Qread` `ffff.f000.0448` `ConfigROM +0x048` `-` — read request
- [ ] 047 `QRresp` `ffff.f000.0448` `ConfigROM +0x048` `0x00000000` — 0x00000000
- [ ] 048 `Qread` `ffff.f000.044c` `ConfigROM +0x04c` `-` — read request
- [ ] 049 `QRresp` `ffff.f000.044c` `ConfigROM +0x04c` `0x00000000` — 0x00000000
- [ ] 050 `Qread` `ffff.f000.0450` `ConfigROM +0x050` `-` — read request
- [ ] 051 `QRresp` `ffff.f000.0450` `ConfigROM +0x050` `0x466f6375` — 0x466f6375 ('Focu')
- [ ] 052 `Qread` `ffff.f000.0454` `ConfigROM +0x054` `-` — read request
- [ ] 053 `QRresp` `ffff.f000.0454` `ConfigROM +0x054` `0x73726974` — 0x73726974 ('srit')
- [ ] 054 `Qread` `ffff.f000.0458` `ConfigROM +0x058` `-` — read request
- [ ] 055 `QRresp` `ffff.f000.0458` `ConfigROM +0x058` `0x65000000` — 0x65000000 ('e')
- [ ] 056 `Qread` `ffff.f000.045c` `ConfigROM +0x05c` `-` — read request
- [ ] 057 `QRresp` `ffff.f000.045c` `ConfigROM +0x05c` `0x000712e5` — 0x000712e5
- [ ] 058 `Qread` `ffff.f000.0460` `ConfigROM +0x060` `-` — read request
- [ ] 059 `QRresp` `ffff.f000.0460` `ConfigROM +0x060` `0x00000000` — 0x00000000
- [ ] 060 `Qread` `ffff.f000.0464` `ConfigROM +0x064` `-` — read request
- [ ] 061 `QRresp` `ffff.f000.0464` `ConfigROM +0x064` `0x00000000` — 0x00000000
- [ ] 062 `Qread` `ffff.f000.0468` `ConfigROM +0x068` `-` — read request
- [ ] 063 `QRresp` `ffff.f000.0468` `ConfigROM +0x068` `0x53414646` — 0x53414646 ('SAFF')
- [ ] 064 `Qread` `ffff.f000.046c` `ConfigROM +0x06c` `-` — read request
- [ ] 065 `QRresp` `ffff.f000.046c` `ConfigROM +0x06c` `0x4952455f` — 0x4952455f ('IRE_')
- [ ] 066 `Qread` `ffff.f000.0470` `ConfigROM +0x070` `-` — read request
- [ ] 067 `QRresp` `ffff.f000.0470` `ConfigROM +0x070` `0x50524f5f` — 0x50524f5f ('PRO_')
- [ ] 068 `Qread` `ffff.f000.0474` `ConfigROM +0x074` `-` — read request
- [ ] 069 `QRresp` `ffff.f000.0474` `ConfigROM +0x074` `0x32344453` — 0x32344453 ('24DS')
- [ ] 070 `Qread` `ffff.f000.0478` `ConfigROM +0x078` `-` — read request
- [ ] 071 `QRresp` `ffff.f000.0478` `ConfigROM +0x078` `0x50000000` — 0x50000000 ('P')
- [ ] 072 `Qread` `ffff.f000.047c` `ConfigROM +0x07c` `-` — read request
- [ ] 073 `QRresp` `ffff.f000.047c` `ConfigROM +0x07c` `0x000712e5` — 0x000712e5
- [ ] 074 `Qread` `ffff.f000.0480` `ConfigROM +0x080` `-` — read request
- [ ] 075 `QRresp` `ffff.f000.0480` `ConfigROM +0x080` `0x00000000` — 0x00000000
- [ ] 076 `Qread` `ffff.f000.0484` `ConfigROM +0x084` `-` — read request
- [ ] 077 `QRresp` `ffff.f000.0484` `ConfigROM +0x084` `0x00000000` — 0x00000000
- [ ] 078 `Qread` `ffff.f000.0488` `ConfigROM +0x088` `-` — read request
- [ ] 079 `QRresp` `ffff.f000.0488` `ConfigROM +0x088` `0x53414646` — 0x53414646 ('SAFF')
- [ ] 080 `Qread` `ffff.f000.048c` `ConfigROM +0x08c` `-` — read request
- [ ] 081 `QRresp` `ffff.f000.048c` `ConfigROM +0x08c` `0x4952455f` — 0x4952455f ('IRE_')
- [ ] 082 `Qread` `ffff.f000.0490` `ConfigROM +0x090` `-` — read request
- [ ] 083 `QRresp` `ffff.f000.0490` `ConfigROM +0x090` `0x50524f5f` — 0x50524f5f ('PRO_')
- [ ] 084 `Qread` `ffff.f000.0494` `ConfigROM +0x094` `-` — read request
- [ ] 085 `QRresp` `ffff.f000.0494` `ConfigROM +0x094` `0x32344453` — 0x32344453 ('24DS')
- [ ] 086 `Qread` `ffff.f000.0498` `ConfigROM +0x098` `-` — read request
- [ ] 087 `QRresp` `ffff.f000.0498` `ConfigROM +0x098` `0x50000000` — 0x50000000 ('P')

## DICE Layout Discovery
Summary: read section layout: global=380B, tx_stride=70q, rx_stride=70q

- [ ] 090 `Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B read request
- [ ] 091 `BRresp` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B` — 40B GLOBAL_OFF=10q (0x28B) | GLOBAL_SIZE=95q (0x17cB) | TX_OFF=105q (0x1a4B) | TX_SIZE=142q (0x238B) | RX_OFF=247q (0x3dcB) | RX_SIZE=282q (0x468B)

## Global State Read
Summary: read global state clock=R48000, Internal, notify=CLOCK_ACCEPTED, rate=48000 Hz

- [ ] 102 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 103 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B OWNER=node 0xffc0 notify@0x000100000000 | NOTIFY=0x00000020 | CLOCK=R48000, Internal | ENABLE=False | STATUS=locked=True, R48000 | RATE=48000Hz

## Owner Claim
Summary: owner claim CAS old=0xffff000000000000 new=0xffc0000100000000

- [ ] 092 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B read request
- [ ] 093 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `380B` — 380B OWNER=No owner | NOTIFY=0x00000020 | CLOCK=R48000, Internal | ENABLE=False | STATUS=locked=True, R48000 | RATE=48000Hz
- [ ] 094 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 095 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 096 `LockRq` `ffff.e000.0028` `GLOBAL_OWNER` `16B` — 16B CAS.old=0xffff000000000000 | CAS.new=0xffc0000100000000
- [ ] 097 `LockResp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=No owner
- [ ] 098 `Bread` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B read request
- [ ] 099 `BRresp` `ffff.e000.0028` `GLOBAL_OWNER` `8B` — 8B OWNER=node 0xffc0 notify@0x000100000000

## Clock Select
Summary: write GLOBAL_CLOCK_SELECT = R48000, Internal

- [ ] 100 `Qwrite` `ffff.e000.0074` `GLOBAL_CLOCK_SELECT` `0x0000020c` — R48000, Internal

## Completion Wait
Summary: async write FW notification address = 0x00000020

- [ ] 101 `Qwrite` `0001.0000.0000` `FW notification address` `0x00000020` — 0x00000020

## Stream Discovery
Summary: discover streams (TX_SIZE) => TX channel 1, s400; RX channel 0

- [ ] 088 `Qread` `ffff.e000.0054` `ffff.e000.0054` `-` — read request
- [ ] 089 `QRresp` `ffff.e000.0054` `ffff.e000.0054` `0x00000000` — 0x00000000
- [ ] 104 `Qread` `ffff.e000.01a4` `TX_NUMBER` `-` — read request
- [ ] 105 `QRresp` `ffff.e000.01a4` `TX_NUMBER` `0x00000001` — 1
- [ ] 106 `Qread` `ffff.e000.03dc` `RX_NUMBER` `-` — read request
- [ ] 107 `QRresp` `ffff.e000.03dc` `RX_NUMBER` `0x00000001` — 1
- [ ] 108 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 109 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 110 `Qread` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 111 `QRresp` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0xffffffff` — unused (-1)
- [ ] 112 `Qread` `ffff.e000.01b0` `TX[0] audio channels` `-` — read request
- [ ] 113 `QRresp` `ffff.e000.01b0` `TX[0] audio channels` `0x00000010` — 16
- [ ] 114 `Qread` `ffff.e000.01b4` `TX[0] MIDI ports` `-` — read request
- [ ] 115 `QRresp` `ffff.e000.01b4` `TX[0] MIDI ports` `0x00000001` — 1
- [ ] 116 `Qread` `ffff.e000.01b8` `TX[0] speed` `-` — read request
- [ ] 117 `QRresp` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400
- [ ] 118 `Bread` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B read request
- [ ] 119 `BRresp` `ffff.e000.01bc` `TX[0] channel names` `256B` — 256B TX[0] ch 0: 'IP 1' | TX[0] ch 1: 'IP 2' | TX[0] ch 2: 'IP 3' | TX[0] ch 3: 'IP 4'
- [ ] 120 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 121 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 122 `Qread` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `-` — read request
- [ ] 123 `QRresp` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0xffffffff` — unused (-1)
- [ ] 124 `Qread` `ffff.e000.03f0` `RX[0] MIDI ports` `-` — read request
- [ ] 125 `QRresp` `ffff.e000.03f0` `RX[0] MIDI ports` `0x00000001` — 1
- [ ] 126 `Qread` `ffff.e000.03e8` `RX[0] seq start` `-` — read request
- [ ] 127 `QRresp` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0
- [ ] 128 `Qread` `ffff.e000.03ec` `RX[0] audio channels` `-` — read request
- [ ] 129 `QRresp` `ffff.e000.03ec` `RX[0] audio channels` `0x00000008` — 8
- [ ] 130 `Bread` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B read request
- [ ] 131 `BRresp` `ffff.e000.03f4` `RX[0] channel names` `256B` — 256B RX[0] ch 0: 'Mon 1' | RX[0] ch 1: 'Mon 2' | RX[0] ch 2: 'Line 3' | RX[0] ch 3: 'Line 4'
- [ ] 142 `Qread` `ffff.e000.03e0` `RX_SIZE` `-` — read request
- [ ] 143 `QRresp` `ffff.e000.03e0` `RX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)
- [ ] 156 `Qread` `ffff.e000.01a8` `TX_SIZE` `-` — read request
- [ ] 157 `QRresp` `ffff.e000.01a8` `TX_SIZE` `0x00000046` — 70 quadlets (0x118 bytes)

## IRM Reservation
Summary: IRM reservations 4 lock ops

- [ ] 132 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 133 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x00001333` — 4915
- [ ] 134 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 135 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0xfffffffe` — 0xfffffffe
- [ ] 136 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 137 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 138 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4915, new=4595 | → allocate 320 (0x140) units
- [ ] 139 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4915 units
- [ ] 140 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0xfffffffe, new=0x7ffffffe | → allocate channel 0
- [ ] 141 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0xfffffffe
- [ ] 146 `Qread` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `-` — read request
- [ ] 147 `QRresp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `0x000011f3` — 4595
- [ ] 148 `Qread` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `-` — read request
- [ ] 149 `QRresp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `0x7ffffffe` — 0x7ffffffe
- [ ] 150 `Qread` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `-` — read request
- [ ] 151 `QRresp` `ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `0xffffffff` — 0xffffffff
- [ ] 152 `LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B` — 8B IRM_BANDWIDTH_AVAILABLE: old=4595, new=4019 | → allocate 576 (0x240) units
- [ ] 153 `LockResp` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `4B` — 4B BW=4595 units
- [ ] 154 `LockRq` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `8B` — 8B IRM_CHANNELS_AVAILABLE_HI: old=0x7ffffffe, new=0x3ffffffe | → allocate channel 1
- [ ] 155 `LockResp` `ffff.f000.0224` `IRM_CHANNELS_AVAILABLE_HI` `4B` — 4B HI=0x7ffffffe

## RX Programming
Summary: program RX[0] = channel 0, seq=0

- [ ] 144 `Qwrite` `ffff.e000.03e4` `RX[0] ISOCHRONOUS channel` `0x00000000` — channel 0
- [ ] 145 `Qwrite` `ffff.e000.03e8` `RX[0] seq start` `0x00000000` — 0

## TX Programming
Summary: program TX[0] = channel 1, speed=s400

- [ ] 158 `Qwrite` `ffff.e000.01ac` `TX[0] ISOCHRONOUS channel` `0x00000001` — channel 1
- [ ] 159 `Qwrite` `ffff.e000.01b8` `TX[0] speed` `0x00000002` — s400

## Enable
Summary: write GLOBAL_ENABLE = True

- [ ] 160 `Qwrite` `ffff.e000.0078` `GLOBAL_ENABLE` `0x00000001` — True
