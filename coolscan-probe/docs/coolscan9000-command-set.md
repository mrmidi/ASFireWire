# CoolScan 9000 ED — SCSI-kommandosett (kartlagt fra SANE coolscan3)

Denne dokumentet kartlegger SCSI-kommandosettet en CoolScan-skanner bruker, basert
på en gjennomgang av SANE-backenden `coolscan3.c` (3191 linjer, GPL). Alle kommandoer
sendes som rå CDB-er gjennom `SBP2Session.sendSCSI(...)` i denne proben — det er den
eneste «limen» som mangler mellom ASFireWires SBP-2-transport og en fungerende skanner.

> **Kilde:** `sane-backends/backend/coolscan3.c`. Backenden bygger CDB-er med
> `cs3_parse_cmd(s, "hex hex …")` og fyller inn verdier med `cs3_pack_byte/word/long`.
> **Alle flerbyte-felter er big-endian** (SCSI-konvensjon) — kritisk for vår port.

---

## ⚠️ Tre funn som påvirker 9000 spesielt

1. **coolscan3 støtter IKKE LS-9000 eksplisitt.** Type-enumet stopper på `CS3_TYPE_LS8000`,
   og ukjent produktstreng gir `SANE_STATUS_UNSUPPORTED`. Produktmatchingen er rene
   strengsammenligninger på 16-tegns strenger (`"LS-8000 ED      "`).
   **9000 er nær identisk med 8000** (begge mellomformat, samme kommandofamilie), så
   porten legger til en `LS-9000`-gren som arver 8000-oppførsel. **Go/no-go-probens
   INQUIRY gir oss den eksakte produktstrengen 9000 rapporterer** — vi matcher på den.

2. **Digital ICE er ikke implementert i SANE.** Backenden leverer kun den rå
   **infrarøde 4. kanalen** (`n_colors = 4`, fargekode `0x09`). Selve støvfjerningen
   gjøres ikke her (VueScan gjør det i programvare). Vår MVP leverer IR-kanalen rå;
   ICE-algoritme er et separat, senere steg.

3. **Multi-sampling snittes host-side.** Ved `samples_per_scan > 1` leser backenden
   `xfer_len_in *= samples_per_scan` byte (N hele rammer) og **midler dem i programvare**.
   Skanneren leverer altså N rå pass; vi står for snittingen.

---

## Opcode-oversikt

| Opcode | CDB-lengde | Kommando | Retning | Bruk |
|--------|-----------|----------|---------|------|
| `0x00` | 6 | TEST UNIT READY | none | Status-polling (`scanner_ready`) |
| `0x03` | 6 | REQUEST SENSE | in | Hente status/feilbits |
| `0x12` | 6 | INQUIRY (std + EVPD-sider) | in | Identifikasjon + kapabilitetssider |
| `0x15` | 6 | MODE SELECT(6) | out | Sette base-oppløsningsenhet |
| `0x16` | 6 | RESERVE UNIT | none | Reservere skanneren |
| `0x17` | 6 | RELEASE UNIT | none | Frigi skanneren |
| `0x1a` | 6 | MODE SENSE(6) | in | Lese moduser |
| `0x1b` | 6 | SCAN | none | Starte skann (lister fargekanaler) |
| `0x24` | 10 | SET WINDOW | out | Skannevindu (per farge) — **den store** |
| `0x25` | 10 | GET WINDOW | in | Lese tilbake vindu/eksponering |
| `0x28` | 10 | READ(10) | in | Hente bildedata |
| `0x2a` | 10 | WRITE(10) | out | LUT-nedlasting + ramme-grenser |
| `0xc0` | 6 | (vendor) status/fase | — | Fase-sjekk |
| `0xc1` | 6 | (vendor) EXECUTE / sideinquiry | varierer | Trigge utførelse; lese Nikon-sider |
| `0xe0` | 10 | (vendor) SET | out | Fokus, autofokus, load/eject/reset |
| `0xe1` | 10 | (vendor) GET | in | Lese fokus m.m. |

---

## Konkrete CDB-er (verifisert mot coolscan3)

Alle eksempler er nøyaktige byte-sekvenser fra backenden. `‹word›`=2 byte BE,
`‹long›`=4 byte BE.

### Status / livssyklus
```
TEST UNIT READY   00 00 00 00 00 00                      (none)
RESERVE UNIT      16 00 00 00 00 00                      (none)
RELEASE UNIT      17 00 00 00 00 00                      (none)
EXECUTE (trigger) c1 00 00 00 00 00                      (none)   // etter set-param-kmd
LOAD medium       e0 00 d1 00 00 00 00 00 0d 00 + 13B    (out)
EJECT medium      e0 00 d0 00 00 00 00 00 0d 00 + 13B    (out)
RESET             e0 00 80 00 00 00 00 00 0d 00 + 13B    (out)
```

### Identifikasjon
```
INQUIRY (std)     12 00 00 00 ‹len› 00                   (in, len byte)
INQUIRY (EVPD)    12 01 ‹page› 00 ‹len› 00               (in)   // page 0xC1 = Nikon-kapabiliteter
```
`scanner_ready` = løkke av TEST UNIT READY (med REQUEST SENSE for statusbits) til
relevante bits er 0, 120 s timeout.

### MODE SELECT — sett base-oppløsningsenhet
```
15 10 00 00 14 00   (CDB: param-lengde 0x14=20)
+ param (20B): 00 00 00 00 08 00 00 00 00 00 00 00 00 01 03 06 00 00 ‹unit_dpi:word› 00 00
```

