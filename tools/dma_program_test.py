# dma_rom_prog.py
# Prints OHCI AT "DMA programs" to read Config ROM at 0xF0000400, 0x408, 0x40C, 0x410.
# - path1:  program CommandPtr + RUN for each read (no chaining)
# - path2:  program once, then append by patching BRANCH (addr|Z) and pulsing WAKE
# - linux:  Linux firewire-ohci style multi-descriptor layout
# - apple1: Apple simple mode - always stops after each transaction
# - apple2: Apple hybrid mode - with needsFlush flag (stop on flush=1, keep running on flush=0)
#
# Immediate headers match the working Apple/FireBug pattern you posted:
#   0x400 → q0=40 01 00 80 | q1=FF FF C0 FF | q2=00 04 00 F0
#   0x408 → q0=40 01 04 80 | q1=FF FF C0 FF | q2=08 04 00 F0
#   0x40C → q0=40 01 06 80 | q1=FF FF C0 FF | q2=0C 04 00 F0
#   0x410 → q0=40 01 08 80 | q1=FF FF C0 FF | q2=10 04 00 F0
#
from typing import List, Tuple
import sys

DESC_SIZE = 32              # 16B desc header + 16B immediate area
LINUX_DESC_SIZE = 16        # Linux descriptor is 16B
BASE_IOVA = 0x80000020      # device address start (matches your logs)
CTL_OUTPUT_LAST_IMM = 0x123C0000  # OUTPUT_LAST | Immediate; low 16 bits = reqLen
REQ_LEN = 12                # 3 quadlets
Z_FOR_32B = 2               # CommandPtr.Z for a 32B block

# Linux descriptor control bits
DESCRIPTOR_OUTPUT_LAST = (1 << 12)
DESCRIPTOR_KEY_IMMEDIATE = (2 << 8)
DESCRIPTOR_IRQ_ALWAYS = (3 << 4)
DESCRIPTOR_BRANCH_ALWAYS = (3 << 2)

# ROM quadlets to read (relative to 0xF0000400)
READ_OFFS = [0x000, 0x008, 0x00C, 0x010]

def le32(n: int) -> bytes:
    return int(n & 0xFFFFFFFF).to_bytes(4, "little")

def le16(n: int) -> bytes:
    return int(n & 0xFFFF).to_bytes(2, "little")

def hex_bytes(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)

def build_qread_header(addr_low_byte: int, tlabel: int) -> bytes:
    """
    12B immediate header for a Read Quadlet Request, matching your observed pattern:
      q0 = 40 01 XX 80  with XX=(addr_low >> 1)
      q1 = FF FF C0 FF
      q2 = AA 04 00 F0  with AA=(addr_low)
    """
    q0 = bytes([0x40, tlabel & 0x3F, (addr_low_byte >> 1) & 0xFF, 0x80])
    q1 = bytes([0xFF, 0xFF, 0xC0, 0xFF])
    q2 = bytes([addr_low_byte & 0xFF, 0x04, 0x00, 0xF0])
    return q0 + q1 + q2

def build_last_immediate_block(header12: bytes, branch_cmdptr: int = 0) -> bytes:
    """
    One 32B block in memory:
      [0..3]   control (LE) = 0x123C0000 | REQ_LEN
      [4..7]   BRANCH (LE)  = next CommandPtr (addr|Z) or 0
      [8..11]  data/ptr     = 0   (unused for immediate)
      [12..15] status       = 0   (HW writes here)
      [16..31] immediate header (12B) + pad(4B)
    """
    control = CTL_OUTPUT_LAST_IMM | REQ_LEN
    desc = (
        le32(control) +          # control
        le32(branch_cmdptr) +    # branch = next CommandPtr (when chaining)
        le32(0) +                # data/ptr (unused for immediate)
        le32(0)                  # status (HW)
    )
    imm = header12 + b"\x00" * (16 - len(header12))
    return desc + imm

def alloc_iova(idx: int) -> int:
    return BASE_IOVA + idx * DESC_SIZE

def alloc_linux_iova(idx: int) -> int:
    """Linux uses 4 × 16B descriptors per packet (64B total)"""
    return BASE_IOVA + idx * (4 * LINUX_DESC_SIZE)

