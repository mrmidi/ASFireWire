# OHCI AT DMA Program Comparison

This document explains the five different approaches to building AT (Asynchronous Transmit) DMA programs for OHCI FireWire controllers, as demonstrated by `dma_program_test.py`.

## Mode Comparison

### PATH1: Program CommandPtr + RUN per request (no chaining)

**Approach**: One descriptor per request, manually start/stop DMA for each packet.

**Descriptor Layout** (32B per packet):
- Single OUTPUT_LAST+IMMEDIATE descriptor
- CommandPtr points to descriptor with Z=2 (32B)
- No BRANCH chaining between packets

**Workflow**:
1. Write descriptor to memory
2. Set CommandPtr register to descriptor address | Z
3. Set RUN=1 to start DMA
4. Wait for completion
5. Set RUN=0 to stop DMA
6. Repeat for next packet

**Pros**:
- Simple, deterministic per-packet control
- Easy to debug individual packets
- No complex chaining logic

**Cons**:
- Higher CPU overhead (register writes per packet)
- Slower for multiple packets
- No automatic pipelining

---

### PATH2: Start once, append by patching BRANCH and pulsing WAKE

**Approach**: Start DMA once, then dynamically chain packets by patching BRANCH field.

**Descriptor Layout** (32B per packet):
- Single OUTPUT_LAST+IMMEDIATE descriptor
- BRANCH field patched after DMA starts
- WAKE pulse tells controller to check for new work

**Workflow**:
1. Write first descriptor, set CommandPtr, RUN=1
2. For subsequent packets:
   - Write new descriptor to memory
   - Patch previous descriptor's BRANCH to point to new one
   - Memory barrier (flush to device)
   - Pulse WAKE bit
3. Controller follows BRANCH chain automatically

**Pros**:
- Lower overhead after initial start
- Automatic pipelining by hardware
- Used by Apple's production driver

**Cons**:
- Requires careful memory ordering
- More complex descriptor management
- Race conditions if not careful with WAKE timing

---

### LINUX: Multi-descriptor layout with branch chaining

**Approach**: Linux firewire-ohci style using 4 descriptors per packet, build entire chain upfront.

**Descriptor Layout** (64B per packet = 4 × 16B descriptors):

```
d[0]: KEY_IMMEDIATE descriptor
  - req_count = packet header length (12B)
  - control = KEY_IMMEDIATE | OUTPUT_LAST | IRQ_ALWAYS | BRANCH_ALWAYS
  - data_address = pointer to d[1] (immediate header storage)
  - branch_address = next packet's d[0] address | Z=3

d[1]: Immediate header storage (16B memory block)
  - Contains 12B IEEE 1394 packet header
  - Not interpreted as descriptor by HW

d[2]: Payload descriptor (unused for quadlet reads)
  - Reserved for block transfers
  - Zero for quadlet reads

d[3]: Driver metadata storage
  - Packet state, pointers, etc.
  - Not touched by hardware
```

**Workflow**:
1. Build entire chain of packets in memory
2. Set BRANCH in each d[0] to point to next packet's d[0]
3. Last packet has branch_address = 0
4. Set CommandPtr to first packet (Z=3 for 4 descriptors)
5. Set RUN=1 once
6. Controller processes entire chain automatically

**Pros**:
- Full hardware automation
- All packets submitted at once
- Standard Linux kernel approach
- Proven compatibility across chipsets

**Cons**:
- More memory per packet (64B vs 32B)
- Complex descriptor structure
- Can't easily insert packets mid-flight

---

### APPLE1: Apple simple mode (always stop after transmission)

**Approach**: Software state byte tracking, always stop after each packet completes.

**Descriptor Layout** (32B per packet):
- Single OUTPUT_LAST+IMMEDIATE descriptor
- BRANCH field always 0 (no chaining)
- Software state byte at context+28 (0=IDLE, 1=RUNNING)

**Workflow**:
1. Check state byte: always 0 (IDLE) for every request
2. Use PATH 1: Write CommandPtr, set RUN=1
3. Set state byte = 1 (RUNNING)
4. **Immediately call stopDMAAfterTransmit():**
   - Wait for descriptor status (15 × 5µs polling)
   - Poll ACTIVE=0 (WaitForDMA)
   - Clear RUN bit
   - Set state byte = 0 (IDLE)
   - Clear tail pointer
5. Repeat for next packet (always PATH 1)

