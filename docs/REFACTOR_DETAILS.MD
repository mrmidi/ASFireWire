# ASOHCI Refactoring Implementation Details

This document provides the specific code stubs and implementation details for the refactoring effort outlined in `REFACTOR.md`. Use this as a reference when populating the new class files.

---

## 1. Bus / Link State Management

- **Class:** `ASOHCIBusManager`
- **Purpose:** Handles the overall state of the FireWire bus, including initialization, shutdown, and reset events.

### `ASOHCIBusManager.hpp`

```cpp
#pragma once
#include <DriverKit/DriverKit.h>
class IOPCIDevice;

class ASOHCIBusManager {
public:
  bool init(IOPCIDevice* pci, uint8_t barIndex);
  void onStart();                      // TODO: OHCI 5.7 link state sequencing; 11.x Self-ID flow
  void onStop();                       // TODO: mask IRQs, HCControl clear, §6, §5.7
  void handleBusResetIRQ();            // TODO: §6.1 BusReset; §11 Self-ID timing
  void handleSelfIDCompleteIRQ();      // TODO: §11.5 Interrupt considerations; re-arm selfID

  // Optional: algorithmic helpers
  void considerGapCountUpdate();       // TODO: set/verify gap count from Self-ID per OHCI 5.7 & 1394-2008 §8.6.2

private:
  IOPCIDevice* _pci{nullptr};
  uint8_t      _bar{0};
};
```

### `ASOHCIBusManager.cpp`

```cpp
#include "ASOHCIBusManager.hpp"
#include <PCIDriverKit/IOPCIDevice.h>
bool ASOHCIBusManager::init(IOPCIDevice* p, uint8_t bar){ _pci=p; _bar=bar; return _pci!=nullptr; }
void ASOHCIBusManager::onStart() { /* no-op: driven today by ASOHCI::Start; keep hooks for future */ }
void ASOHCIBusManager::onStop()  { /* no-op: pair with Start cleanup per §5.7, §6 */ }
void ASOHCIBusManager::handleBusResetIRQ() { /* TODO: mask BusReset (§6.2), arm SelfID (§11.4), snapshot NodeID (§5.11) */ }
void ASOHCIBusManager::handleSelfIDCompleteIRQ() { /* TODO: read SelfIDCount (§11.2), parse, (re-)enable cycle timer (§5.13, §10/9) */ }
void ASOHCIBusManager::considerGapCountUpdate() { /* TODO: derive from Self-IDs per 1394-2008 topology rules */ }
```

---

## 2. Safe MMIO Register Access

- **Class:** `ASOHCIRegisterAccess`
- **Purpose:** Provides safe, centralized static methods for reading from and writing to OHCI hardware registers.

### `ASOHCIRegisterAccess.hpp`

```cpp
#pragma once
#include <stdint.h>
class IOPCIDevice;

struct ASOHCIRegisterAccess {
  static void wr(IOPCIDevice* pci, uint8_t bar, uint32_t off, uint32_t v);
  static uint32_t rd(IOPCIDevice* pci, uint8_t bar, uint32_t off);
  static void set(IOPCIDevice* pci, uint8_t bar, uint32_t off, uint32_t mask); // §5.* set regs
  static void clr(IOPCIDevice* pci, uint8_t bar, uint32_t off, uint32_t mask); // §5.* clear regs
};
```

### `ASOHCIRegisterAccess.cpp`

```cpp
#include "ASOHCIRegisterAccess.hpp"
#include <PCIDriverKit/IOPCIDevice.h>
void ASOHCIRegisterAccess::wr(IOPCIDevice* p, uint8_t b, uint32_t o, uint32_t v){ if(p) p->MemoryWrite32(b,o,v); }
uint32_t ASOHCIRegisterAccess::rd(IOPCIDevice* p, uint8_t b, uint32_t o){ uint32_t v=0; if(p) p->MemoryRead32(b,o,&v); return v; }
void ASOHCIRegisterAccess::set(IOPCIDevice* p, uint8_t b, uint32_t o, uint32_t m){ wr(p,b,o,m); /* OHCI uses distinct Set/Clear */ }
void ASOHCIRegisterAccess::clr(IOPCIDevice* p, uint8_t b, uint32_t o, uint32_t m){ wr(p,b,o,m); }
```

