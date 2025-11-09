# AT DMA Programming Modes - Visual Comparison

## State Transitions for Each Mode

### PATH1: Simple Start/Stop
```
┌─────────────────────────────────────────┐
│  Request N arrives                      │
│  ↓                                      │
│  Program CommandPtr(desc_N)             │
│  ↓                                      │
│  Set RUN=1                              │
│  ↓                                      │
│  [Hardware transmits packet]            │
│  ↓                                      │
│  Wait for completion                    │
│  ↓                                      │
│  Clear RUN=0                            │
│  ↓                                      │
│  Context IDLE                           │
└─────────────────────────────────────────┘
                ↓ (next request)
         [Repeat same pattern]
```

### PATH2: Dynamic Chaining with WAKE
```
Request 1:
┌─────────────────────────────────────────┐
│  contextRunning = false                 │
│  ↓                                      │
│  Program CommandPtr(desc_1)             │
│  ↓                                      │
│  Set RUN=1                              │
│  ↓                                      │
│  contextRunning = true                  │
└─────────────────────────────────────────┘

Request 2 (while running):
┌─────────────────────────────────────────┐
│  contextRunning = true                  │
│  ↓                                      │
│  Patch prev.branch = (desc_2 | Z)       │
│  ↓                                      │
│  Memory barrier (OSSynchronizeIO)       │
│  ↓                                      │
│  Set WAKE=1 (pulse)                     │
└─────────────────────────────────────────┘

On completion queue empty:
┌─────────────────────────────────────────┐
│  outstanding.InUse() == 0               │
│  ↓                                      │
│  Clear RUN=0                            │
│  ↓                                      │
│  contextRunning = false                 │
└─────────────────────────────────────────┘
```

### LINUX: Pre-Built Chain
```
Submit batch:
┌─────────────────────────────────────────┐
│  Build entire chain in memory:         │
│    desc_1.branch → desc_2               │
│    desc_2.branch → desc_3               │
│    desc_3.branch → desc_4               │
│    desc_4.branch → 0                    │
│  ↓                                      │
│  wmb() - write memory barrier           │
│  ↓                                      │
│  Program CommandPtr(desc_1)             │
│  ↓                                      │
│  Set RUN=1                              │
│  ↓                                      │
│  [Hardware processes entire chain]      │
│  ↓                                      │
│  ctx->running = true (until bus reset)  │
└─────────────────────────────────────────┘
```

### APPLE1: Always Stop (Simple Mode)
```
┌─────────────────────────────────────────┐
│  Request N arrives                      │
│  ↓                                      │
│  state_byte (this+28) = 0 (IDLE)        │
│  ↓                                      │
│  Program CommandPtr(desc_N)             │
│  ↓                                      │
│  Set RUN=1                              │
│  ↓                                      │
│  state_byte = 1 (RUNNING)               │
│  ↓                                      │
│  ⚠️ stopDMAAfterTransmit() immediate:   │
│    • Wait descriptor status (15×5µs)    │
│    • WaitForDMA() → poll ACTIVE=0       │
│    • Clear RUN=0                        │
│    • state_byte = 0 (IDLE)              │
│    • this+6 = 0 (clear tail)            │
└─────────────────────────────────────────┘
                ↓ (next request)
    [Always sees state_byte=0 → PATH 1]
```

### APPLE2: Hybrid Mode (needsFlush Flag)
```
Request 1 (state_byte=0, needsFlush=0):
┌─────────────────────────────────────────┐
│  state_byte = 0 (IDLE) → PATH 1         │
│  ↓                                      │
│  Program CommandPtr(desc_1)             │
│  ↓                                      │
│  Set RUN=1                              │
│  ↓                                      │
│  state_byte = 1 (RUNNING)               │
│  ↓                                      │
│  needsFlush = 0 → Keep running, return  │
└─────────────────────────────────────────┘

Request 2 (state_byte=1, needsFlush=0):
┌─────────────────────────────────────────┐
│  state_byte = 1 (RUNNING) → PATH 2      │
│  ↓                                      │
│  Patch prev.branch = (desc_2 | Z)       │
│  ↓                                      │
│  Set WAKE=1                             │
│  ↓                                      │
│  needsFlush = 0 → Keep running          │
└─────────────────────────────────────────┘

Request 3 (state_byte=1, needsFlush=1):
┌─────────────────────────────────────────┐
│  state_byte = 1 (RUNNING) → PATH 2      │
│  ↓                                      │
│  Patch prev.branch = (desc_3 | Z)       │
│  ↓                                      │
│  Set WAKE=1                             │
│  ↓                                      │
│  needsFlush = 1 → stopDMAAfterTransmit()│
│    • state_byte = 0 (IDLE)              │
└─────────────────────────────────────────┘

Request 4 (state_byte=0, needsFlush=0):
┌─────────────────────────────────────────┐
│  state_byte = 0 (IDLE) → PATH 1         │
│  [Repeat from Request 1 pattern]        │
└─────────────────────────────────────────┘
```

## Critical Difference: State Tracking