def print_block(idx: int, block: bytes, label: str):
    iova = alloc_iova(idx)
    ctl = int.from_bytes(block[0:4], "little")
    br  = int.from_bytes(block[4:8], "little")
    print(f"\n[{label}] Descriptor @ iova=0x{iova:08X}  (Z=2 for 32B)")
    print(f"  ctl=0x{ctl:08X}  br=0x{br:08X}")
    print(f"  +0x00: {hex_bytes(block[0:16])}")
    print(f"  +0x10 (immediate): {hex_bytes(block[16:32])}")

def make_program_blocks() -> List[Tuple[int, bytes, int]]:
    """Create 4 × 32B OUTPUT_LAST_Immediate blocks and their (addr|Z) CommandPtr."""
    blocks = []
    tlabel = 1
    for idx, offs in enumerate(READ_OFFS):
        addr_low = (0x400 + offs) & 0xFF   # 0x00, 0x08, 0x0C, 0x10
        hdr = build_qread_header(addr_low, tlabel)
        blk = build_last_immediate_block(hdr, branch_cmdptr=0)
        cmdptr = (alloc_iova(idx) & 0xFFFFFFF0) | Z_FOR_32B  # (addr|Z)
        blocks.append((idx, blk, cmdptr))
        tlabel = (tlabel + 1) & 0x3F
    return blocks

def build_linux_descriptor(req_count: int, control: int, data_addr: int, branch_addr: int) -> bytes:
    """
    Linux struct descriptor (16B):
      +0x00: req_count (LE16)
      +0x02: control (LE16)
      +0x04: data_address (LE32)
      +0x08: branch_address (LE32)
      +0x0C: res_count (LE16) - set to 0, HW updates
      +0x0E: transfer_status (LE16) - set to 0, HW updates
    """
    return (le16(req_count) + le16(control) + le32(data_addr) + 
            le32(branch_addr) + le16(0) + le16(0))

def build_linux_packet_descriptors(idx: int, addr_low: int, tlabel: int, next_iova: int = 0) -> bytes:
    """
    Linux at_context_queue_packet() uses 4 descriptors per packet:
      d[0]: KEY_IMMEDIATE - holds timestamp/metadata in req_count and res_count
      d[1]: Header quadlet storage (12B immediate data)
      d[2]: Payload descriptor (not used for quadlet reads, but reserved)
      d[3]: Driver data storage (packet pointer)
    
    Last descriptor (d[0] for no-payload packets) gets:
      OUTPUT_LAST | IRQ_ALWAYS | BRANCH_ALWAYS
    """
    base = alloc_linux_iova(idx)
    hdr = build_qread_header(addr_low, tlabel)
    
    # d[0]: KEY_IMMEDIATE with header length, will be marked OUTPUT_LAST
    control_d0 = DESCRIPTOR_KEY_IMMEDIATE | DESCRIPTOR_OUTPUT_LAST | DESCRIPTOR_IRQ_ALWAYS | DESCRIPTOR_BRANCH_ALWAYS
    d0 = build_linux_descriptor(
        req_count=REQ_LEN,
        control=control_d0,
        data_addr=base + LINUX_DESC_SIZE,  # points to d[1] for immediate data
        branch_addr=next_iova
    )
    
    # d[1]: Immediate packet header storage (12B header + 4B padding)
    # In Linux this is inline memory, not a descriptor the HW interprets
    d1 = hdr + b"\x00" * 4
    
    # d[2]: Payload descriptor (unused for quadlet reads)
    d2 = build_linux_descriptor(0, 0, 0, 0)
    
    # d[3]: Driver data (unused, just placeholder)
    d3 = b"\x00" * LINUX_DESC_SIZE
    
    return d0 + d1 + d2 + d3

def print_linux_packet(idx: int, data: bytes, label: str, next_iova: int = 0):
    """Print a 64B Linux packet (4 × 16B descriptors)"""
    base = alloc_linux_iova(idx)
    print(f"\n[{label}] Linux packet @ iova=0x{base:08X}  (4 descriptors, 64B total)")
    
    # Parse d[0]
    req_count = int.from_bytes(data[0:2], "little")
    control = int.from_bytes(data[2:4], "little")
    data_addr = int.from_bytes(data[4:8], "little")
    branch_addr = int.from_bytes(data[8:12], "little")
    
    print(f"  d[0] @ +0x00: reqCount={req_count:#06x} ctl={control:#06x} dataAddr=0x{data_addr:08X} branch=0x{branch_addr:08X}")
    print(f"       {hex_bytes(data[0:16])}")
    print(f"  d[1] @ +0x10 (header): {hex_bytes(data[16:32])}")
    print(f"  d[2] @ +0x20 (payload): {hex_bytes(data[32:48])}")
    print(f"  d[3] @ +0x30 (driver): {hex_bytes(data[48:64])}")

