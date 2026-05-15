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

## Status

| Etap | Status | Testy |
|------|--------|-------|
| 1 — Testy IR Receive | ✅ Zrobione | +12 testów (IsochRxDmaRing + IsochReceiveContext) |
| 2 — Async Block/Lock | ✅ Zrobione | +11 testów (AsyncCommandBuilder) + PayloadContextStub |
| 3 — StreamProcessor testy | ✅ Zrobione | +17 testów (StreamProcessor + AM824Decoder) |
| 4 — Analiza MOTU path | ✅ Zrobione | `MOTU_828_MK3_BringUp.md` — 2 krytyczne gapy |
| 5 — IRM + ConnectOPCR fix | ✅ Zrobione | +16 testów (CMPClientTests + IRMClientTests) |

**Łącznie testów w projekcie: 485/485 ✅**

**Git:** 7 commitów czeka na push (brak dostępu collaboratora do `mrmidi/ASFireWire`)