### ❌ WRONG: Using Hardware ACTIVE Bit
```
Request 1:
  CommandPtr(desc_1) → RUN=1
  [Hardware: ACTIVE 0→1→0 during transmission]
  
Request 2:
  Driver sees ACTIVE=0 ✗
  Thinks: "Context idle, can use PATH 2"
  Patches desc_1.branch = (desc_2 | Z=2)
  Issues WAKE
  
  ⚠️ BUG: Hardware already cached Z=0!
  - Fetch engine stopped with Z=0 knowledge
  - WAKE sets ACTIVE=1 but doesn't restart fetch
  - desc_2 never transmits → timeout
```

### ✅ CORRECT: Using Software State Byte
```
Request 1:
  state_byte = 0 (IDLE)
  CommandPtr(desc_1) → RUN=1
  state_byte = 1 (RUNNING)
  stopDMAAfterTransmit():
    • Wait for completion
    • Clear RUN=0
    • state_byte = 0 (IDLE) ✓
  
Request 2:
  state_byte = 0 (IDLE) ✓
  Driver knows: "Must use PATH 1"
  CommandPtr(desc_2) → RUN=1
  
  ✓ SUCCESS: Fresh descriptor fetch
  - No stale Z=0 in cache
  - Hardware starts cleanly
  - desc_2 transmits correctly
```

## Performance Comparison (4 ROM Reads)

| Mode | MMIO Writes | DMA Starts | Memory Barriers | Polls/Waits |
|------|-------------|------------|-----------------|-------------|
| **PATH1** | 8 (4×CmdPtr + 4×RUN) | 4 | 8 | 4× completion |
| **PATH2** | 4 (1×CmdPtr + 1×RUN + 3×WAKE) | 1 | 7 (1 initial + 3×2 per append) | 1× completion + 3× WAKE poll |
| **LINUX** | 2 (1×CmdPtr + 1×RUN) | 1 | 1 | 1× batch completion |
| **APPLE1** | 8 (4×CmdPtr + 4×RUN) | 4 | 8 | 4× stopDMA wait |
| **APPLE2** | 4 (1×CmdPtr + 1×RUN + 3×WAKE) | 1 | 7 | 1× stopDMA (last) + 3× no-wait |

**Winner: LINUX** (fewest operations, maximum hardware automation)  
**Best for DriverKit: APPLE2** (efficiency + safety + proven compatibility)  
**Safest: APPLE1** (no quirk dependencies, always recovers cleanly)

## Descriptor Memory Layout Comparison

### PATH1/PATH2/APPLE1/APPLE2 (32B per packet)
```
Descriptor @ 0x80000020:
+0x00: [control=0x123C000C] [branch=0x00000000]
+0x08: [data=0x00000000]    [status=0x00000000]
+0x10: [immediate: 40 01 00 80 FF FF C0 FF]
+0x18: [immediate: 00 04 00 F0 00 00 00 00]

Total ring capacity: 256 descriptors = 8KB
```

### LINUX (64B per packet = 4×16B descriptors)
```
Packet @ 0x80000020:
d[0] +0x00: [reqCount=12] [control=0x123C]
d[0] +0x04: [dataAddr=0x80000030] [branch=0x80000063]
d[0] +0x0C: [resCount=0] [xferStatus=0]

d[1] +0x10: [immediate header: 40 01 00 80 FF FF C0 FF]
d[1] +0x18: [immediate header: 00 04 00 F0 00 00 00 00]

d[2] +0x20: [payload descriptor - unused for quadlet]
d[3] +0x30: [driver metadata - packet pointer]

Total ring capacity: 64 packets = 256 descriptors = 4KB
```

## Recommended Mode Selection Guide

```
┌─────────────────────────────────────────────────────────┐
│ START: What is your use case?                          │
└─────────────────────────────────────────────────────────┘
                         ↓
        ┌────────────────┴────────────────┐
        │                                 │
    Config ROM / Discovery          Streaming / Bulk
    Sporadic requests               Continuous traffic
        │                                 │
        ↓                                 ↓
    ┌───────────┐                   ┌──────────┐
    │ APPLE2    │                   │ LINUX    │
    │ (hybrid)  │                   │ (chain)  │
    └───────────┘                   └──────────┘
        │                                 │
        │ Hardware quirks?                │ Uncached DMA?
        ↓                                 ↓
    ┌───────────┐                   ┌──────────┐
    │ YES       │                   │ NO       │
    │ ↓         │                   │ ↓        │
    │ APPLE1    │                   │ APPLE2   │
    │ (simple)  │                   │ (hybrid) │
    └───────────┘                   └──────────┘
```

**Decision Criteria:**

1. **Hardware Known Good (no quirks)** → APPLE2 or LINUX
2. **Hardware Has Quirks (Agere/LSI)** → APPLE1 (safest)
3. **Debugging/Bring-up** → PATH1 (simplest visibility)
4. **Current ASFW** → PATH2 with watchguards (working well)
5. **Future Production** → APPLE2 (optimal balance)
