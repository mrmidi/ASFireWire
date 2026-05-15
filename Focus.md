# Focus.md — Plan pracy nad ASFireWire

Cel końcowy: MOTU 828 MK3 działający na macOS Tahoe przez sterownik DriverKit.

## Stan implementacji (maj 2026)

| Subsystem | Status | Uwagi |
|-----------|--------|-------|
| OHCI init & bus reset | ✅ Działa | Self-ID, topology, gap count |
| Async TX/RX (quadlet read) | ✅ Działa | Block read/write, lock, PHY — częściowo |
| Config ROM reading | ✅ Działa | Pełny scanner z FSM multi-node |
| AV/C / FCP | 🚧 W toku | Music Subunit, PCR space |
| IRM | 🚧 Częściowo | Elekcja i alokacja kanału |
| Isoch Transmit (IT) | ✅ Działa | AM824 + SYT + cadence, testowane na hardware |
| Isoch Receive (IR) | 🚧 WIP | Pipeline istnieje, wymaga walidacji na hardware |
| AudioDriverKit | 🚧 W toku | `ASFWAudioDriver` + `ASFWAudioNub` podłączone |

---

## Etap 1 — Testy IR Receive ✏️

**Pliki:** `tests/IsochReceiveContextTests.cpp`, `tests/IsochRxDmaRingTests.cpp`

Aktualny stan testów jest szczątkowy (`IsochReceiveContextTests` ma tylko 4 testy, w tym jeden z `// TODO: Advanced test`). Napisać testy weryfikujące:

- `Poll()` przetwarza pakiety gdy deskryptory mają `xferStatus != 0`
- `Poll()` zwraca 0 na pustym ringu (już jest)
- `Stop()` poprawnie wyłącza kontekst i zeruje stan
- `Configure()` → `Start()` → `Stop()` → `Configure()` (re-use bez OOM)
- `DrainCompleted` prawidłowo re-arms deskryptory po przetworzeniu

Wszystkie bez hardware — symulacja przez bezpośredni zapis do deskryptorów w pamięci.

---

## Etap 2 — Brakujące komendy Async ✏️

**Pliki:** `ASFWDriver/Async/Commands/`, `tests/`

Block read/write i lock commands są "partially done". Sprawdzić co dokładnie brakuje, doimplementować i pokryć testami. Wzorzec: porównać z istniejącymi quadlet read/write.

---

## Etap 3 — Testy StreamProcessor / AM824 ✏️

**Pliki:** `ASFWDriver/Isoch/Receive/StreamProcessor.hpp`, `ASFWDriver/Isoch/Audio/AM824Decoder.hpp`

StreamProcessor parsuje CIP header i dekoduje AM824. Napisać testy z realnymi danymi:
- syntetyczne pakiety IEC 61883-6 (stereo 48kHz)
- walidacja DBC continuity (wykrywanie dropped packets)
- dekodowanie AM824 → PCM S32

Dane referencyjne dostępne w `tools/parse_amdtp.py` i plikach `.bin` w `tools/`.

---

## Etap 4 — Analiza ścieżki MOTU 828 MK3 ✏️

**Cel:** Ustalić dokładnie co musi się wydarzyć żeby MOTU zaczął streamować audio.

Analiza zakończona — szczegóły w `MOTU_828_MK3_BringUp.md`.

### Zidentyfikowane gapy (2 krytyczne)

**GAP 1 — `CMPClient::ConnectOPCR` nie wpisuje kanału do oPCR** (krytyczny)
- Plik: `ASFWDriver/Protocols/AVC/CMP/CMPClient.cpp`
- `ConnectOPCR(plug, callback)` wywołuje `PerformConnect(..., setChannel=nullopt, ...)` — inkrementuje p2p ale nie ustawia pola `channel` w oPCR
- Per IEC 61883-1 §10.4.2: kontroler MUSI wpisać kanał do oPCR przy p2p connect (tak jak robi `ConnectIPCR`)
- **Fix:** dodać parametr `uint8_t channel` do `ConnectOPCR`, przekazać jako `setChannel`

**GAP 2 — IRM nie jest wywoływany z `AVCAudioBackend::StartStreaming`** (krytyczny)
- Plik: `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp/.hpp`
- Kanały hardcoded: `kDefaultIrChannel=0`, `kDefaultItChannel=1` — bez `IRMClient::AllocateResources`
- `AVCAudioBackend` nie ma pola `IRMClient*` — brak ścieżki injection
- Bandwidth dla MOTU 48kHz 18ch ≈ 146 units (S400) nigdy nie rezerwowany
- **Fix:** dodać `SetIRMClient(IRMClient*)`, wywołać `AllocateResources(ch, bw, cb)` przed `StartReceive`, przekazać dynamiczny kanał