def program_linux(blocks_count: int = 4):
    """
    Linux firewire-ohci style: multi-descriptor packets with automatic chaining.
    Each packet is 4 × 16B descriptors = 64B.
    """
    print("=== LINUX: firewire-ohci multi-descriptor layout with branch chaining ===")
    print("Each packet: d[0]=KEY_IMMEDIATE+OUTPUT_LAST, d[1]=header, d[2]=payload, d[3]=driver_data")
    
    packets = []
    tlabel = 1
    
    for idx, offs in enumerate(READ_OFFS[:blocks_count]):
        addr_low = (0x400 + offs) & 0xFF
        # For all but last packet, branch to next
        next_iova = 0
        if idx < blocks_count - 1:
            next_iova = (alloc_linux_iova(idx + 1) & 0xFFFFFFF0) | 3  # Z=3 for next packet (4 descriptors)
        
        pkt = build_linux_packet_descriptors(idx, addr_low, tlabel, next_iova)
        packets.append((idx, pkt, addr_low))
        tlabel = (tlabel + 1) & 0x3F
    
    # Print first packet with startup
    idx0, pkt0, addr0 = packets[0]
    print_linux_packet(idx0, pkt0, f"READ #1 @ 0xF00004{addr0:02X}", 
                       int.from_bytes(pkt0[8:12], "little"))
    print(f"  Step: CommandPtr=0x{(alloc_linux_iova(0) & 0xFFFFFFF0) | 3:08X}  (addr=0x{alloc_linux_iova(0):08X}, Z=3)")
    print("        Set RUN=1  → controller starts and processes chained packets")
    
    # Print remaining packets
    for i in range(1, len(packets)):
        idx, pkt, addr = packets[i]
        print_linux_packet(idx, pkt, f"READ #{i+1} @ 0xF00004{addr:02X}",
                          int.from_bytes(pkt[8:12], "little"))
        if i < len(packets) - 1:
            print(f"        Chains to next via BRANCH")
    
    print("\n  Note: Linux builds entire chain before starting DMA (context_append + context_run)")
    print("        All descriptors have BRANCH_ALWAYS; last packet has branch=0")

def program_apple1(blocks_count: int = 4):
    """
    Apple AppleFWOHCI simple mode: PATH 1 for every request (always stops).
    
    Pattern from executeCommandElement @ 0xDBBE:
    - this+28 = 0 (IDLE state) for every request
    - PATH 1: Program CommandPtr + set RUN
    - Immediately call stopDMAAfterTransmit()
    - this+28 → 0 (back to IDLE)
    
    Result: Next request always uses PATH 1 (no stale Z=0 append attempts)
    """
    print("=== APPLE1: AppleFWOHCI simple mode (always stop after transmission) ===")
    print("Software state byte (this+28): tracks IDLE(0) vs RUNNING(1)")
    print("Pattern: PATH1 → RUN → stopDMAAfterTransmit → IDLE → repeat")
    
    packets = []
    tlabel = 1
    
    for idx, offs in enumerate(READ_OFFS[:blocks_count]):
        addr_low = (0x400 + offs) & 0xFF
        hdr = build_qread_header(addr_low, tlabel)
        blk = build_last_immediate_block(hdr, branch_cmdptr=0)
        cmdptr = (alloc_iova(idx) & 0xFFFFFFF0) | Z_FOR_32B
        packets.append((idx, blk, cmdptr, addr_low))
        tlabel = (tlabel + 1) & 0x3F
    
    for i, (idx, blk, cmdptr, addr) in enumerate(packets):
        print_block(idx, blk, f"READ #{i+1} @ 0xF00004{addr:02X}")
        print(f"  State byte (this+28) = 0 (IDLE)")
        print(f"  Step 1: CommandPtr=0x{cmdptr:08X}  (addr=0x{cmdptr & 0xFFFFFFF0:08X}, Z={cmdptr & 0xF})")
        print(f"  Step 2: WriteControlSet(RUN=0x8000)  → context starts fetching")
        print(f"  Step 3: Set state byte (this+28) = 1 (RUNNING)")
        print(f"  Step 4: stopDMAAfterTransmit() called immediately:")
        print(f"          - Wait for descriptor status (15 × 5µs poll)")
        print(f"          - WaitForDMA() → poll ACTIVE=0")
        print(f"          - WriteControlClear(RUN=0x8000)")
        print(f"          - Set state byte (this+28) = 0 (IDLE)")
        print(f"          - Set this+6 = 0 (clear tail pointer)")
        if i < len(packets) - 1:
            print()
    
    print("\n  Result: Every request uses PATH 1 (fresh CommandPtr programming)")
    print("          No Z=0 append bug possible - state always resets to IDLE")
    print("          Same pattern for all requests regardless of timing")

