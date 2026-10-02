# AV/C device targets

The devices the rebuilt AV/C stack should serve. Built 2026-09-27 from:
- Linux `sound/firewire/bebob/bebob.c` `bebob_id_table` (BeBoB) and `oxfw/oxfw.c` `oxfw_id_table` (Oxford);
- FFADO 2.5.0 `configuration` (entries with `driver = BEBOB / OXFORD / GENERICAVC`);
- our catalog on `main` (`ASFWDriver/DeviceProfiles/Audio/Definitions/`).

IDs are 24-bit vendor OUI + model ID from the Config ROM. "Linux spec" is the per-device quirk
struct in `bebob.c`; `spec_normal` means no clock/meter quirk. Status: **S** = supported in ASFW,
**R** = recognized but not supported, blank = not in our catalog. **Desk** = the user has one.

## What counts as AV/C here

AV/C = the unit directory says 1394TA (specifier `0x00a02d`) + AV/C (version `0x010001`) and the device
is driven through FCP. Two chip families:

- **Oxford** OXFW970 / OXFW971: plain AV/C (music + audio subunits). No vendor extensions.
- **BridgeCo BeBoB** DM1000 / DM1100 / DM1500 (`bebob.c:10`): AV/C plus BridgeCo extended PLUG INFO (`02 C0`)
  and the `0x2F` stream-format command.

Not AV/C, so out of scope for this list:
- **DICE / TCAT:** register-based.
- **Echo Fireworks:** EFC over async at `0xECC0'0000'0000`. Its unit version is `0x010000`, not AV/C
  (`fireworks.c:304`, `fireworks_transaction.c:25-34`). FFADO can tunnel EFC in AV/C (`EfcOverAVCCmd`),
  but Linux never does.
- **MOTU, RME, Digidesign 00x, TASCAM FW-1884/1082/FE-8.**

## Oxford (Linux `oxfw.c`)

| Device | Vendor | Model | Chip | Notes | Status |
|---|---|---|---|---|---|
| Apogee Duet FireWire | `0x0003db` | `0x01dddd` | OXFW971 | Apogee vendor commands on top. **Desk.** | S |
| Mackie Onyx-i series (earlier units) | `0x000ff2` | `0x081216` | OXFW971 | Linux matches any LOUD unit that is AV/C (`oxfw.c:355-369`) | S |
| Mackie Onyx 1640i (Oxford run) | `0x000ff2` | `0x001640` | OXFW971 | Linux quirks: ignore NO-INFO packets | R |
| Mackie Onyx Satellite | `0x000ff2` | `0x00200f` | OXFW970 | SYT always `0xffff`; wrong DBS quirk | |
| Tapco Link.FireWire 4x6 | `0x000ff2` | `0x000460` | OXFW970 | SYT always `0xffff` | |
| Mackie d.2 pro / d.4 pro, U.420, U.420d | `0x000ff2` | unknown | OXFW971 | Covered by the LOUD wildcard; no IDs known | |
| Griffin FireWave | `0x001292` | `0x00f970` | OXFW970 | 5.1 speaker interface; Linux spec `griffin_firewave` | |
| LaCie FireWire Speakers | `0x00d04b` | `0x00f970` | OXFW970 | Linux spec `lacie_speakers` | |
| Miglia HarmonyAudio (HA02) | `0x0030e0` | `0x00f970` | OXFW970 | Vendor ID is Oxford's own ASIC default | |
| Behringer F-Control Audio 202 (FCA202) | `0x001564` | `0x00fc22` | OXFW970 | SYT not reliable | |
| TASCAM FireOne | `0x00022e` | `0x800007` | OXFW971 | | |
| Stanton SCS.1m | `0x001260` | `0x001000` | OXFW971 | FFADO: `GENERICAVC` | |
| Stanton SCS.1d | `0x001260` | `0x002000` | OXFW971 | | |

## BeBoB, no vendor quirks (Linux `spec_normal`)

The generic BeBoB path (phase 5, B1) should cover these with no per-device code.