### Sekwencja po naprawie
```
AllocateResources(ch, bw) → StartReceive(ch) → ConnectOPCR(0, ch) → StartTransmit(ch+1) → ConnectIPCR(0, ch+1)
```

---

## Etap 5 — Implementacja gapów IRM + CMP ✏️

**Pliki:** `ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp/.cpp`, `ASFWDriver/Audio/Backends/AVCAudioBackend.hpp/.cpp`

### GAP 1 — `ConnectOPCR` z kanałem
- Dodać `uint8_t channel` do `ConnectOPCR(plug, channel, callback)`
- Zaktualizować `PerformConnect(…, setChannel=channel, …)`
- Zaktualizować wszystkich callerów (AVCAudioBackend, IsochHandler test)
- Testy jednostkowe

### GAP 2 — IRM w AVCAudioBackend
- Dodać `IRMClient* irmClient_` + `SetIRMClient(IRMClient*)`
- W `StartStreaming`: `AllocateResources(ch, bw, cb)` async przed `StartReceive`
- W `StopStreaming`: `ReleaseResources(ch, bw)` po `StopReceive`
- Bandwidth: 146 units dla 48kHz S400 (parametryzować przez kanałów)
- Testy jednostkowe

---

---

## Etap 6 — Bring-up hardening ✏️

Seria ulepszeń stabilności wymaganych przed testami hardware.

### 6a — GAP 3: oPCR read-back po ConnectOPCR ✅
- Plik: `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp`
- Po CAS `ConnectOPCR` odczytuje oPCR[0] i sprawdza, czy kanał faktycznie został wpisany
- 3 nowe testy w `CMPClientTests` — ReadOPCR OK/fail/invalid-plug

### 6b — Bus reset recovery w AudioCoordinator ✅
- Plik: `ASFWDriver/Audio/AudioCoordinator.cpp/.hpp`
- `OnDeviceSuspended`: zatrzymuje backend + dodaje GUID do `suspendedGuids_`
- `OnDeviceResumed`: gdy GUID był w `suspendedGuids_`, wywołuje ponownie `StartStreaming(guid)`
- IEEE 1394 §8.3: bus reset kończy wszystkie połączenia isoch — wymagana pełna sekwencja restart

### 6c — Naprawa akumulacji rescanAttempts_ ✅
- Plik: `ASFWDriver/Protocols/AVC/AVCDiscovery.cpp`
- `OnUnitResumed` resetuje licznik `rescanAttempts_[guid]`
- Bez tej poprawki po N bus resetach urządzenie traciłoby discovery na stałe

### 6d — IOPCIClassMatch zamiast IOPCIMatch ✅
- Plik: `ASFWDriver/Info.plist`
- `IOPCIMatch: 0x590111c1` (tylko Agere) → `IOPCIClassMatch: 0x0c001000&0xffffff00` (każdy OHCI FireWire)
- Teraz pasuje do Apple TB adapter (TI XIO2213B) i innych chipów OHCI

---

---

## Etap 7 — RX queue wiring fix ✏️

**Plik:** `ASFWDriver/Isoch/Audio/ASFWAudioDriver.cpp`

### Problem
`CreateRxQueue` w `ASFWAudioNub` jest wywoływany leniwie — dopiero przy pierwszym `StartAudioStreaming`.
`MapRxQueueFromNub` w `ASFWAudioDriver::Start` zawodzi bo kolejka jeszcze nie istnieje → `rxQueueValid = false`.
W `ZtsTimerOccurred` ścieżka RX jest pomijana — brak audio z FireWire IR do HAL.

### Fix ✅
W `StartDevice`, po `nub->StartAudioStreaming()`:
- Jeśli `!rxQueueValid` — wywołuje `MapRxQueueFromNub` ponownie → `HandleBeginRead` dostarcza audio z IR do HAL
- Jeśli `!txQueueValid` — wywołuje `MapTxQueueFromNub` ponownie → `HandleWriteEnd` pisze audio z CoreAudio do shared TX queue (nie do local ring buffer)
- Oba aktualizują `inputChannelCount`/`outputChannelCount` z nagłówków kolejek

---

## Etap 9 — HandleChangeSampleRate (runtime sample rate switching) ✏️

**Pliki:** `ASFWDriver/Isoch/Audio/ASFWIOUserAudioDevice.iig/.cpp`, `ASFWDriver/Protocols/AVC/IAVCDiscovery.hpp`, `ASFWDriver/Protocols/AVC/AVCDiscovery.hpp/.cpp`

### Problem
`IOUserAudioDevice` był tworzony jako `IOUserAudioDevice::Create(...)` — nie ma możliwości overrida `HandleChangeSampleRate`. Zmiana sample rate przez HAL (Audio MIDI Setup) była ignorowana: rate zablokowany na 48kHz ustawionym podczas discovery.