def program_apple2(blocks_count: int = 4):
    """
    Apple AppleFWOHCI hybrid mode: needsFlush flag controls stop behavior.
    
    Pattern from executeCommandElement @ 0xDBBE + asyncWrite @ 0xEDCA:
    - commandElement[10] = needsFlush flag (offset +40)
    - needsFlush=0: Quadlet operations, keep context running
    - needsFlush=1: Block writes, stop after transmission
    
    First request: Always PATH 1 (state byte = 0)
    Subsequent requests:
      - If previous needsFlush=1: state byte = 0 → PATH 1
      - If previous needsFlush=0: state byte = 1 → PATH 2 (branch chain + WAKE)
    """
    print("=== APPLE2: AppleFWOHCI hybrid mode (needsFlush flag controls stop) ===")
    print("needsFlush=0: Quadlet ops, keep running, use PATH 2 for next")
    print("needsFlush=1: Block writes, stop after tx, use PATH 1 for next")
    
    # For ROM reads, needsFlush=0 (simple quadlet operations)
    # Simulate: first 3 requests use needsFlush=0, last uses needsFlush=1
    needs_flush = [False, False, False, True]
    
    packets = []
    tlabel = 1
    
    for idx, offs in enumerate(READ_OFFS[:blocks_count]):
        addr_low = (0x400 + offs) & 0xFF
        hdr = build_qread_header(addr_low, tlabel)
        blk = build_last_immediate_block(hdr, branch_cmdptr=0)
        cmdptr = (alloc_iova(idx) & 0xFFFFFFF0) | Z_FOR_32B
        packets.append((idx, blk, cmdptr, addr_low, needs_flush[idx] if idx < len(needs_flush) else False))
        tlabel = (tlabel + 1) & 0x3F
    
    state_byte = 0  # Start IDLE
    
    for i, (idx, blk, cmdptr, addr, flush) in enumerate(packets):
        if state_byte == 0:
            # PATH 1: IDLE state
            print_block(idx, blk, f"READ #{i+1} @ 0xF00004{addr:02X}")
            print(f"  State byte (this+28) = 0 (IDLE) → Use PATH 1")
            print(f"  needsFlush = {1 if flush else 0}")
            print(f"  Step 1: CommandPtr=0x{cmdptr:08X}  (addr=0x{cmdptr & 0xFFFFFFF0:08X}, Z={cmdptr & 0xF})")
            print(f"  Step 2: WriteControlSet(RUN=0x8000)  → context starts")
            print(f"  Step 3: Set state byte (this+28) = 1 (RUNNING)")
            state_byte = 1
            
            if flush:
                print(f"  Step 4: needsFlush=1 → stopDMAAfterTransmit() called:")
                print(f"          - Wait descriptor status + WaitForDMA()")
                print(f"          - WriteControlClear(RUN=0x8000)")
                print(f"          - Set state byte (this+28) = 0 (IDLE)")
                state_byte = 0
            else:
                print(f"  Step 4: needsFlush=0 → Keep context running, return")
        else:
            # PATH 2: RUNNING state
            patched_prev_idx = i - 1
            _, prev_blk, _, _, _ = packets[patched_prev_idx]
            patched_blk = bytearray(prev_blk)
            patched_blk[4:8] = le32(cmdptr)  # Patch branch word
            
            print_block(patched_prev_idx, bytes(patched_blk), 
                       f"READ #{i} (prev with BRANCH → #{i+1})")
            print(f"  State byte (this+28) = 1 (RUNNING) → Use PATH 2")
            print(f"  Step 1: Patch prev.branchWord = 0x{cmdptr:08X}")
            print(f"          (offset = blockCount=2 ? 0 : 16×(n-1))")
            
            print_block(idx, blk, f"READ #{i+1} @ 0xF00004{addr:02X}")
            print(f"  needsFlush = {1 if flush else 0}")
            print(f"  Step 2: WriteControlSet(WAKE=0x1000)  → pulse wake bit")
            
            if flush:
                print(f"  Step 3: needsFlush=1 → stopDMAAfterTransmit() called:")
                print(f"          - Set state byte (this+28) = 0 (IDLE)")
                state_byte = 0
            else:
                print(f"  Step 3: needsFlush=0 → Keep running for next request")
        
        if i < len(packets) - 1:
            print()
    
    print("\n  Key Behavior:")
    print("  - First request: Always PATH 1 (state byte starts at 0)")
    print("  - needsFlush=0: Context keeps running → next uses PATH 2")
    print("  - needsFlush=1: Context stops → next uses PATH 1")
    print("  - Prevents Z=0 append: state byte tracks software intent, not hardware ACTIVE")

