# MOTU 828 MK3 — Analiza ścieżki bring-up audio

> **Etap 4 Focus.md** — Ustalenie co musi się wydarzyć żeby MOTU 828 MK3 zaczął streamować audio przez ASFireWire na Tahoe.
>
> Data analizy: maj 2026. Autor: Etap 4 sesji ASFireWire.

---

## 1. Wymagana sekwencja bring-up

Poniżej pełna sekwencja zdarzeń, krok po kroku, wymagana przez IEEE 1394 + IEC 61883-1 + AV/C (TA 1999027):

```
[Boot / Bus Reset]
  │
  ▼
1. Self-ID + Topology                    ✅ zaimplementowane (OHCI init, BusManager)
2. Config ROM scan                        ✅ zaimplementowane (ROMScanner FSM)
3. AV/C: UNIT INFO                       ✅ zaimplementowane (AVCUnit::ProbeSubunits)
4. AV/C: SUBUNIT INFO                    ✅ zaimplementowane (AVCUnit::ProbeSubunits)
5. AV/C: MUSIC SUBUNIT capabilities      ✅ zaimplementowane (MusicSubunit probe)
6. AV/C: PLUG INFO (unit plugs)          ✅ zaimplementowane (AVCUnit::ProbePlugs)
7. AV/C: EXTENDED STREAM FORMAT          ✅ zaimplementowane (StreamFormatParser)
8. AV/C: SET INPUT PLUG SIGNAL FORMAT    ✅ zaimplementowane (AVCDiscovery, opcode 0x19)

[CoreAudio StartIO — AVCAudioBackend::StartStreaming]
  │
  ▼
9.  IRM: AllocateResources(ch, bw)       ❌ BRAK — kanały hardcoded 0/1
10. CMP: ConnectOPCR(0, channel=N)       ⚠️  p2p inkrementowany, ale ch NIE pisany do oPCR
11. IR:  IsochReceiveContext::Configure  ✅ zaimplementowane, ale z hardcoded ch=0
12. IR:  IsochReceiveContext::Start()    ✅ zaimplementowane
13. IT:  IsochTransmitContext::Configure ✅ zaimplementowane, ale z hardcoded ch=1
14. IT:  IsochTransmitContext::Start()   ✅ zaimplementowane
15. CMP: ConnectIPCR(0, channel=M)       ✅ zaimplementowane (ch=1 hardcoded)
```

---

## 2. Co jest zaimplementowane ✅

### AV/C Discovery (`AVCDiscovery.cpp`)
- UNIT INFO / SUBUNIT INFO / plug scan działa
- Sample rate switch: opcode 0x19 (INPUT PLUG SIGNAL FORMAT) na poziomie unit (subunit=0xFF), operand 0x90/0x02 = AM824 48kHz
- `MusicSubunitCapabilities::GetAudioDeviceConfiguration()` poprawnie mapuje kanały
- `AVCAudioBackend` jest tworzony i ma `SetCMPClient(...)` — CMP jest podłączony

### IRM Client (`IRMClient.cpp`)
- `AllocateResources(channel, bw, callback)` — pełna implementacja z CAS na `CHANNELS_AVAILABLE` (0xFFFF F000 0218) i `BANDWIDTH_AVAILABLE` (0xFFFF F000 0220) na węźle IRM
- `ReleaseResources(channel, bw, callback)` — symetrycznie zaimplementowane
- IRM node jest ustawiany po topology scan przez `ControllerCore`

### CMP Client (`CMPClient.cpp`)
- `ConnectOPCR(plug, callback)` — read → check online → CAS p2p+1
- `ConnectIPCR(plug, channel, callback)` — read → check online → CAS p2p+1 + set channel
- `DisconnectOPCR/DisconnectIPCR` — symetrycznie zaimplementowane
- `ReadOPCR(plug, callback)` — dostępne do read-back

### IsochReceiveContext
- `Configure(channel, contextIndex)` — ustawia `ContextMatch = 0xF0000000 | (channel & 0x3F)`, wzywa `audio_.ConfigureFor48k()`, buduje DMA ring
- `Start()` — wpisuje `CommandPtr`, ustawia `ContextControlSet = kRun | kIsochHeader`, włącza IR interrupt mask
- `Poll()` — przetwarza zakończone deskryptory, wywołuje `OnPacket`