### Fix ✅
- **`ASFWIOUserAudioDevice`** — nowa podklasa `IOUserAudioDevice` (`.iig` + `.cpp`):
  - Override `HandleChangeSampleRate(double)`:
    1. `AudioCoordinator::StopStreaming(guid)`
    2. `IAVCDiscovery::SendSampleRateCommand(guid, rateHz, cb)` — AV/C opcode 0x19, poll ≤500ms
    3. `super::HandleChangeSampleRate(rate)` — aktualizuje CoreAudio device rate
    4. `AudioCoordinator::StartStreaming(guid)` — restart IR+IT przy nowym rate
  - `SetStreamingContext(ASFWAudioNub*, uint64_t guid)` LOCALONLY — wiring po `Create()`
- **`IAVCDiscovery::SendSampleRateCommand`** — nowa wirtualna metoda interfejsu
- **`AVCDiscovery::SendSampleRateCommand`** — implementacja: szuka AVCUnit po GUID, buduje `AVCCommand` z SFC (IEC 61883-6 Table 5), wysyła przez FCP, callback z wynikiem
- **`ASFWAudioDriver`** — tworzy `ASFWIOUserAudioDevice` zamiast `IOUserAudioDevice`, wywołuje `SetStreamingContext`
- **`MockAVCDiscovery`** w testach — dodana nowa metoda do mocka

### SFC mapping (IEC 61883-6 Table 5)
32kHz=0x00 · 44.1kHz=0x01 · 48kHz=0x02 · 88.2kHz=0x03 · 96kHz=0x04 · 176.4kHz=0x05 · 192kHz=0x06

---

## Status

| Etap | Status | Testy |
|------|--------|-------|
| 1 — Testy IR Receive | ✅ Zrobione | +12 testów (IsochRxDmaRing + IsochReceiveContext) |
| 2 — Async Block/Lock | ✅ Zrobione | +11 testów (AsyncCommandBuilder) + PayloadContextStub |
| 3 — StreamProcessor testy | ✅ Zrobione | +17 testów (StreamProcessor + AM824Decoder) |
| 4 — Analiza MOTU path | ✅ Zrobione | `MOTU_828_MK3_BringUp.md` — 2 krytyczne gapy |
| 5 — IRM + ConnectOPCR fix | ✅ Zrobione | +16 testów (CMPClientTests + IRMClientTests) |
| 6 — Bring-up hardening | ✅ Zrobione | +3 testy (ReadOPCR), bus reset recovery, rescan fix, IOPCIClassMatch |
| 7 — RX queue wiring fix | ✅ Zrobione | rxQueueValid=false bug — re-map po StartAudioStreaming |
| 8 — TX queue wiring fix | ✅ Zrobione | txQueueValid=false bug — HandleWriteEnd pisał do ring buffer zamiast shared queue |
| 9 — HandleChangeSampleRate | ✅ Zrobione | ASFWIOUserAudioDevice + SendSampleRateCommand — runtime rate switching |

---

## Stan ścieżki bring-up MOTU 828 MK3 (po Etapie 9)

### Zamknięte krytyczne gapy
| Krok | Status | Naprawione w |
|------|--------|-------------|
| OHCI init, bus reset, topology | ✅ | bazowy |
| Config ROM scan | ✅ | bazowy |
| AV/C discovery, Music Subunit | ✅ | bazowy |
| Sample rate → 48kHz via AV/C 0x19 | ✅ | Etap 5 |
| IRM: AllocateResources przed CMP | ✅ | Etap 5 |
| CMP ConnectOPCR z kanałem | ✅ | Etap 5 |
| oPCR read-back po connect | ✅ | Etap 6a |
| Bus reset recovery (suspend/resume) | ✅ | Etap 6b |
| rescanAttempts_ reset na resume | ✅ | Etap 6c |
| IOPCIClassMatch (TB adapter) | ✅ | Etap 6d |
| RX queue wiring w StartDevice | ✅ | Etap 7 |
| TX queue wiring w StartDevice | ✅ | Etap 8 |
| IR Poll ← przerwania OHCI | ✅ | bazowy |
| SYT clock gate (500ms) | ✅ | bazowy |

### Pozostałe (nie blokują initial test)
- `AVCUnitPlugSignalFormatCommand` / `MusicSubunit::SetSampleRate` — dead code (rate zmieniane przez `AVCDiscovery::SendSampleRateCommand`)
- Hardware test na Tahoe + TB adapter + MOTU 828 MK3

**Łącznie testów w projekcie: 488/488 ✅**

**Git:** 13 commitów czeka na push (brak dostępu collaboratora do `mrmidi/ASFireWire`)