---

## 3. Cycle Timer & Timebase

- **Class:** `ASOHCICycleTimerService`
- **Purpose:** Manages the OHCI isochronous cycle timer.

### `ASOHCICycleTimerService.hpp`

```cpp
#pragma once
#include <stdint.h>
class IOPCIDevice;

class ASOHCICycleTimerService {
public:
  bool init(IOPCIDevice* pci, uint8_t barIndex);
  void enable(bool en);     // TODO: §5.13; LinkControl.CycleTimerEnable (§5.10) gating
  uint32_t readCycleTimer(); // TODO: §5.13 bits [24:0] + seconds wrap (§6.1 Cycle64Seconds)
private:
  IOPCIDevice* _pci{nullptr};
  uint8_t _bar{0};
};
```

### `ASOHCICycleTimerService.cpp`

```cpp
#include "ASOHCICycleTimerService.hpp"
#include <PCIDriverKit/IOPCIDevice.h>
bool ASOHCICycleTimerService::init(IOPCIDevice* p, uint8_t b){ _pci=p; _bar=b; return _pci!=nullptr; }
void ASOHCICycleTimerService::enable(bool){ /* no-op: ASOHCI toggles today; centralize later per §5.10/§5.13 */ }
uint32_t ASOHCICycleTimerService::readCycleTimer(){ uint32_t v=0; if(_pci) _pci->MemoryRead32(_bar, /*kOHCI_IsochronousCycleTimer*/ 0, &v); return v; }
```

---

## 4. Self-ID & Topology Database

- **Class:** `ASOHCITopologyDB`
- **Purpose:** Parses Self-ID packets and maintains a database of the FireWire bus topology.

### `ASOHCITopologyDB.hpp`

```cpp
#pragma once
#include <vector>
#include <stdint.h>

struct ASAlphaNode {
  uint8_t phyId{0};
  bool linkActive{false};
  uint8_t speedCode{0}; // 0=S100… per §11 / IEEE 1394-2008 §16.3.2.1
  uint8_t powerClass{0};
  bool contender{false};
  std::vector<uint8_t> ports; // 2-bit codes per Table 16-4
};

class ASOHCITopologyDB {
public:
  void reset();
  void ingestSelfIDs(const uint32_t* quads, uint32_t count); // TODO: feed from SelfIDParser
  const std::vector<ASAlphaNode>& nodes() const { return _nodes; }
  uint8_t probableRoot() const; // TODO: derive root based on parent/child port codes

private:
  std::vector<ASAlphaNode> _nodes;
};
```

### `ASOHCITopologyDB.cpp`

```cpp
#include "ASOHCITopologyDB.hpp"
void ASOHCITopologyDB::reset(){ _nodes.clear(); }
void ASOHCITopologyDB::ingestSelfIDs(const uint32_t*, uint32_t){ /* no-op: hook to your SelfIDParser output */ }
uint8_t ASOHCITopologyDB::probableRoot() const { return 0xFF; /* TODO: choose node with no parent port, see 1394-2008 */ }
```

---

## 5. Configuration ROM Builder

- **Class:** `ASOHCIConfigROM`
- **Purpose:** Builds the IEEE 1212-compliant Configuration ROM image.

### `ASOHCIConfigROM.hpp`

```cpp
#pragma once
#include <vector>
#include <stdint.h>

// Minimal ROM image container. See IEEE 1212-2001 §7.x (formats, CRC, required entries).
class ASOHCIConfigROM {
public:
  bool buildMinimal();                   // TODO: §7.2, §7.6 required leaves/directories
  const std::vector<uint32_t>& image() const { return _quads; }
  uint32_t crc32() const;                // TODO: §7.3 CRC calculation over header/dirs

private:
  std::vector<uint32_t> _quads;
};
```

### `ASOHCIConfigROM.cpp`

```cpp
#include "ASOHCIConfigROM.hpp"
bool ASOHCIConfigROM::buildMinimal(){ _quads.clear(); /* TODO: Bus_Info_Block, Root_Dir per §7.6 */ return true; }
uint32_t ASOHCIConfigROM::crc32() const { return 0; /* TODO: §7.3 polynomial calc */ }
```