### AVCAudioBackend::StartStreaming (sekwencja)
- Poprawna kolejność operacji: StartReceive → ConnectOPCR (sync) → StartTransmit → ConnectIPCR (sync)
- Poprawny teardown w razie błędu (StopReceive + DisconnectOPCR)
- SYT clock gate w StartTransmit (czeka max 500ms na ExternalSyncBridge)

---

## 3. Zidentyfikowane gapy ❌

### GAP 1 — IRM nie jest wywoływany (krytyczny)

**Plik:** `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp`, `AVCAudioBackend.hpp`

`AVCAudioBackend` nie ma pola `IRMClient*`. Klasa ma tylko `CMPClient*` (wstrzyknięty przez `SetCMPClient`). W `StartStreaming` kanały są hardcoded:

```cpp
constexpr uint8_t kDefaultIrChannel = 0;   // hardcoded
constexpr uint8_t kDefaultItChannel = 1;   // hardcoded
```

`IRMClient::AllocateResources` nigdy nie jest wywołane. Skutki:
- Naruszenie IEEE 1394 (pasmo nie jest rezerwowane)
- Jeśli inny węzeł na magistrali zajął kanał 0, MOTU i tak nadaje na 0 → kolizja
- Przy starcie CoreAudio na MOTU, który sam wybrał inny kanał (np. po reset), kontekst IR słucha na złym kanale

**Wymagana poprawka:**
1. Dodaj `IRMClient* irmClient_{nullptr}` + `SetIRMClient(IRMClient*)` do `AVCAudioBackend`
2. Przed `StartReceive` wywołaj `irmClient_->AllocateResources(channel, bandwidth, callback)` asynchronicznie
3. Przekaż rzeczywisty kanał z IRM do `StartReceive` i `ConnectOPCR`

Bandwidth dla 48kHz AM824 S400 (MOTU 828 MK3, 18 kanałów):
```
syt_interval = 8 (48kHz non-blocking)
dbs = 18 (16 audio + 2 midi/reserved slots)
isoch_payload = syt_interval * dbs * 4 = 8 * 18 * 4 = 576 bytes
// IEC 61883-1 §B.2:
bandwidth_units = ceil((32 + 576) * 8 / (0.0491775 * 1600)) ≈ 146 units  (S400)
// Praktycznie: Apple używa ~146 units dla stereo 48kHz, skaluj proporcjonalnie do kanałów
```

---

### GAP 2 — ConnectOPCR nie wpisuje kanału do oPCR (krytyczny)

**Plik:** `ASFWDriver/Protocols/AVC/CMP/CMPClient.cpp`, linia 149-158

```cpp
void CMPClient::ConnectOPCR(uint8_t plugNum, CMPCallback callback) {
    // ...
    PerformConnect(PCRRegisters::GetOPCRAddress(plugNum), plugNum,
                   std::nullopt,   // ← channel NOT written!
                   callback);
}
```

Per IEC 61883-1 §10.4.2, przy p2p connection do oPCR, kontroler MUSI napisać numer kanału do pola `channel[5:0]` w tym samym CAS który inkrementuje `p2p_connection_count`. Bez tego:
- MOTU widzi p2p=1 (connection established)
- Ale pole `channel` w oPCR = cokolwiek co device miał domyślnie (zazwyczaj 63 = unallocated lub ostatnia wartość)
- Kontekst IR słucha na kanale 0 (hardcoded), MOTU nadaje na innym → brak pakietów

Dla porównania: `ConnectIPCR` **robi to poprawnie** (bierze `channel` i pisze do iPCR).

**Wymagana poprawka:**
```cpp
// Dodaj przeciążenie lub zmień sygnaturę:
void CMPClient::ConnectOPCR(uint8_t plugNum, uint8_t channel, CMPCallback callback);
// → PerformConnect(..., setChannel = channel, ...)
```

---

### GAP 3 — Brak read-back kanału z oPCR po connect

**Plik:** `ASFWDriver/Audio/Backends/AVCAudioBackend.cpp`

Po `ConnectOPCR` kanał w oPCR powinien być odczytany z powrotem i użyty do konfiguracji IR context. Prawidłowy flow (per Apple IOFireWireAVC):
```
AllocateIRMChannel(N)
ConnectOPCR(plug=0, channel=N)   ← pisze N do oPCR
ReadOPCR(plug=0)                  ← potwierdza że N jest w oPCR
IsochReceiveContext::Configure(channel=N, ...)
```

Obecnie `StartReceive(kDefaultIrChannel=0)` jest wywołany **przed** `ConnectOPCR`, i używa hardcoded 0. To zbieg okoliczności jeśli zadziała.

