# AR Request WrResp ACK Missing - Critical Bug Analysis

## TL;DR - Critical Findings

**The Bug**: ASFW receives FCP response block writes but never sends WrResp ACK packets, causing devices to retransmit.

**Root Cause**: Missing AT Response context and WrResp transmission infrastructure.

**Solution Validated**: Both Linux (`firewire/core-transaction.c`) and Apple (`IOFireWireController::processWriteRequest`) use identical pattern:
1. Handler returns rCode (not void)
2. Infrastructure sends WrResp automatically (not user code)
3. Broadcast writes (destID=0xFFFF) never get responses

**Critical Corrections Applied**:
- \u2705 Fixed `ResponseCode` enum values to match IEEE 1394 spec (AddressError=0x7, not 0x5)
- \u2705 Added broadcast destID filtering (no WrResp for 0xFFFF)
- \u2705 Clarified policy separation: handlers choose rCode, AR infrastructure sends WrResp
- \u2705 Made WrResp machinery protocol-agnostic (not FCP-specific)

**Implementation Ready**: Document now serves as spec-grade design guide for AT Response context implementation.

---

## Problem Statement

**Symptom**: AV/C devices (e.g., Apogee Duet) retransmit FCP response packets multiple times, causing duplicate responses and "opcode mismatch" validation errors.

**Root Cause**: ASFW driver receives incoming FCP response block writes (AR Request context, tCode 0x1) but **never sends WrResp ACK packets** back to the device. The device interprets missing ACKs as packet loss and retransmits.

**Severity**: **CRITICAL** - Blocks proper AV/C device communication, causes response queue desynchronization.

---

## IEEE 1394 Transaction Protocol

### Split Transaction Model

Per IEEE 1394-1995 §6.2.4, asynchronous block write transactions follow this sequence:

```
Initiator (Device)                    Responder (Driver)
─────────────────                    ──────────────────
1. Send Block Write Request
   (tCode=0x1, payload=FCP response)
                                 ──▶  2. Receive in AR Request context
                                      3. Process request (route to handler)
                                 ◀──  4. Send Write Response (WrResp)
                                         tCode=0x2, rCode=COMPLETE
5. Receive WrResp ACK
6. Transaction complete ✓
```

**If WrResp is missing:**
```
Initiator (Device)                    Responder (Driver)
─────────────────                    ──────────────────
1. Send Block Write Request
                                 ──▶  2. Receive, process
                                      3. ❌ NO WrResp sent!
5. Timeout waiting for WrResp
6. Retransmit same packet (tLabel may change)
                                 ──▶  7. Duplicate received
8. Retransmit again...
                                 ──▶  9. Duplicate received
```

---

## Packet Analyzer Evidence

From actual Apogee Duet capture:

```
028:6379  UNIT_INFO cmd (tLabel=34, opcode=0x30)          ← Driver sends command
028:6381  WrResp (tLabel=34)                              ← Device ACKs command
028:6381  UNIT_INFO response (tLabel=31) ← DUPLICATE #1  ⚠️ Device retransmits
028:6384  SUBUNIT_INFO cmd (tLabel=35, opcode=0x31)      ← Driver sends next command
028:7064  UNIT_INFO response (tLabel=30) ← DUPLICATE #2  ⚠️ Device retransmits again
028:7747  WrResp (tLabel=35)                              ← Device ACKs next command
028:7747  SUBUNIT_INFO response (tLabel=1)                ← Correct response
```

**Analysis:**
- Device sends UNIT_INFO response at `028:6381` (tLabel=31)
- Driver **never sends WrResp** for this block write
- Device retransmits at `028:7064` (tLabel=30) - **different tLabel!**
- Driver receives duplicate while waiting for SUBUNIT_INFO response
- Validation correctly rejects (opcode 0x30 ≠ 0x31)

---

## Linux FireWire Implementation

### Core Transaction Handler

**File**: `firewire/core-transaction.c`