def program_path1(blocks: List[Tuple[int, bytes, int]]):
    print("=== PATH 1: Program CommandPtr + RUN per request (no chaining) ===")
    for idx, blk, cmdptr in blocks:
        print_block(idx, blk, f"READ #{idx+1}")
        print(f"  Step: CommandPtr=0x{cmdptr:08X}  (addr=0x{cmdptr & 0xFFFFFFF0:08X}, Z={cmdptr & 0xF})")
        print("        Set RUN=1  → controller fetches and transmits this request.")
        print("        After AR response/completion, driver stops context (RUN=0).")

def program_path2(blocks: List[Tuple[int, bytes, int]]):
    print("=== PATH 2: Start once, append by patching BRANCH and pulsing WAKE ===")
    # Submit first request
    idx0, blk0, cmdptr0 = blocks[0]
    print_block(idx0, blk0, "READ #1")
    print(f"  Step: CommandPtr=0x{cmdptr0:08X}  (addr=0x{cmdptr0 & 0xFFFFFFF0:08X}, Z={cmdptr0 & 0xF})")
    print("        Set RUN=1  → controller starts and fetches READ #1.")
    # Append subsequent requests
    for i in range(1, len(blocks)):
        idx_prev, blk_prev, _ = blocks[i-1]
        idx, blk, cmdptr = blocks[i]
        patched_prev = bytearray(blk_prev)
        # Patch BRANCH with (next addr|Z)
        patched_prev[4:8] = le32(cmdptr)
        print_block(idx_prev, bytes(patched_prev), f"READ #{i} (prev with BRANCH → #{i+1})")
        print(f"  Step: Patch BRANCH to 0x{cmdptr:08X}; publish 16B; memory barrier; WAKE=1.")
        print_block(idx, blk, f"READ #{i+1}")
        blocks[i-1] = (idx_prev, bytes(patched_prev), _)

def main():
    mode = (sys.argv[1] if len(sys.argv) > 1 else "path1").strip().lower()
    if mode not in ("path1", "path2", "linux", "apple1", "apple2"):
        print("Usage: python dma_rom_prog.py [path1|path2|linux|apple1|apple2]")
        print("  path1:  Simple start/stop per request")
        print("  path2:  Dynamic branch chaining with WAKE")
        print("  linux:  Linux firewire-ohci multi-descriptor layout")
        print("  apple1: Apple simple mode (always stop after tx)")
        print("  apple2: Apple hybrid mode (needsFlush flag)")
        return
    
    if mode == "linux":
        program_linux()
    elif mode == "apple1":
        program_apple1()
    elif mode == "apple2":
        program_apple2()
    else:
        blocks = make_program_blocks()
        if mode == "path1":
            program_path1(blocks)
        else:
            program_path2(blocks)

if __name__ == "__main__":
    main()