---

### GAP 4 — opcode 0x19 vs 0x18 dla MOTU RX path

**Plik:** `ASFWDriver/Protocols/AVC/AVCDiscovery.cpp`, linia ~449

```cpp
cdb.opcode = 0x19;   // INPUT PLUG SIGNAL FORMAT — DEVICE RECEIVES from host (TX path)
```

Opcode 0x19 = `INPUT PLUG SIGNAL FORMAT` = format na wejściu urządzenia (co przyjmuje od hosta).
Opcode 0x18 = `OUTPUT PLUG SIGNAL FORMAT` = format na wyjściu urządzenia (co wysyła do hosta = nasze RX).

Większość urządzeń (w tym MOTU 828 MK3 na Apple stack) mirroruje oba przy zmianie jednego. Ale jeśli MOTU nie zaczyna nadawać po CMP connect, warto sprawdzić czy wysłanie 0x18 na unit output plug 0 z tym samym operandem zmienia sytuację.

Nie jest to bloker — niskie prawdopodobieństwo problemu z MOTU.

---

### GAP 5 — IRMClient nie jest wstrzykiwany do AVCAudioBackend

**Plik:** `ASFWDriver/Audio/Backends/AVCAudioBackend.hpp`

`AVCAudioBackend` ma `SetCMPClient(CMPClient*)` ale nie ma odpowiednika dla `IRMClient`. Nawet gdyby chcieć dodać IRM allocation, nie ma ścieżki injection. Dla porównania: `DiceAudioBackend` nie ma też IRM (używa hardcoded kanałów 0/1 w tej samej przestrzeni problemowej).

---

## 4. Kolejność naprawy

| # | Gap | Trudność | Bloker? |
|---|-----|----------|---------|
| 1 | `ConnectOPCR` bez channel | Mała — dodać parametr + testy | **TAK** |
| 2 | IRM allocation missing | Średnia — wstrzyknąć IRMClient, async flow | **TAK** |
| 3 | Read-back oPCR po connect | Mała — ReadOPCR callback chain | Tak (precyzja) |
| 4 | StartReceive przed ConnectOPCR | Refactor kolejności | Nie (SYT gate je rozdziela) |
| 5 | opcode 0x18 dla output plug | Mała — opcjonalny dodatkowy AVC command | Nie |

---

## 5. Proponowany poprawiony flow w AVCAudioBackend::StartStreaming

```
1. AllocateResources(ch=auto, bw=146*ratio) →async→ callback z ch
2. StartReceive(ch)          ← IR context konfiguruje się na ch
3. ConnectOPCR(0, ch)        ← oPCR[0].channel = ch, p2p++
4. ReadOPCR(0)               ← potwierdzenie (optional)
5. [SYT clock gate]
6. StartTransmit(ch+1)       ← IT na kanale ch+1 (też z IRM)
7. ConnectIPCR(0, ch+1)      ← iPCR[0].channel = ch+1, p2p++
```

Teardown (odwrotna kolejność):
```
DisconnectIPCR → StopTransmit → DisconnectOPCR → StopReceive → ReleaseResources
```

---

## 6. MOTU 828 MK3 — identyfikacja urządzenia

Brak wpisu MOTU w codebase. Vendory MOTU do dodania w quirks gdy potrzeba:

| Parametr | Wartość |
|----------|---------|
| Vendor ID | `0x0001F2` (MOTU — Mark of the Unicorn) |
| Model ID 828 MK3 | sprawdzić z `dmesg`/FireBug na Sequoia |
| Spec ID | `0x00A02D` (standardowy AV/C) |
| Stream mode | `kNonBlocking` (standard IEC 61883-6, 8 samples/packet @ 48kHz) |

Brak MOTU quirka oznacza że urządzenie będzie traktowane jako generyczny AV/C — co jest prawidłowe.

---

## 7. Podsumowanie — co zrobić żeby MOTU zagrał

**Minimalne poprawki (2 pliki):**

1. **`CMPClient::ConnectOPCR`** — dodać parametr `uint8_t channel`, przekazać do `PerformConnect` jako `setChannel`
2. **`AVCAudioBackend`** — dodać `IRMClient* irmClient_` + `SetIRMClient`, wywołać `AllocateResources` przed `StartReceive`, przekazać kanał dynamicznie zamiast hardcoded 0/1

Po tych poprawkach sekwencja będzie spec-compliant i MOTU powinien zacząć nadawać na właściwym kanale.