### Fokus
```
SET FOCUS    e0 00 c1 00 00 00 00 00 09 00 00 ‹focus:long› 00 00 00 00   (out)
READ FOCUS   e1 00 c1 00 00 00 00 00 0d 00                                (in, 13B)
             // focus = ((b1<<8|b2)<<16) | (b3<<8|b4)  fra svaret
AUTOFOCUS    e0 00 a0 00 00 00 00 00 09 00 00 ‹focusx:long› ‹focusy:long› (out)
```

### SET WINDOW (0x24) — 58-byte vindusdeskriptor, sendes **per farge**
CDB (10B): `24 00 00 00 00 00 00 00 3a 00` (byte8 `0x3a`=58 transfer; byte9 `0x80` for
LS40/4000/50/5000, ellers `0x00`). Deretter 58 byte:

| Offset | Felt | Verdi |
|--------|------|-------|
| 0–7 | header | `00 00 00 00 00 00 00 32` (0x32=50 = deskriptorlengde) |
| 8 | color id | `cs3_colors[color]` (R/G/B/IR = 1/2/3/9) |
| 9 | — | `00` |
| 10–11 | resx | ‹word› (device-enheter) |
| 12–13 | resy | ‹word› |
| 14–17 | x-offset | ‹long› |
| 18–21 | y-offset | ‹long› |
| 22–25 | width | ‹long› |
| 26–29 | height | ‹long› |
| 30–32 | brightness/contrast | `00 00 00` |
| 33 | image composition | `05` |
| 34 | bit-dybde | `real_depth` (8/14/16) |
| 35–47 | — | 13× `00` |
| 48 | multiread/ordering | `(samples_per_scan-1) << 4` |
| 49 | averaging + pos/neg | `0x80 | (negative ? 0 : 1)` |
| 50 | scan kind | `01` normal / `20` AE / `40` AE+WB |
| 51 | scanning mode | `02` single / `10` multi |
| 52 | color interleaving | `02` |
| 53 | (AE) | `ff` |
| 54–57 | eksponering | ‹long› (×10 ns) — eller `00 00 00 00` for IR |

### SCAN (0x1b) — etter at alle vinduer er satt
```
RGB    1b 00 00 00 03 00 01 02 03        (none)   // 3 farger + fargekoder
RGBI   1b 00 00 00 04 00 01 02 03 09     (none)   // 4 farger inkl. IR (0x09)
```

### GET WINDOW (0x25) — les tilbake eksponering, per farge
```
25 01 00 00 00 ‹color› 00 00 3a 00       (in, 58B)
// eksponering = bytes[54..57] som long
```

### READ bildedata (0x28)
```
28 00 00 00 00 00 ‹xfer:3B BE› 00        (in, xfer byte)
// xfer_len_in *= samples_per_scan før lesing (multi-sample = N rammer)
```

### LUT-nedlasting (0x2a, ved normal-skann) — per farge
```
2a 00 03 00 ‹color› 01 ‹2*n_lut:3B BE› 00 + LUT-data(‹word› per punkt)   (out)
```

### Ramme-grenser (0x2a, mellomformat multi-ramme — viktig for 8000/9000)
```
2a 00 88 00 00 03 ‹(4+n_frames*16):3B BE› 00 + grensedata               (out)
```

---

## Full skannesekvens (`cs3_scan`)

1. `scanner_ready` (vent til dokument klart)
2. **convert_options** — beregn geometri (host-side, ingen kommando)
3. **SET BOUNDARY** (`2a 00 88 …`) — ramme-grenser (mellomformat)
4. **SET FOCUS** (`e0 00 c1 …`)
5. `scanner_ready`
6. **SEND LUT** (`2a 00 03 …`, kun ved normal-skann)
7. **SET WINDOW** (`24 …`) — én gang per farge (3 eller 4)
8. **GET WINDOW** (`25 …`) — les eksponering tilbake
9. **SCAN** (`1b …`)
10. Løkke: **READ(10)** (`28 …`) til alle linjer er hentet → sett sammen til bilde

Etterbehandling host-side: multi-sample-snitting, bit-shift (`shift_bits`), evt.
LUT, og (senere) ICE-støvfjerning fra IR-kanalen → skriv TIFF.

---

## Hvordan dette mapper til vår probe

`SBP2Session.sendSCSI(cdb:direction:transferLength:outgoing:)` dekker alle mønstrene:
- 6/10-byte CDB → `cdb`-array
- data inn (INQUIRY/READ/GET WINDOW) → `direction: .fromTarget`, `transferLength: N`
- data ut (SET WINDOW/MODE SELECT/LUT/vendor SET) → `direction: .toTarget`, `outgoing: [...]`
- ingen data (TUR/SCAN/RESERVE/EXECUTE) → `direction: .none`

Se `Sources/CoolScanProbe/CoolScan.swift` for kommandobyggerne (skjelett, klart for
testing når go/no-go er bestått).

## Åpne spørsmål (krever maskinvare å avklare)
- Eksakt produktstreng 9000 rapporterer (→ modellmatching).
- Innhold i Nikon-kapabilitetsside `0xC1` på 9000 (maks oppløsning, grenser, fokusområde).
- Statusbit-tolkning fra REQUEST SENSE på 9000.
- Om 9000 trenger ekstra init utover 8000 (f.eks. medium-holder/adapter-deteksjon).

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