---

## 6. CSR Space Façade

- **Class:** `ASOHCICSRSpace`
- **Purpose:** Exposes the CSR (Control and Status Register) address space that the host must serve.

### `ASOHCICSRSpace.hpp`

```cpp
#pragma once
#include <stdint.h>
#include <vector>

// Expose CSR registers a host must serve (1212 §6), plus OHCI §5.5 autonomous windows.
class ASOHCICSRSpace {
public:
  void attachConfigROM(const std::vector<uint32_t>* rom); // map ROM into CSR window (§5.5)
  // Handlers for transactions routed to phys req filter (§12 / §5.14)
  bool read(uint64_t addr, uint32_t* out);   // TODO: 1212 §5.1 + §6 register map
  bool write(uint64_t addr, uint32_t val);   // TODO: 1212 STATE_CLEAR/SET semantics (§6.1–§6.5)
  bool lock(uint64_t addr, uint32_t arg, uint32_t* out); // TODO: §5.2 lock transactions

private:
  const std::vector<uint32_t>* _rom{nullptr};
};
```

### `ASOHCICSRSpace.cpp`

```cpp
#include "ASOHCICSRSpace.hpp"
void ASOHCICSRSpace::attachConfigROM(const std::vector<uint32_t>* r){ _rom=r; }
bool ASOHCICSRSpace::read(uint64_t, uint32_t* out){ if(out) *out=0; return false; }
bool ASOHCICSRSpace::write(uint64_t, uint32_t){ return false; }
bool ASOHCICSRSpace::lock(uint64_t, uint32_t, uint32_t* out){ if(out) *out=0; return false; }
```

---

## 7. Asynchronous Request/Response Orchestration

- **Class:** `ASOHCIAsyncManager`
- **Purpose:** Orchestrates the Asynchronous Receive (AR) and Asynchronous Transmit (AT) DMA contexts.

### `ASOHCIAsyncManager.hpp`

```cpp
#pragma once
#include <stdint.h>

class ASOHCIARContext;
class ASOHCIATContext;
class ASOHCICSRSpace;

class ASOHCIAsyncManager {
public:
  void bind(ASOHCIARContext* arReq, ASOHCIARContext* arRsp,
            ASOHCIATContext* atReq, ASOHCIATContext* atRsp,
            ASOHCICSRSpace* csr);

  void onARPacketAvailable();  // TODO: §8.7 AR data formats; decode headers, route to CSR or to client
  void onATComplete();         // TODO: §7.6 AT interrupts; ack handling and retries (§7.4)
  void queueAsyncResponse();   // TODO: construct response packet (§7.8) via ATContext

private:
  ASOHCIARContext* _arReq{nullptr};
  ASOHCIARContext* _arRsp{nullptr};
  ASOHCIATContext* _atReq{nullptr};
  ASOHCIATContext* _atRsp{nullptr};
  ASOHCICSRSpace*  _csr{nullptr};
};
```

### `ASOHCIAsyncManager.cpp`

```cpp
#include "ASOHCIAsyncManager.hpp"
void ASOHCIAsyncManager::bind(ASOHCIARContext* a, ASOHCIARContext* b, ASOHCIATContext* c, ASOHCIATContext* d, ASOHCICSRSpace* e){
  _arReq=a; _arRsp=b; _atReq=c; _atRsp=d; _csr=e;
}
void ASOHCIAsyncManager::onARPacketAvailable(){ /* TODO: parse §8.7 headers, handle CSR vs. client */ }
void ASOHCIAsyncManager::onATComplete(){ /* TODO: read xferStatus (§7.6), manage retries (§7.4) */ }
void ASOHCIAsyncManager::queueAsyncResponse(){ /* TODO: build OUTPUT_* descriptors (§7.1, §7.8) and wake context (§3.1) */ }
```

---

## 8. Isochronous Orchestration

- **Class:** `ASOHCIIsochManager`
- **Purpose:** Orchestrates the Isochronous Receive (IR) and Isochronous Transmit (IT) DMA contexts.

### `ASOHCIIsochManager.hpp`

