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

Przeanalizować:
1. Sekwencja AV/C: jakie komendy muszą pójść do urządzenia (SET_SIGNAL_FORMAT, PLUG_INFO itp.)
2. IRM: alokacja kanału isochronous i pasma
3. PCR space: ustawienie oPCR (output Plug Control Register) na urządzeniu
4. Uruchomienie IR context na właściwym kanale

Porównać z `docs/IOFireWireAVC/` i `docs/linux/` jako referencją. Zidentyfikować gap w aktualnym kodzie.

---

## Status

| Etap | Status |
|------|--------|
| 1 — Testy IR Receive | ⬜ Do zrobienia |
| 2 — Async Block/Lock | ⬜ Do zrobienia |
| 3 — StreamProcessor testy | ⬜ Do zrobienia |
| 4 — Analiza MOTU path | ⬜ Do zrobienia |