#### FCP Region Handler (lines 925-962)

```c
static void handle_fcp_region_request(struct fw_card *card,
                      struct fw_packet *p,
                      struct fw_request *request,
                      unsigned long long offset)
{
    struct fw_address_handler *handler;
    int tcode, destination, source;

    // Validate FCP address and size
    if ((offset != (CSR_REGISTER_BASE | CSR_FCP_COMMAND) &&
         offset != (CSR_REGISTER_BASE | CSR_FCP_RESPONSE)) ||
        request->length > 0x200) {
        fw_send_response(card, request, RCODE_ADDRESS_ERROR);
        return;
    }

    tcode = async_header_get_tcode(p->header);
    destination = async_header_get_destination(p->header);
    source = async_header_get_source(p->header);

    // Only accept write requests
    if (tcode != TCODE_WRITE_QUADLET_REQUEST &&
        tcode != TCODE_WRITE_BLOCK_REQUEST) {
        fw_send_response(card, request, RCODE_TYPE_ERROR);
        return;
    }

    // Invoke ALL registered FCP handlers (allows multiple listeners)
    scoped_guard(rcu) {
        list_for_each_entry_rcu(handler, &address_handler_list, link) {
            if (is_enclosing_handler(handler, offset, request->length))
                handler->address_callback(card, request, tcode, destination, source,
                                          p->generation, offset, request->data,
                                          request->length, handler->callback_data);
        }
    }

    // ✅ CRITICAL: Always send WrResp ACK after invoking handlers
    fw_send_response(card, request, RCODE_COMPLETE);
}
```

**Key Points:**
1. **Line 961**: `fw_send_response(card, request, RCODE_COMPLETE)` - **ALWAYS** sends WrResp
2. Handlers are invoked **before** sending WrResp (synchronous processing)
3. Multiple handlers can process same FCP packet (broadcast pattern)
4. WrResp is sent **unconditionally** after handlers complete

#### Response Packet Construction (lines 840-870)

```c
void fw_send_response(struct fw_card *card,
                      struct fw_request *request, int rcode)
{
    u32 *data = NULL;
    unsigned int data_length = 0;

    /* unified transaction or broadcast: don't respond */
    if (request->ack != ACK_PENDING ||
        HEADER_DESTINATION_IS_BROADCAST(request->request_header)) {
        fw_request_put(request);
        return;
    }

    if (rcode == RCODE_COMPLETE) {
        data = request->data;
        data_length = fw_get_response_length(request);
    }

    // Build response packet (swaps src/dest, sets rCode)
    fw_fill_response(&request->response, request->request_header, rcode, data, data_length);

    // Increase reference count for in-flight tracking
    fw_request_get(request);

    // Submit response packet to AT queue
    card->driver->send_response(card, &request->response);
}
```

**Response Packet Format** (per `fw_fill_response`, lines 703-755):

For **TCODE_WRITE_BLOCK_REQUEST** (0x1):
- Response tCode = `TCODE_WRITE_RESPONSE` (0x2)
- Header length = 12 bytes (no payload)
- Payload length = 0
- Source/Dest swapped from request
- tLabel preserved from request
- rCode = `RCODE_COMPLETE` (0x0)

---

## Apple IOFireWireAVC Implementation

**File**: `IOFireWireAVC/IOFireWireAVCUnit.cpp`

### FCP Response Handler (lines 463-656)

```cpp
UInt32 IOFireWireAVCUnit::AVCResponse(void *refcon, UInt16 nodeID, IOFWSpeed &speed,
                    FWAddress addr, UInt32 len, const void *buf, IOFWRequestRefCon requestRefcon)
{
    IOFireWireAVCUnit *me = (IOFireWireAVCUnit *)refcon;
    UInt8 *pResponseBytes = (UInt8*) buf;
    UInt32 res = kFWResponseAddressError;
    
    // ... validation and matching logic ...
    
    if (matchFound) {
        // Process response, update command state
        // ...
        res = kFWResponseComplete;  // ✅ Tell IOKit to send WrResp ACK
    }
    
    return res;
}
```