```cpp
#pragma once
#include <stdint.h>
#include <vector>

class ASOHCIITContext;
class ASOHCIIRContext;

struct IsochChannelReservation {
  uint8_t  channel{63}; // 0..63; 63=none
  uint32_t speedCode{0}; // alpha S100.. per §9.6 / §10.6
  uint16_t maxPayload{0};
};

class ASOHCIIsochManager {
public:
  bool init(); // prepare queues (no HW touching)
  // Tx
  bool openIT(uint8_t channel, uint32_t speedCode, uint16_t maxPayload); // §9.2 context regs + §9.3 controller
  bool queueITPacket(const void* payload, uint32_t bytes, uint32_t tag, uint32_t sy); // §9.6 data format
  void closeIT(uint8_t channel);

  // Rx
  bool openIR(uint8_t channel, uint32_t speedCode, uint16_t bufBytes);   // §10.3 + §10.4 controller
  bool recycleIRBuffers();                                               // §10.1 list programs
  void closeIR(uint8_t channel);

private:
  std::vector<IsochChannelReservation> _openIT;
  std::vector<IsochChannelReservation> _openIR;
  ASOHCIITContext* _it{nullptr};
  ASOHCIIRContext* _ir{nullptr};
};
```

### `ASOHCIIsochManager.cpp`

```cpp
#include "ASOHCIIsochManager.hpp"
bool ASOHCIIsochManager::init(){ return true; }
bool ASOHCIIsochManager::openIT(uint8_t, uint32_t, uint16_t){ return true; }     // TODO: IT context init (§9.2)
bool ASOHCIIsochManager::queueITPacket(const void*, uint32_t, uint32_t, uint32_t){ return true; } // TODO: build OUTPUT_* chain (§9.1)
void ASOHCIIsochManager::closeIT(uint8_t){ /* TODO: stop run, drain (§3.1) */ }
bool ASOHCIIsochManager::openIR(uint8_t, uint32_t, uint16_t){ return true; }     // TODO: IR context init (§10.3)
bool ASOHCIIsochManager::recycleIRBuffers(){ return true; }                       // TODO: §10.1 program append
void ASOHCIIsochManager::closeIR(.8_t){ /* TODO: stop run, drain */ }
```

---

## 9. Dedicated IT/IR Context Wrappers

- **Classes:** `ASOHCIITContext`, `ASOHCIIRContext`
- **Purpose:** Wrappers for the Isochronous Transmit and Receive DMA contexts.

### `ASOHCIITContext.hpp`

```cpp
#pragma once
#include <stdint.h>
class IOPCIDevice;

class ASOHCIITContext {
public:
  bool initialize(IOPCIDevice*, uint8_t barIndex, uint32_t contextNum);
  bool start();
  bool stop();
  bool appendProgramBlock(/* dma addr/len etc. */); // TODO: §9.4 appending
  void handleInterrupt(); // TODO: §9.5 IT interrupts

private:
  IOPCIDevice* _pci{nullptr};
  uint8_t _bar{0};
  uint32_t _ctx{0};
  // TODO: descriptor pool like your AT/AR
};
```

### `ASOHCIITContext.cpp`

```cpp
#include "ASOHCIITContext.hpp"
bool ASOHCIITContext::initialize(IOPCIDevice*, uint8_t, uint32_t){ return true; }
bool ASOHCIITContext::start(){ return true; }
bool ASOHCIITContext::stop(){ return true; }
bool ASOHCIITContext::appendProgramBlock(){ return true; }
void ASOHCIITContext::handleInterrupt(){ /* no-op */ }
```

### `ASOHCIIRContext.hpp`

```cpp
#pragma once
#include <stdint.h>
class IOPCIDevice;

class ASOHCIIRContext {
public:
  bool initialize(IOPCIDevice*, uint8_t barIndex, uint32_t contextNum);
  bool start();
  bool stop();
  bool replenishBuffers(); // TODO: §10.1 program patterns
  void handleInterrupt();  // TODO: §10.5 IR interrupts

private:
  IOPCIDevice* _pci{nullptr};
  uint8_t _bar{0};
  uint32_t _ctx{0};
};
```

### `ASOHCIIRContext.cpp`