| Device | Vendor | Model | Notes | Status |
|---|---|---|---|---|
| Edirol FA-66 | `0x0040ab` | `0x00010049` | FFADO mixer `EdirolFa66Control` | |
| Edirol FA-101 | `0x0040ab` | `0x00010048` | FFADO mixer `EdirolFa101Control` | |
| PreSonus FireBox | `0x000a92` | `0x00010000` | Shares OUI with the DICE FireStudio | |
| PreSonus FirePod / FP10 | `0x000a92` | `0x00010066` | | |
| PreSonus Inspire 1394 | `0x000a92` | `0x00010001` | | |
| BridgeCo RD Audio1 | `0x0007f5` | `0x00010048` | Reference design | |
| BridgeCo Audio5 | `0x0007f5` | `0x00010049` | Reference design | |
| Mackie Onyx 1220/1620/1640 FireWire I/O card | `0x000ff2` | `0x00010065` | FFADO gives vendor `0x00000f` | |
| Mackie d.2 FireWire card | `0x000ff2` | `0x00010067` | DM1000; same model ID as the TASCAM IF-FW/DM | |
| TASCAM IF-FW/DM | `0x00022e` | `0x00010067` | | |
| Stanton ScratchAmp | `0x001260` | `0x00000001` | | |
| Behringer XENIX UFX 1204 | `0x001564` | `0x00001204` | | |
| Behringer XENIX UFX 1604 | `0x001564` | `0x00001604` | | |
| Behringer X32 (X-UF card) | `0x001564` | `0x00000006` | | |
| Behringer F-Control Audio 1616 | `0x001564` | `0x001616` | | |
| Behringer F-Control Audio 610 | `0x001564` | `0x000610` | | |
| Apogee Rosetta 200/400, DA/AD/DD-16X (X-FireWire card) | `0x0003db` | `0x00010048` | | |
| Apogee Ensemble | `0x0003db` | `0x01eeee` | | |
| ESI Quatafire 610 | `0x000f1b` | `0x00010064` | FFADO adds a variant, model `0x00000210` | |
| CME Matrix K FW | `0x00000a` | `0x00030000` | | |
| Phonic Helix Board 12 / 18 / 24 FireWire MkII | `0x001496` | `0x00050000` / `0x00060000` / `0x00070000` | | |
| Phonic FireFly 808 FireWire | `0x001496` | `0x00080000` | | |
| Phonic FireFly 202/302/808 Universal, Helix Board Universal | `0x001496` | `0x00000000` | One model ID for many units | |
| Lynx Aurora 8/16 (LT-FW) | `0x0019e5` | `0x00000001` | | |
| ICON FireXon | `0x001a9e` | `0x00000001` | | |
| Prism Sound Orpheus | `0x001198` | `0x00010048` | Start sequence in `tmp/orpheus-fork` (phase 5, B4) | |
| Prism Sound ADA-8XR | `0x001198` | `0x0000ada8` | | |
| TerraTec EWS MIC2 / MIC8 | `0x000aac` | `0x00000005` | | |
| TerraTec Aureon 7.1 FireWire; Acoustic Reality eAR Master One, Eroica, Figaro, Ciaccona | `0x000aac` | `0x00000002` | | |
| M-Audio ProFire Lightbridge | `0x000d6c` | `0x000100a1` | | |
| Digidesign Mbox 2 Pro | `0x00a07e` | `0x0000a9` | | |
| Toneweal FW66 | `0x002327` | `0x020002` | | |

## BeBoB with vendor quirks (Linux has a spec struct)

These stream through the generic path. The quirk is clock-source and/or meter access through vendor commands.

