# CoolScanProbe

«Proof of life» for å drive en **Nikon CoolScan 9000 ED** (FireWire / SBP-2) på
**macOS Tahoe** via [ASFireWire](https://github.com/mrmidi/ASFireWire)-stacken.

Verktøyet snakker med ASFireWire-dext-en gjennom dens DriverKit user client og
gjør hele kjeden ende-til-ende:

```
åpne dext → enumerere bussen → finne SBP-2-target → login → INQUIRY → TEST UNIT READY
```

En vellykket `INQUIRY` som rapporterer `Nikon … LS-9000` er **go/no-go-punktet**
for hele prosjektet. Da er transportlaget (OHCI → async → SBP-2 → SCSI-passthrough)
bevist, og alt videre er å porte CoolScan-kommandosettet (referanse:
[SANE coolscan3](http://www.sane-project.org/man/sane-coolscan3.5.html)) oppå
`SBP2Session.sendSCSI(...)`.

## Hvordan det henger sammen med ASFireWire

Verktøyet rører **ikke** driveren — det konsumerer bare det generiske
SCSI-passthrough-API-et som allerede finnes i `ASFWDriver/UserClient/Handlers/SBP2Handler`.
Selector-numre, wire-formater og service-navn er speilet direkte fra ASFireWire-kilden:

| Steg | ASFireWire-selector | Kilde |
|------|--------------------|-------|
| Enumerere enheter | 16 `GetDiscoveredDevices` | `DeviceDiscoveryWireFormats.hpp` |
| Opprette sesjon | 53 `CreateSBP2Session` | `SBP2Handler.hpp` |
| Login | 54 `StartSBP2Login` | `SBP2SessionRegistry` |
| Login-status | 55 `GetSBP2SessionState` | `LoginState`-enum |
| Send SCSI-CDB | 59 `SubmitSBP2Command` | `SBP2CommandWireFormats.hpp` |
| Hent resultat | 60 `GetSBP2CommandResult` | — |

## Forutsetninger

1. **ASFireWire-dext-en må være bygget og lastet.** Per ASFireWires README:
   bygg/signer via Xcode med betalt Apple-konto + entitlement, **eller** gratis
   konto + **SIP avskrudd**. Anbefalt:
   ```sh
   systemextensionsctl developer on
   ```
   Verifiser at den er lastet:
   ```sh
   systemextensionsctl list
   ```
2. **Maskinvare:** Apple TB→FW-adapter (+ TB3→TB2 ved behov) → FW800→FW400 til
   CoolScan 9000. Skanneren påslått.
3. Bussen må ha kommet opp i ASFireWire (Self-ID / topologi i loggen).

## Bygg og kjør

```sh
swift build
.build/debug/CoolScanProbe
```

Ingen entitlements trengs for *selve proben* utover at den ikke er sandkasset
(et vanlig CLI-verktøy er ikke det).

## Tolke utdata

- `Fant N enhet(er)` med en linje merket `SBP-2 ✅` → bussen og discovery virker.
- `SBP-2 login OK` → login/ORB/fetch-agent-maskineriet i ASFireWire virker mot 9000.
- `🎉 INQUIRY OK … vendor=«Nikon» product=«LS-9000 …»` → **transportlaget er bevist.**

### Hvis `IOServiceOpen` nektes (`KERN_PROTECTION_FAILURE`)
Da begrenser dext-en hvilke klienter som får åpne user-client-en. Med SIP av +
`systemextensionsctl developer on` er dette normalt ikke et problem. Hvis det
likevel skjer, er fallbacken å kjøre samme logikk fra **ASFW-appen** (som allerede
har tilgang) — koden her er bevisst delt så `SBP2Session`/`SCSI` kan limes rett inn.

## Veikart

- [x] Transport-probe (denne): discovery + login + INQUIRY + TEST UNIT READY
- [ ] CoolScan 9000 SCSI-kommandosett (mode select/sense, vindu, oppløsning,
      fokus/autofokus, multi-sampling, IR-kanal for Digital ICE) — port fra `coolscan3`
- [ ] Strip-vis lesing av bildedata → TIFF
- [ ] Enkelt frontend (CLI/app)

## Lisens

`coolscan3` er GPL; kode portet derfra gjør avledet verktøy GPL (greit for
personlig bruk). ASFireWire har p.t. **ingen LICENSE-fil** — dette verktøyet
kopierer ikke ASFireWire-kode, det konsumerer dext-en ved runtime.