**Pros**:
- **Prevents Z=0 append bug** - state always resets to IDLE
- Simple, deterministic behavior
- No race conditions with hardware state
- Same pattern for all requests regardless of timing

**Cons**:
- Highest overhead (stop/start per packet)
- No pipelining benefits
- Synchronous completion wait before next submit

**Key Insight**: Apple discovered that tracking **software intent** (state byte) rather than **hardware state** (ACTIVE bit) prevents the Z=0 append bug where hardware caches Z=0 and subsequent branch patches fail.

---

### APPLE2: Apple hybrid mode (needsFlush flag controls stop)

**Approach**: Software state byte + per-descriptor needsFlush flag for selective stopping.

**Descriptor Layout** (32B per packet):
- Single OUTPUT_LAST+IMMEDIATE descriptor
- needsFlush flag at element+40 (0=keep running, 1=stop after tx)
- State byte determines PATH 1 vs PATH 2

**Workflow**:
1. **First request** (state byte = 0 → IDLE):
   - Use PATH 1: CommandPtr + RUN
   - Set state byte = 1 (RUNNING)
   - If needsFlush=1: stopDMAAfterTransmit() → state byte = 0
   - If needsFlush=0: Keep running, return

2. **Subsequent requests** (depends on previous needsFlush):
   - If state byte = 0 (previous had needsFlush=1):
     * Use PATH 1 (fresh CommandPtr programming)
   - If state byte = 1 (previous had needsFlush=0):
     * Use PATH 2 (patch prev.branchWord + WAKE)
   - After submission:
     * If needsFlush=1: stop and reset state byte = 0
     * If needsFlush=0: keep running

**needsFlush Flag Logic** (from asyncWrite @ 0xEDCA):
```cpp
commandElement[10] = 0;  // Default: no flush
if (writeFlags & kIOFWWriteBlockRequest) {
    commandElement[10] = 1;  // Set needsFlush for block writes
}
```

**Pros**:
- **Best of both worlds**: Simple ops pipeline, complex ops get coherency
- Prevents Z=0 append bug (state byte tracking)
- Efficient for quadlet bursts (keep running)
- Safe for block writes (explicit stop for cache coherency)

**Cons**:
- More complex logic (per-packet flag + state tracking)
- Client must set needsFlush correctly
- Still synchronous wait when needsFlush=1

**Use Cases**:
- **needsFlush=0**: Quadlet reads/writes, Config ROM discovery
- **needsFlush=1**: Block writes with scatter/gather DMA, explicit drain requests

---

## Key Technical Details

### Z Field (Descriptor Count)
- **Z=2**: 2 descriptors = 32B (PATH1, PATH2)
- **Z=3**: 3-4 descriptors = 64B (LINUX)
- Encoded in low 4 bits of CommandPtr/branch_address

### Control Field Bits
```
DESCRIPTOR_OUTPUT_LAST    = (1 << 12)  // Last descriptor in packet
DESCRIPTOR_KEY_IMMEDIATE  = (2 << 8)   // Immediate data (not DMA from buffer)
DESCRIPTOR_IRQ_ALWAYS     = (3 << 4)   // Generate IRQ on completion
DESCRIPTOR_BRANCH_ALWAYS  = (3 << 2)   // Always follow branch (not conditional)
```

### Memory Ordering
- **PATH2**: Requires `wmb()` (write memory barrier) after descriptor update, before WAKE
- **LINUX**: Uses `cpu_to_le32()` for endianness + `wmb()` before branch update
- **APPLE1/2**: Uses `OSSynchronizeIO()` memory barrier before register writes

### CommandPtr Format
```
CommandPtr = (descriptor_address & 0xFFFFFFF0) | Z
```
- Address must be 16-byte aligned
- Z indicates how many descriptors to fetch

---

## Implementation Recommendations

**For Discovery/Config ROM Reading** (current ASFW use case):
- **APPLE2** is ideal (hybrid mode)
  - Matches proven Apple implementation
  - Efficient pipelining for quadlet bursts (needsFlush=0)
  - State byte prevents Z=0 append bug
- **Alternative**: ASFW's current PATH2 with AR-side stop
  - Good balance of performance and control
  - Allows dynamic request insertion
  - Requires watchguards for hardware quirks

**For High-Throughput Streaming**:
- **LINUX** style preferred
  - Maximum hardware automation
  - Best for bulk transfers
  - Proven on many chipsets
- **Alternative**: APPLE2 with needsFlush=0 for entire stream