| Device | Vendor | Model | Linux spec | Status |
|---|---|---|---|---|
| TerraTec Phase 88 Rack FW | `0x000aac` | `0x00000003` | `phase88_rack_spec`. **Desk.** | S |
| TerraTec Phase 24 FW | `0x000aac` | `0x00000004` | `yamaha_terratec_spec` | |
| TerraTec Phase X24 FW | `0x000aac` | `0x00000007` | `yamaha_terratec_spec` | |
| Yamaha GO44 | `0x00a0de` | `0x0010000b` | `yamaha_terratec_spec` | |
| Yamaha GO46 | `0x00a0de` | `0x0010000c` | `yamaha_terratec_spec` | |
| Focusrite Saffire Pro 26 I/O | `0x00130e` | `0x00000003` | `saffirepro_26_spec` | |
| Focusrite Saffire Pro 10 I/O | `0x00130e` | `0x000006` | `saffirepro_10_spec` | |
| Focusrite Saffire | `0x00130e` | `0x00000000` | `saffire_spec`. Rate is generic BeBoB AV/C (`bebob_focusrite.c:292-295`); clock source and meters are Focusrite registers at `0x0001'0000'0000` (`:16`, `:216`, `:57`) | |
| Focusrite Saffire LE | `0x00130e` | `0x00000000` | `saffire_le_spec`: same model ID. Linux picks it by the model name "SaffireLE" (`bebob.c:143`); only the meter layout differs | |
| M-Audio FireWire 410 | `0x0007f5` | `0x00010046` | `maudio_fw410_spec`; vendor field is BridgeCo | |
| M-Audio FireWire 410 (bootloader) | `0x0007f5` | `0x00010058` | none: needs firmware upload first | |
| M-Audio FireWire Audiophile | `0x000d6c` | `0x00010060` | `maudio_audiophile_spec` (same ID as the bootloader) | |
| M-Audio FireWire Solo | `0x000d6c` | `0x00010062` | `maudio_solo_spec` | |
| M-Audio Ozonic | `0x000d6c` | `0x0000000a` | `maudio_ozonic_spec` | |
| M-Audio NRV10 | `0x000d6c` | `0x00010081` | `maudio_nrv10_spec` | |

## BeBoB, M-Audio special firmware (restricted AV/C)

This firmware freezes on AV/C it doesn't understand (`AVC_DEVICE_HAZARDS.md` H1). Only the exact frames in
`AVCCommandFilter.hpp` may be sent, so there's no generic discovery. The graph comes from the catalog.

| Device | Vendor | Model | Notes | Status |
|---|---|---|---|---|
| M-Audio FireWire 1814 | `0x000d6c` | `0x00010071` | **Desk.** Bootloader identity is `0x00010070` | S |
| M-Audio ProjectMix I/O | `0x000d6c` | `0x00010091` | | S |

## Known to Linux, IDs unknown (`bebob.c` "able to be supported")

Apogee Mini-ME, Apogee Mini-DAC, Cakewalk Sonar Power Studio 66, CME UF400e, ESI Quatafire XL,
Infrasonic DewX, Infrasonic Windy6, Mackie Digital X Bus x.200 / x.400, Rolf Spuler FireWire Guitar.
The generic path should pick these up by capability, with no catalog row, once one appears.

## Counts

- Oxford: 13 rows. 2 supported (Duet, Onyx-i), 1 recognized.
- BeBoB `spec_normal`: 32 rows. None supported yet; this is the generic path's target.
- BeBoB with quirks: 15 rows. 1 supported (Phase 88).
- M-Audio special: 2 rows, both supported.

## Open points

- **Mackie vendor ID for the BeBoB cards:** Linux says `0x000ff2`, FFADO says `0x00000f`. The first real ROM
  settles it.
- **Model `0x00010067` is used by both the Mackie d.2 and the TASCAM IF-FW/DM.** Tell them apart by vendor ID.
- **Model `0x00000000` is both the Focusrite Saffire and the Saffire LE.** Linux tells them apart by the model
  name "SaffireLE" (`bebob.c:143`), so vendor + model is not enough here; the ROM name must be read.
- **Model `0x00010048` is used by the Edirol FA-101, BridgeCo RD Audio1, the Apogee X-FireWire card and the
  Prism Orpheus** (the BridgeCo reference-design default). Always match vendor + model, never model alone.
- **Which of these answer the music subunit status descriptor** (Apple's path) is unknown. It's measured in
  phase 4 on the Duet and the Phase 88 first.
