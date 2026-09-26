# Converter chips in FireWire audio interfaces

Why this exists: a device's declared latency (`inputLatency1x` / `outputLatency1x` in its profile) includes the
A/D and D/A group delay, and that part is fixed by the chip. Knowing the chip lets a measured round trip be split
into converter delay (not removable) and transport/safety (ours to tune). See `TX_OWNERSHIP.md` §1e.

**Status of every entry: unverified.** Nothing below has been checked on a board. Use a datasheet's group delay
as a starting point to compare against a loopback measurement, never as a declared value on its own.

## Sources

- **Focusrite support e-mail**, quoted on Gearspace:
  https://gearspace.com/threads/focusrite-saffire-pro-24-vs-apogee-duet-converters-shoot-out.469949/post-6195320
  Pro 24 and Pro 24 DSP: TI PCM3168A. Pro 14, Pro 40, Liquid Saffire 56: Cirrus CS4272.
  The same post's author (not Focusrite) says the Apogee Duet uses the CS4272.
- **Community list**, Gearspace:
  https://gearspace.com/threads/anyone-here-got-the-forssell-mad2-ad-converter.1034635/post-11376511
  Compiled by forum users; entries are hearsay unless confirmed elsewhere. Its "Saffire Pro: CS4272" row does not
  name a model; for the Pro 24 the Focusrite e-mail above is the source.

## Devices this driver supports or has seen

| Device | A/D | D/A | Source | Measured |
|---|---|---|---|---|
| Focusrite Saffire Pro 24 / Pro 24 DSP | TI PCM3168A | TI PCM3168A | Focusrite e-mail | Loopback L ≈ 109–111 fr at 48 kHz; PCM3168A datasheet (SBAS452A) group delay ADC 27/fS + DAC 28/fS = 55 fr |
| Focusrite Saffire Pro 14 / Pro 40, Liquid Saffire 56 | Cirrus CS4272 | Cirrus CS4272 | Focusrite e-mail | — |
| Apogee Duet (FireWire) | Cirrus CS4272 (mic pre TI PGA2500, 1394 bridge Oxford OXFW971) | Cirrus CS4272 | Both posts (forum claim) | — |
| M-Audio FireWire 1814 | Ins 1–2 AKM AK5385A, ins 3–8 AKM AK5381 | AKM AK4358 | Community list | — |
| M-Audio ProjectMix | AKM AK5381 | AKM AK4358 | Community list | — |

The 1814 lists two A/D parts (inputs 1–2 and 3–8). The driver declares one input latency for all its inputs
(`MAudioSpecialProfile::RxReportedLatencyFrames`, input stream latency 0): CoreAudio has no per-channel latency,
only device and per-stream latency, so a difference could only be expressed by splitting the inputs into two
streams. Whether the two parts differ at all is not known yet: it needs their datasheets and a per-input
loopback measurement.

## Other FireWire interfaces in the community list

Kept for when one of these is brought up; same caveat.

| Device | Converters |
|---|---|
| Alesis io\|26 | CS5361 |
| Apogee Ensemble | CS4272 |
| Echo AudioFire 2 / 4 (newer; older) | AK4620B (AK4620A) |
| Echo AudioFire 8 | CS4272 |
| Echo AudioFire 8a, Pre8 | AK4620B |
| Echo AudioFire 12 (newer: FireWire port bevel down; older: bevel up) | AK4620B (CS4272) |
| Focusrite Saffire / Saffire LE | CS42428-CQ / CS42432-DMZ |
| Lexicon FW810S | CS4272-CZZ, CS42426 |
| Mackie Onyx FireWire card | A/D AK5384, D/A AK4528 |
| Mackie Onyx 400F / 1200F | A/D AK5385, D/A AK4358 |
| MOTU 828 | AK4321VF, CS4223-KS |
| MOTU 828mk2 | AK4528 ×8, AK4382 (main outs) |
| MOTU 828mk3 | CS5364 (8 in), CS5368, AK4358, AK4382 (main), AK4396 |
| MOTU 8pre | A/D AK5385, D/A AK4382 |
| MOTU 896HD | AK5385 |
| MOTU Traveler | A/D AK5385AVS (D/A unknown) |
| MOTU UltraLite | AK4620AVF |
| Metric Halo Mobile I/O 2882 | A/D AK5383, D/A AK4393 |
| Metric Halo ULN-2 | A/D AK5383, D/A AK4393 |
| Metric Halo ULN-8 / LIO-8 | A/D AK5394, D/A AK4395 |
| PreSonus FireBox | A/D AK5384, D/A AK4358 |
| PreSonus FireStudio / FirePod | AK5384, AK4358 (listed with "?") |
| PreSonus FireStudio Mobile | A/D PCM4204, D/A PCM4104 |
| RME Fireface 800 | A/D AK5385, D/A AK4395 (AK4396 since March 2005) |
| RME Fireface 400 | AK4620A |
| Roland/Edirol FA-66 | AK4385VT, CS5340 |
| Steinberg MR816 | A/D AK5385BVF, D/A AK4358VQ |
| TC Electronic Konnekt 24D / Live | AK4620B |
| TC Electronic Studio Konnekt 48 | D/A AK4359, AK4385; A/D AK5358, AK5359 |
| TC Electronic Impact Twin | AK4620 |
| Universal Audio Apollo | A/D AK5388EQ, D/A CS4398, headphones AK4480 |