**For Hardware Compatibility/Quirks**:
- **APPLE1** is safest (simple mode)
  - No reliance on WAKE working correctly
  - Fresh CommandPtr every time
  - Handles Agere/LSI quirks gracefully
- **PATH1** also works but lacks state byte protection

**For Debugging/Testing**:
- **PATH1** is simplest
  - Full visibility per packet
  - Easiest to trace
  - Good for hardware bring-up
- **APPLE1** adds state tracking visibility

---

## Critical Architectural Insight: State Byte vs Hardware ACTIVE

### The Z=0 Append Bug

**Root Cause**: Using hardware ACTIVE bit to decide PATH 1 vs PATH 2

**Failure Sequence**:
1. Request 1: Use PATH 1, program CommandPtr with Z=2
2. Hardware fetches, transmits, completes → ACTIVE goes 0
3. Driver sees ACTIVE=0, thinks "context idle, use PATH 2"
4. Driver patches previous descriptor BRANCH from 0 to (newAddr|Z=2)
5. **Problem**: Hardware already cached Z=0 and stopped fetch engine
6. Driver issues WAKE → ACTIVE goes 1 but fetch engine doesn't restart
7. New descriptor never transmits → timeout

**Apple's Solution**: Internal software state byte at context+28
- **Value 0 (IDLE)**: Use PATH 1 (CommandPtr programming)
- **Value 1 (RUNNING)**: Use PATH 2 (branch patch + WAKE)
- **State transitions**:
  * 0→1: When RUN bit is set (PATH 1 execution)
  * 1→0: When stopDMAAfterTransmit() completes
  * 1→1: When PATH 2 succeeds (context stays running)

**Why This Works**:
- State byte reflects **software submission state**, not hardware DMA state
- IDLE state guarantees hardware has no cached descriptor information
- RUNNING state guarantees hardware fetch engine is active and can accept WAKE
- No race condition: state changes happen synchronously in driver code

**ASFW Implementation**:
- Uses `contextRunning_` boolean flag (equivalent to Apple's state byte)
- AR-side stop: Sets `contextRunning_=false` when outstanding queue empties
- Multi-factor PATH decision: Checks `contextRunning_ && hwRunBit && ringNonEmpty`
- Watchguards: Pre-WAKE RUN check, 7ms ACTIVE poll, PATH 1 fallback

---

## Testing the Script

```bash
# Show PATH1 approach (manual start/stop)
python3 dma_program_test.py path1

# Show PATH2 approach (dynamic chaining)
python3 dma_program_test.py path2

# Show Linux multi-descriptor approach
python3 dma_program_test.py linux

# Show Apple simple mode (always stop)
python3 dma_program_test.py apple1

# Show Apple hybrid mode (needsFlush flag)
python3 dma_program_test.py apple2
```

Each mode generates the same logical transaction (read Config ROM at 0xF0000400, 0x408, 0x40C, 0x410) but uses different DMA programming strategies.

---

## Mode Comparison Summary Table

| Feature | PATH1 | PATH2 | LINUX | APPLE1 | APPLE2 |
|---------|-------|-------|-------|--------|--------|
| **Descriptor Size** | 32B | 32B | 64B (4×16B) | 32B | 32B |
| **State Tracking** | None | Manual | ctx->running | State byte | State byte + flag |
| **Chaining** | None | Dynamic (WAKE) | Pre-built | None | Conditional |
| **Stop Behavior** | Per packet | AR-side | Never | Every packet | Per needsFlush |
| **DMA Start** | CommandPtr+RUN | CommandPtr+RUN | CommandPtr+RUN | CommandPtr+RUN | CommandPtr+RUN |
| **DMA Append** | N/A | WAKE pulse | Auto-chain | N/A | WAKE pulse |
| **Z=0 Bug Risk** | Medium | Medium | Low | **None** | **None** |
| **Performance** | Low | Medium-High | High | Low | Medium-High |
| **Complexity** | Low | Medium | High | Low | Medium |
| **Quirk Tolerance** | Medium | Medium | High | **High** | **High** |
| **Use Case** | Debug/Test | ASFW Current | Streaming | Compatibility | Production |

**Key Takeaways**:
- **APPLE1/2 eliminate Z=0 bug** through software state byte tracking
- **LINUX achieves high performance** through hardware automation but requires uncached DMA
- **PATH2 balances** performance and control but needs watchguards for quirks
- **APPLE2 is optimal** for most use cases: efficient, safe, and proven in production