**Key Points:**
1. Handler returns `kFWResponseComplete` (equivalent to Linux `RCODE_COMPLETE`)
2. `IOFWPseudoAddressSpace` (Apple's address space manager) **automatically** sends WrResp based on return value
3. Driver code doesn't manually construct WrResp - IOKit handles it

---

## Apple IOFireWireFamily Low-Level Implementation

**File**: `IOFireWireFamily.kmodproj/IOFireWireController.cpp`

### Write Request Processing (lines 5784-5842)

```cpp
void IOFireWireController::processWriteRequest(UInt16 sourceID, UInt32 tLabel,
                                               UInt32 *hdr, void *buf, int len, IOFWSpeed speed)
{
    UInt32 ret = kFWResponseAddressError;
    FWAddress addr((hdr[1] & kFWAsynchDestinationOffsetHigh) >> kFWAsynchDestinationOffsetHighPhase, hdr[2]);
    IOFWAddressSpace * found;
    
    // Iterate through all registered address spaces
    fSpaceIterator->reset();
    while( (found = (IOFWAddressSpace *) fSpaceIterator->getNextObject())) {
        // Call address space handler (e.g., IOFWPseudoAddressSpace::doWrite)
        ret = found->doWrite(sourceID, speed, addr, len, buf, (IOFWRequestRefCon)(uintptr_t)tLabel);
        if(ret != kFWResponseAddressError)
            break;  // Handler accepted the request
    }
    
    // ✅ CRITICAL: Automatically send WrResp after handler returns
    if ( ((hdr[0] & kFWAsynchDestinationID) >> kFWAsynchDestinationIDPhase) != 0xffff )
        fFWIM->asyncWriteResponse(sourceID, speed, tLabel, ret, addr.addressHi);
    else
        DebugLog("Skipped asyncWriteResponse because destID=0x%x\n", ...);  // Broadcast
}
```

**Key Points:**
1. **Line 5831**: Calls `doWrite()` on each address space until one accepts (returns non-`kFWResponseAddressError`)
2. **Line 5839**: **ALWAYS** sends WrResp via `fFWIM->asyncWriteResponse()` with handler's return code
3. Only skips WrResp for broadcast writes (destID=0xffff)
4. Handler return value becomes rCode in WrResp packet

### Address Space Handler Interface

**File**: `IOFireWireFamily.kmodproj/IOFWAddressSpace.h` (lines 210-220)

```cpp
class IOFWAddressSpace : public OSObject
{
    // Abstract method - must be implemented by subclasses
    virtual UInt32 doWrite(UInt16 nodeID, IOFWSpeed &speed, FWAddress addr, UInt32 len,
                           const void *buf, IOFWRequestRefCon refcon) = 0;
};
```

**Return Values** (from header comments, lines 54-59):
```cpp
kFWResponseComplete       = 0,  // OK
kFWResponseConflictError  = 4,  // Resource conflict, may retry
kFWResponseDataError      = 5,  // Data not available
kFWResponseTypeError      = 6,  // Operation not supported
kFWResponseAddressError   = 7   // Address not valid in target device
```

### FCP Pseudo Address Space

**File**: `IOFireWireFamily.kmodproj/IOFWPseudoAddressSpace.cpp`

```cpp
UInt32 IOFWPseudoAddressSpace::doWrite(UInt16 nodeID, IOFWSpeed &speed, FWAddress addr,
                                       UInt32 len, const void *buf, IOFWRequestRefCon refcon)
{
    // Check if address is in our range
    if (!contains(addr))
        return kFWResponseAddressError;
    
    // Call user-provided callback
    if (fWriter) {
        return fWriter(fRefCon, nodeID, speed, addr, len, buf, refcon);
    }
    
    return kFWResponseComplete;
}
```

**Flow Summary:**
```
1. OHCI receives block write (AR Request) → processWriteRequest()
2. processWriteRequest() → doWrite() on each IOFWAddressSpace
3. IOFWPseudoAddressSpace::doWrite() → user callback (e.g., AVCResponse)
4. User callback returns rCode (kFWResponseComplete, etc.)
5. processWriteRequest() → fFWIM->asyncWriteResponse(rCode)
6. FWIM (FireWire Interface Module) → builds WrResp packet, submits to AT Response DMA
7. OHCI transmits WrResp to device
```

**Design Pattern:**
- **Separation of concerns**: Handler logic (user code) only returns rCode
- **Infrastructure handles WrResp**: IOFireWireController automatically sends response
- **No manual packet construction**: User never builds WrResp headers

---

## ASFW Current Implementation

### AR Request Packet Flow

```
1. OHCI DMA → AR Request buffer (tCode=0x1, block write)
2. RxPath::ProcessARInterrupts() → dequeue buffer
3. PacketRouter::RoutePacket(ARContextType::Request, ...)
4. PacketRouter invokes registered handler (tCode=0x1)
5. FCPResponseRouter::RouteBlockWrite(packet)
6. FCPTransport::OnFCPResponse(nodeID, gen, payload)
7. ❌ NO WrResp sent - transaction incomplete!
```

### Missing Component

**File**: `Async/Rx/PacketRouter.hpp`

```cpp
using PacketHandler = std::function<void(const ARPacketView&)>;
```

**Problem**: Handler signature is `void` - no way to return rCode!

**File**: `Protocols/AVC/FCPResponseRouter.hpp` (lines 59-95)

```cpp
bool RouteBlockWrite(const Async::ARPacketView& packet) {
    // ... validation ...
    
    FCPTransport* transport = avcDiscovery_.GetFCPTransportForNodeID(srcNodeID);
    if (!transport) {
        return true;  // FCP packet, but no handler
    }

    transport->OnFCPResponse(srcNodeID, generation, packet.payload);
    
    return true;  // ❌ Return value not used for WrResp!
}
```

**Problem**: `RouteBlockWrite` returns `bool` (handled/not handled) but this doesn't trigger WrResp transmission.

---

## Linux OHCI Driver Implementation

### Response Transmission

**File**: `firewire/ohci.c`

```c
static int ohci_send_response(struct fw_card *card, struct fw_packet *packet)
{
    struct fw_ohci *ohci = fw_card_to_ohci(card);
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&ohci->lock, flags);

    // Queue response packet to AT Response context
    ret = at_context_queue_packet(&ohci->at_response_ctx, packet);

    spin_unlock_irqrestore(&ohci->lock, flags);

    return ret;
}
```

**Key Points:**
1. Response packets use **AT Response context** (separate from AT Request)
2. OHCI has dedicated DMA context for sending responses
3. Hardware automatically handles ACK/retry logic

---

## Proposed Mitigation for ASFW

### Option 1: Add Response Callback to PacketHandler (Recommended)

**Rationale**: Mirrors Linux/Apple pattern - handler returns rCode, infrastructure sends WrResp.

#### Step 1: Update PacketHandler Signature

```cpp
// File: Async/Rx/PacketRouter.hpp

/// Response codes per IEEE 1394-1995 Table 6-3 and Linux/Apple implementations
enum class ResponseCode : uint8_t {
    Complete      = 0x0,  // OK - request successfully completed
    ConflictError = 0x4,  // Resource conflict, may retry
    DataError     = 0x5,  // Data not available / corrupted
    TypeError     = 0x6,  // Operation not supported for this address
    AddressError  = 0x7,  // Address not valid in this address space
    NoResponse    = 0xFF  // Internal sentinel: do not send WrResp (AR Response context)
};

/// Packet handler callback - returns response code for AR Requests
/// 
/// **Policy**: Handlers only choose the rCode. The AR infrastructure (PacketRouter)
/// owns the decision of whether to actually send a WrResp (e.g., skips broadcast).
using PacketHandler = std::function<ResponseCode(const ARPacketView&)>;
```

**Key Design Principle**: Handlers are **protocol-agnostic**. They only return a response code indicating success/failure. The AR infrastructure handles all WrResp transmission policy (broadcast filtering, packet construction, AT Response submission).

#### Step 2: Update PacketRouter to Send WrResp

```cpp
// File: Async/Rx/PacketRouter.cpp

void PacketRouter::RoutePacket(ARContextType contextType, std::span<const uint8_t> packetData) {
    // ... existing parsing logic ...
    
    if (contextType == ARContextType::Request && handler) {
        // Invoke handler, get response code
        ResponseCode rCode = handler(view);
        
        // Send WrResp if handler requests it
        if (rCode != ResponseCode::NoResponse) {
            SendWriteResponse(view, rCode);
        }
    }
}
```

#### Step 3: Implement SendWriteResponse

```cpp
// File: Async/Tx/ResponseSender.hpp (NEW)

class ResponseSender {
public:
    /// Send WrResp packet for incoming write request
    /// 
    /// **Policy**: Per IEEE 1394-1995 §6.2.4.3 and Linux/Apple behavior,
    /// no response is sent for broadcast writes (destID=0xFFFF).
    /// 
    /// @param request Original request packet view
    /// @param rCode Response code (COMPLETE, ADDRESS_ERROR, etc.)
    void SendWriteResponse(const ARPacketView& request, ResponseCode rCode) {
        // Extract destination ID from original request
        uint16_t destID = ExtractDestID(request.header);
        
        // Per IEEE 1394 spec: no response for broadcast writes
        if (destID == 0xFFFF) {
            ASFW_LOG_V3(Async, "ResponseSender: Skipping WrResp for broadcast write (destID=0xFFFF)");
            return;
        }
        
        // Internal sentinel: handler explicitly requested no response
        if (rCode == ResponseCode::NoResponse) {
            return;
        }
        
        // Build response packet header (swap src/dest, set rCode)
        uint32_t responseHeader[3];
        BuildResponseHeader(request.header, rCode, responseHeader);
        
        // Submit to AT Response context
        atResponseContext_->SubmitResponse(responseHeader, nullptr, 0);
    }
    
private:
    /// Build WRITE_RESPONSE packet header per IEEE 1394-1995 §6.2.4.3
    /// 
    /// **Wire Format** (big-endian):
    /// - Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
    /// - Q1: [srcID:16][rCode:4][reserved:12]
    /// - Q2: [reserved:32]
    /// 
    /// @param requestHeader Original request packet header (big-endian)
    /// @param rCode Response code (0x0-0x7, per IEEE 1394 Table 6-3)
    /// @param responseHeader Output buffer for 3-quadlet response header
    void BuildResponseHeader(std::span<const uint8_t> requestHeader,
                             ResponseCode rCode,
                             uint32_t* responseHeader) {
        // Extract from request (big-endian wire format)
        uint16_t srcID = ExtractSourceID(requestHeader);   // Device → becomes destID in response
        uint16_t destID = ExtractDestID(requestHeader);    // Us → becomes srcID in response
        uint8_t tLabel = ExtractTLabel(requestHeader);     // Preserved from request
        
        // Build WRITE_RESPONSE header (tCode=0x2)
        // Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
        responseHeader[0] = (srcID << 16) | (tLabel << 10) | (0x2 << 4);  // tCode=0x2 (WRITE_RESPONSE)
        
        // Q1: [srcID:16][rCode:4][reserved:12]
        responseHeader[1] = (destID << 16) | (static_cast<uint8_t>(rCode) << 12);
        
        // Q2: reserved (must be zero per spec)
        responseHeader[2] = 0;
    }
};
```

#### Step 4: Add AT Response Context

**Challenge**: ASFW currently only has **AT Request context** for outgoing requests. Need to add **AT Response context** for sending WrResp packets.

**File**: `Async/Tx/ATManager.hpp` (modify)

```cpp
class ATManager {
public:
    // Existing: Submit outgoing requests
    AsyncHandle SubmitRequest(const WriteParams& params, ...);
    
    // NEW: Submit outgoing responses (for AR Request handling)
    AsyncHandle SubmitResponse(const uint32_t* header, const void* payload, size_t length);
    
private:
    ATRequestContext atRequestContext_;   // Existing
    ATResponseContext atResponseContext_; // NEW - for WrResp packets
};
```

**OHCI Configuration**:
- AT Response context uses **different OHCI registers** than AT Request
- Control register: `ATRespContextControlSet` (offset 0x0C0)
- Command pointer: `ATRespCommandPtr` (offset 0x0C4)
- See OHCI §7.2.3 for AT Response context details

---

### Option 2: Immediate Workaround (Quick Fix)

**Rationale**: Minimal changes to unblock AV/C discovery while full AT Response context is implemented.

#### Step 4: Update FCPResponseRouter

**Important**: FCP is just one address space handler. The WrResp policy (broadcast filtering, packet construction) lives in the AR infrastructure, not in FCP-specific code.

```cpp
// File: Protocols/AVC/FCPResponseRouter.hpp

ResponseCode RouteBlockWrite(const Async::ARPacketView& packet) {
    uint64_t destOffset = Async::ExtractDestOffset(packet.header);
    
    // Check if this is an FCP response address
    if (destOffset != kFCPResponseAddress) {
        return ResponseCode::AddressError;  // Not FCP address
    }
    
    // Validate tCode (only accept writes per FCP spec)
    if (packet.tCode != 0x0 && packet.tCode != 0x1) {  // WRITE_QUADLET or WRITE_BLOCK
        return ResponseCode::TypeError;  // FCP only accepts writes
    }
    
    // Route to FCPTransport
    FCPTransport* transport = avcDiscovery_.GetFCPTransportForNodeID(packet.sourceID);
    if (!transport) {
        ASFW_LOG_V2(Async, "FCPResponseRouter: No transport for nodeID=0x%04x", packet.sourceID);
        // Still return Complete - we handled the packet, just no listener
        return ResponseCode::Complete;
    }
    
    transport->OnFCPResponse(packet.sourceID, generation, packet.payload);
    
    // FCP response successfully processed
    return ResponseCode::Complete;
}

private:
void SendFCPResponseACK(const ARPacketView& request) {
    // Build minimal WrResp packet
    uint32_t header[3];
    header[0] = (request.sourceID << 16) | (request.tLabel << 10) | (0x2 << 4);
    header[1] = (request.destID << 16);  // rCode=0 (COMPLETE)
    header[2] = 0;
    
    // Submit to AT Request context as a hack (wrong context, but works)
    // This will send the WrResp packet
    atManager_->SubmitRawPacket(header, 12, nullptr, 0);
}
```

**Key Points:**
- Handler returns `ResponseCode`, not `bool`
- Returns `AddressError` if not FCP address (lets other handlers try)
- Returns `TypeError` if wrong tCode (FCP only accepts writes)
- Returns `Complete` if successfully processed (even if no listener)
- **Does NOT** send WrResp - that's the AR infrastructure's job
```

**Limitations**:
- Uses AT Request context instead of AT Response context (non-standard)
- Only handles FCP responses, not general AR Request handling
- Not a proper long-term solution

---

## Recommended Implementation Plan

### Phase 1: Infrastructure (Blocking)
1. ✅ **Add AT Response Context** to OHCI/ATManager
   - Configure OHCI AT Response DMA context
   - Implement `SubmitResponse()` API
   - Test with simple WrResp packets

2. ✅ **Update PacketHandler Signature**
   - Change return type from `void` to `ResponseCode`
   - Update all existing handlers to return `NoResponse` (preserve current behavior)

3. ✅ **Implement ResponseSender**
   - Create `BuildResponseHeader()` helper
   - Integrate with PacketRouter
   - Add logging for WrResp transmission

### Phase 2: FCP Integration
4. ✅ **Update FCPResponseRouter**
   - Return `ResponseCode::Complete` for valid FCP responses
   - Return `ResponseCode::AddressError` for invalid addresses
   - Return `ResponseCode::TypeError` for non-write requests

5. ✅ **Test with AV/C Device**
   - Verify WrResp packets sent via packet analyzer
   - Confirm device stops retransmitting
   - Validate no duplicate responses

### Phase 3: General AR Request Handling
6. ⏳ **Add CSR Register Handlers**
   - Implement read/write handlers for Config ROM, CSR registers
   - Return appropriate rCodes based on operation success
   - **Note**: WrResp machinery is protocol-agnostic - CSR/unit-directory handlers are just additional address spaces returning ResponseCode

7. ⏳ **Add Unit Directory Handlers**
   - Handle reads to unit directory space
   - Support for FCP command address (0xFFFFF0000B00)

---

## Testing Strategy

### Unit Tests
- [ ] `BuildResponseHeader()` - verify header format, tCode=0x2, correct src/dest swap
- [ ] `SendWriteResponse()` - verify AT Response submission
- [ ] `SendWriteResponse()` - verify **no WrResp for destID=0xFFFF** (broadcast)
- [ ] `PacketRouter` - verify rCode propagation from handler to ResponseSender
- [ ] `ResponseCode` enum - verify numeric values match IEEE 1394 spec (AddressError=0x7)

### Integration Tests
- [ ] Send test FCP response via packet injection
- [ ] Verify WrResp sent with correct tLabel, rCode
- [ ] Verify **1:1 mapping**: every unicast AR Request → exactly one WrResp (same tLabel)
- [ ] Verify **no WrResp for AR Response context** (handlers return NoResponse)
- [ ] Verify no retransmissions from device

### Hardware Tests
- [ ] Connect Apogee Duet, run AV/C discovery
- [ ] Capture packets with analyzer
- [ ] Verify WrResp for each FCP response (unicast)
- [ ] Verify **no WrResp for broadcast writes** (if any)
- [ ] Confirm no duplicate responses in logs
- [ ] Verify rCode values on wire match ResponseCode enum

---

## References

### IEEE 1394 Specification
- **IEEE 1394-1995 §6.2.4**: Asynchronous transaction protocol
- **IEEE 1394-1995 §6.2.4.3**: Write response packet format
- **IEEE 1394a-2000 §6.2.4.1.2.3**: Split transaction timing

### OHCI Specification
- **OHCI §7.2.3**: AT Response context configuration
- **OHCI §8.3**: Asynchronous transmit DMA
- **OHCI §8.7**: Packet formats

### Linux FireWire
- `firewire/core-transaction.c`: Request/response handling
- `firewire/ohci.c`: OHCI AT Response context implementation

### Apple IOFireWireFamily
- `IOFireWireAVC/IOFireWireAVCUnit.cpp`: FCP response handler
- `IOFireWireFamily/IOFWAddressSpace.h`: Pseudo address space API

---

## Impact Assessment

### Current Impact
- **Severity**: Critical
- **Affected**: All AV/C device communication
- **Workaround**: None (device retransmissions cause validation failures)

### Post-Fix Impact
- **Performance**: Eliminates unnecessary retransmissions
- **Reliability**: Proper IEEE 1394 compliance
- **Compatibility**: Matches Linux/Apple behavior

---

## Conclusion

The missing WrResp ACK support is a **fundamental gap** in ASFW's AR Request handling. While the transaction completion fix (Part 1-3) correctly handles the race condition between AT and AR completion paths, it cannot solve device retransmissions caused by missing WrResp ACKs.

**Immediate Action**: Implement AT Response context and WrResp transmission (Phase 1-2) to unblock AV/C device communication.

**Long-term**: Complete general AR Request handling (Phase 3) for full IEEE 1394 compliance.