```cpp
#include "ASOHCIIRContext.hpp"
bool ASOHCIIRContext::initialize(IOPCIDevice*, uint8_t, uint32_t){ return true; }
bool ASOHCIIRContext::start(){ return true; }
bool ASOHCIIRContext::stop(){ return true; }
bool ASOHCIIRContext::replenishBuffers(){ return true; }
void ASOHCIIRContext::handleInterrupt(){ /* no-op */ }
```

---

## 10. DMA Descriptor Micro-Builder

- **Namespace:** `ASOHCIDesc`
- **Purpose:** Provides functions to encode OHCI DMA descriptors for all context types.

### `ASOHCIDMAProgramBuilder.hpp`

```cpp
#pragma once
#include <stdint.h>

// Pack/encode OHCI DMA descriptors for AT/AR/IT/IR per §3.2, §7.1, §8.1, §9.1, §10.1.
namespace ASOHCIDesc {
  struct OutputMore { /* fields */ };
  struct OutputLast { /* fields */ };
  struct InputMore  { /* fields */ };
  struct InputLast  { /* fields */ };

  void initOutputMore(OutputMore* d, uint32_t dma, uint32_t bytes); // TODO: set cmd,key,i,b etc.
  void initOutputLast(OutputLast* d, uint32_t dma, uint32_t bytes);
  void initInputMore (InputMore*  d, uint32_t dma, uint32_t bytes);
  void initInputLast (InputLast*  d, uint32_t dma, uint32_t bytes);
}
```

### `ASOHCIDMAProgramBuilder.cpp`

```cpp
#include "ASOHCIDMAProgramBuilder.hpp"
using namespace ASOHCIDesc;
void initOutputMore(OutputMore*, uint32_t, uint32_t) {}
void initOutputLast(OutputLast*, uint32_t, uint32_t) {}
void initInputMore (InputMore*,  uint32_t, uint32_t) {}
void initInputLast (InputLast*,  uint32_t, uint32_t) {}
```

---

## 11. Physical Request Filtering

- **Class:** `ASOHCIAddressHandler`
- **Purpose:** Handles physical read/write/lock requests routed from the AR context.

### `ASOHCIAddressHandler.hpp`

```cpp
#pragma once
#include <stdint.h>
class ASOHCICSRSpace;

class ASOHCIAddressHandler {
public:
  explicit ASOHCIAddressHandler(ASOHCICSRSpace* csr) : _csr(csr) {}
  bool handlePhysicalRead (uint64_t addr, uint32_t* out); // §12.1 + 1212 §5.1
  bool handlePhysicalWrite(uint64_t addr, uint32_t val);  // §12.1
  bool handleLock       (uint64_t addr, uint32_t arg, uint32_t* out); // 1212 §5.2

  void onPostedWriteError(uint32_t hi, uint32_t lo); // §12.2 host bus err follow-up

private:
  ASOHCICSRSpace* _csr{nullptr};
};
```

### `ASOHCIAddressHandler.cpp`

```cpp
#include "ASOHCIAddressHandler.hpp"
#include "ASOHCICSRSpace.hpp"
bool ASOHCIAddressHandler::handlePhysicalRead(uint64_t a, uint32_t* o){ return _csr && _csr->read(a,o); }
bool ASOHCIAddressHandler::handlePhysicalWrite(uint64_t a, uint32_t v){ return _csr && _csr->write(a,v); }
bool ASOHCIAddressHandler::handleLock(uint64_t a, uint32_t w, uint32_t* o){ return _csr && _csr->lock(a,w,o); }
void ASOHCIAddressHandler::onPostedWriteError(uint32_t, uint32_t){ /* TODO: §13 host bus error diagnostics */ }
```

---

## 12. Lightweight Trace Utility

- **File:** `ASOHCITrace.hpp`
- **Purpose:** Provides a centralized `os_log` handle for tracing.

### `ASOHCITrace.hpp`

```cpp
#pragma once
#include <os/log.h>
inline os_log_t ASOHCITraceLog(){ static os_log_t L=os_log_create("net.mrmidi.ASFireWire.ASOHCI","trace"); return L; }
```

### `ASOHCITrace.cpp`

```cpp
// This file can be left empty or can contain a single `#include "ASOHCITrace.hpp"` if needed for build systems.
```
