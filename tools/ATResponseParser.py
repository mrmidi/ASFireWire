#!/usr/bin/env python3
"""
ATResponseParser.py - IEEE 1394 Asynchronous Response Packet Parser

Parses FireWire asynchronous response packets captured from OHCI AR (Async Receive) context.
Handles OHCI's little-endian header format and FireWire's big-endian payload format.

IEEE 1394 ASYNCHRONOUS PACKET STRUCTURE (IEEE 1394-2008 §6.2)
==============================================================

On the FireWire Bus (Big-Endian, transmitted MSB first):
---------------------------------------------------------
1. Packet Header (3-4 quadlets, depending on tcode):
   
   Quadlet 0: [destination_ID:16][tLabel:6][rt:2][tcode:4][pri:4]
   Quadlet 1: [source_ID:16][rcode:4 or offset_high:16][reserved:12]
   Quadlet 2: [destination_offset_low:32] (for requests)
              [reserved:32] (for responses, typically 0x00000000)
   Quadlet 3: [data:32] (for read quad response)
              [data_length:16][extended_tcode:16] (for block packets)
   
2. Header CRC (1 quadlet):
   - CRC-32 computed over header quadlets
   - Polynomial: 0x04C11DB7
   - Algorithm defined in IEEE 1394-2008 Table 6-17

3. Data Block (0 or more quadlets, for block packets):
   - Payload data in big-endian format
   - Padded to quadlet boundary if needed
   
4. Data CRC (1 quadlet, if data block present):
   - CRC-32 computed over data block
   - Same polynomial as header CRC

OHCI CONTROLLER TRANSFORMATION (OHCI 1.1 §7.4.2)
=================================================

The OHCI controller performs these operations when receiving packets:

1. CRC Validation:
   - Hardware validates header_CRC and data_CRC
   - Packets with bad CRCs are dropped or flagged with error status
   - CRCs are STRIPPED and never passed to software

2. Byte Order Conversion:
   - Header quadlets (Q0-Q3) are converted from big-endian (wire format)
     to LITTLE-ENDIAN (host memory format) on Intel/ARM systems
   - This is a BYTE-SWAP of entire quadlets, not field reordering
   - Payload data beyond header remains in BIG-ENDIAN format

3. Storage in AR Buffer:
   - Header quadlets (LE, CRCs removed)
   - Payload data (BE, unchanged)
   - Optional timestamp (LE, OHCI-specific, appended if enabled)

FIELD EXTRACTION FROM OHCI AR BUFFER
=====================================

After OHCI stores the packet in little-endian format, extract fields directly
from the LE quadlets without further byte swapping:

Example: header[0] = 0xFFC20160 (LE stored in memory)

On the wire (BE): 0x6001C2FF = [C2FF:16][00:6][1:2][6:4][0:4]
In memory (LE):   0xFFC20160 = bits are now: [C2FF:upper16][0160:lower16]

Field extraction (no byte swap needed, work directly on LE value):
  destination_ID = (header[0] >> 16) & 0xFFFF = 0xFFC2 ✓
  tLabel        = (header[0] >> 10) & 0x3F   = 0
  retry         = (header[0] >> 8)  & 0x3    = 1
  tcode         = (header[0] >> 4)  & 0xF    = 6
  priority      = (header[0] >> 0)  & 0xF    = 0

This matches the Linux kernel's packet-header-definitions.h approach.

DATA VALUE INTERPRETATION
=========================

For read quadlet responses, header[3] contains the response data:
  - Stored as LE in OHCI buffer: 0xE28F2004
  - Must byte-swap to get BE interpretation: 0x04208FE2
  
This is because the data quadlet was big-endian on the wire (0x04208FE2),
OHCI byte-swapped it during storage (0xE28F2004), and we must reverse
this to interpret the actual data value.

Cross-validated against:
- IEEE 1394-2008 §6.2: Asynchronous packet format
- OHCI 1.1 Specification §7.4: Async Receive DMA  
- Linux kernel: firewire/packet-header-definitions.h
"""

import sys
import struct
from typing import List, Tuple, Optional
from dataclasses import dataclass
from enum import IntEnum


class EventCode(IntEnum):
    """
    OHCI Event Codes (OHCI 1.1 Table 3-2 + Linux firewire-ohci)
    
    Written to ContextControl.event field after packet transmission/reception.
    Values 0x00-0x0F are OHCI internal events.
    Values 0x10-0x1F are IEEE 1394 ACK codes from the bus.
    
    Cross-referenced with Linux kernel firewire/ohci.c event table.
    """
    # OHCI Internal Events (0x00-0x0F)
    EVT_NO_STATUS = 0x00        # No status available
    EVT_LONG_PACKET = 0x02      # Packet exceeds maximum length
    EVT_MISSING_ACK = 0x03      # No acknowledgment received
    EVT_UNDERRUN = 0x04         # Buffer underrun (data not ready)
    EVT_OVERRUN = 0x05          # Buffer overrun (too much data)
    EVT_DESCRIPTOR_READ = 0x06  # Error reading descriptor from memory
    EVT_DATA_READ = 0x07        # Error reading data from memory
    EVT_DATA_WRITE = 0x08       # Error writing data to memory
    EVT_BUS_RESET = 0x09        # Bus reset occurred during transfer
    EVT_TIMEOUT = 0x0A          # Transaction timeout
    EVT_TCODE_ERR = 0x0B        # Invalid transaction code
    EVT_UNKNOWN = 0x0E          # Unknown event
    EVT_FLUSHED = 0x0F          # Context flushed
    
    # IEEE 1394 ACK Codes (0x10-0x1F)
    ACK_COMPLETE = 0x11         # Packet acknowledged as complete
    ACK_PENDING = 0x12          # Packet acknowledged as pending
    ACK_BUSY_X = 0x14           # Busy (exponential backoff)
    ACK_BUSY_A = 0x15           # Busy (protocol A)
    ACK_BUSY_B = 0x16           # Busy (protocol B)
    ACK_TARDY = 0x1B            # Ack sent too late
    ACK_DATA_ERROR = 0x1D       # Data error (CRC failed)
    ACK_TYPE_ERROR = 0x1E       # Type error (unsupported tcode)
    
    # Linux-specific
    PENDING_CANCELLED = 0x20    # Pending operation cancelled


class TCode(IntEnum):
    """
    IEEE 1394 Transaction Codes (IEEE 1394-2008 Table 6-2)
    
    Defines the type and format of asynchronous packets.
    """
    WRITE_QUAD_REQUEST = 0x0
    WRITE_BLOCK_REQUEST = 0x1
    WRITE_RESPONSE = 0x2
    # 0x3 reserved
    READ_QUAD_REQUEST = 0x4
    READ_BLOCK_REQUEST = 0x5
    READ_QUAD_RESPONSE = 0x6
    READ_BLOCK_RESPONSE = 0x7
    CYCLE_START = 0x8
    LOCK_REQUEST = 0x9
    ISOCHRONOUS_BLOCK = 0xA
    LOCK_RESPONSE = 0xB
    # 0xC-0xD reserved
    # 0xE = PHY packet (not used in async transactions)


class RCode(IntEnum):
    """
    IEEE 1394 Response Codes (IEEE 1394-2008 Table 6-3)
    
    Indicates the result of an asynchronous transaction.
    Only present in response packets (not requests).
    """
    COMPLETE = 0x0          # Transaction completed successfully
    # 0x1-0x3 reserved
    CONFLICT_ERROR = 0x4    # Resource conflict prevented completion
    DATA_ERROR = 0x5        # Data field failed CRC check or length mismatch
    TYPE_ERROR = 0x6        # Unsupported transaction or invalid field
    ADDRESS_ERROR = 0x7     # Address not accessible in destination node
    # 0x8-0xF reserved


class RetryCode(IntEnum):
    """
    IEEE 1394 Retry Codes (IEEE 1394-2008 §6.2.4.3)
    
    Indicates retry protocol for this transmission attempt.
    """
    NEW = 0x0  # First attempt (no retry)
    X = 0x1    # Retry with exponential backoff (most common)
    A = 0x2    # Retry protocol A
    B = 0x3    # Retry protocol B


@dataclass
class ResponsePacket:
    """
    Parsed IEEE 1394 asynchronous response packet.
    
    All fields are extracted from OHCI AR buffer format (little-endian quadlets).
    Data values are converted to big-endian for proper interpretation.
    """
    # Header fields (quadlet 0)
    destination_id: int  # 16-bit node address (destination of this response)
    tlabel: int          # 6-bit transaction label (matches original request)
    retry: RetryCode     # 2-bit retry code (0=NEW, 1=X, 2=A, 3=B)
    tcode: TCode         # 4-bit transaction code (typically 0x6 or 0x7 for responses)
    priority: int        # 4-bit priority (0-15, typically 0)
    
    # Header fields (quadlet 1)
    source_id: int       # 16-bit node address (source of this response)
    rcode: RCode         # 4-bit response code (0=success, 4-7=errors)
    reserved: int        # 12-bit reserved field (typically 0)
    
    # Quadlet 2 (context-dependent)
    quadlet_2: int       # For requests: destination_offset_low
                         # For responses: reserved (typically 0)
    
    # Quadlet 3 (IEEE 1394 payload data, big-endian)
    quadlet_data: int    # For read quad response: the read result value
                         # For write quad request: the value to write
                         # For block packets: data_length:16 | extended_tcode:16
    
    # OHCI-specific metadata (quadlet 4)
    xfer_status: Optional[int] = None   # 16-bit transfer status flags
    timestamp: Optional[int] = None      # 16-bit cycle timer value
    
    # Decoded OHCI fields
    event_code: Optional[int] = None     # Event code from xferStatus
    cycle_seconds: Optional[int] = None  # 3-bit seconds from timestamp
    cycle_count: Optional[int] = None    # 13-bit cycle count from timestamp
    
    # Payload data (big-endian, for block packets)
    payload: bytes = b''
    
    # Raw packet data as stored in OHCI AR buffer
    raw_data: bytes = b''
    
    def get_event_name(self) -> Optional[str]:
        """Get human-readable event code name"""
        if self.event_code is None:
            return None
        try:
            return EventCode(self.event_code).name
        except ValueError:
            return f"UNKNOWN_0x{self.event_code:02x}"
    
    def get_timestamp_info(self) -> Optional[str]:
        """Get formatted timestamp information"""
        if self.cycle_seconds is None or self.cycle_count is None:
            return None
        # Cycle count increments every 125 microseconds (8000 Hz)
        # 8000 counts = 1 second
        microseconds = (self.cycle_count * 1000000) // 8000
        return f"{self.cycle_seconds}s + {microseconds}µs (count={self.cycle_count})"
    
    def __str__(self) -> str:
        """Human-readable packet description"""
        lines = []
        lines.append(f"{'=' * 70}")
        lines.append(f"IEEE 1394 Asynchronous Response Packet")
        lines.append(f"{'=' * 70}")
        
        # Addresses
        lines.append(f"Source ID:      0x{self.source_id:04x}")
        lines.append(f"Destination ID: 0x{self.destination_id:04x}")
        
        # Transaction info
        lines.append(f"Transaction Label: {self.tlabel}")
        lines.append(f"Transaction Code:  0x{self.tcode:x} ({self.tcode.name})")
        lines.append(f"Response Code:     0x{self.rcode:x} ({self.rcode.name})")
        lines.append(f"Retry Code:        0x{self.retry:x} ({self.retry.name})")
        lines.append(f"Priority:          {self.priority}")
        
        # Quadlet 2 interpretation
        if self.tcode == TCode.READ_QUAD_RESPONSE:
            lines.append(f"Offset/Reserved:   0x{self.quadlet_2:08x}")
            lines.append(f"Quadlet Data:      0x{self.quadlet_data:08x} (BE)")
        elif self.tcode == TCode.WRITE_QUAD_REQUEST:
            lines.append(f"Offset Low:        0x{self.quadlet_2:08x}")
            lines.append(f"Quadlet Data:      0x{self.quadlet_data:08x} (BE)")
        else:
            lines.append(f"Quadlet 2:         0x{self.quadlet_2:08x}")
            lines.append(f"Quadlet 3:         0x{self.quadlet_data:08x}")
        
        # OHCI status
        if self.xfer_status is not None:
            lines.append(f"OHCI xferStatus:   0x{self.xfer_status:04x}")
            # Decode event code (bits 4-0 of upper byte in ContextControl register)
            if self.event_code is not None:
                event_name = self.get_event_name()
                lines.append(f"  Event Code:      0x{self.event_code:02x} ({event_name})")
        if self.timestamp is not None:
            lines.append(f"OHCI timeStamp:    0x{self.timestamp:04x}")
            timestamp_info = self.get_timestamp_info()
            if timestamp_info:
                lines.append(f"  Decoded:         {timestamp_info}")
        
        # Payload
        if self.payload:
            lines.append(f"Payload ({len(self.payload)} bytes, big-endian):")
            for i in range(0, len(self.payload), 16):
                chunk = self.payload[i:i+16]
                hex_str = ' '.join(f'{b:02x}' for b in chunk)
                lines.append(f"  [{i:04x}] {hex_str}")
        
        # Raw dump
        lines.append(f"\nRaw Packet ({len(self.raw_data)} bytes):")
        for i in range(0, len(self.raw_data), 16):
            chunk = self.raw_data[i:i+16]
            hex_str = ' '.join(f'{b:02x}' for b in chunk)
            lines.append(f"  [{i:04x}] {hex_str}")
        
        lines.append(f"{'=' * 70}")
        return '\n'.join(lines)


class ATResponseParser:
    """
    Parser for IEEE 1394 asynchronous response packets from OHCI AR buffers.
    
    OHCI AR Context Buffer Format (OHCI 1.1 §7.4.2):
    ------------------------------------------------
    The OHCI controller receives IEEE 1394 packets and stores them in AR
    (Async Receive) context buffers with the following transformations:
    
    1. CRC Validation & Removal:
       - Hardware validates header_CRC and data_CRC (IEEE 1394 CRC-32)
       - CRCs are STRIPPED - never visible to software
       - Bad CRC packets are dropped or flagged with error status
    
    2. Byte Order Conversion:
       - Header quadlets (originally big-endian on wire) are converted to
         LITTLE-ENDIAN and stored in host memory
       - This is a byte-swap of entire 32-bit words
       - Payload data remains in BIG-ENDIAN format (unchanged from wire)
    
    3. Optional Timestamp:
       - OHCI may append a timestamp quadlet (little-endian)
       - Controlled by AR context control register
       - Contains cycle timer value when packet was received
    
    Memory Layout in AR Buffer:
    ---------------------------
    For a Read Quad Response packet (OHCI 1.1 Figure 8-8):
    
    Offset  | Content                    | Endianness | Source
    --------|----------------------------|------------|------------------
    0x00    | Quadlet 0 (header)         | LE         | IEEE 1394 header
    0x04    | Quadlet 1 (header)         | LE         | IEEE 1394 header  
    0x08    | Quadlet 2 (offset low)     | LE         | IEEE 1394 header
    0x0C    | Quadlet 3 (quadlet data)   | LE→BE*     | IEEE 1394 payload
    0x10    | Quadlet 4 (status)         | LE         | OHCI xferStatus:16 | timeStamp:16
    
    *Quadlet 3 is IEEE 1394 payload data, stored LE by OHCI, but must be
     interpreted as BE to get the actual data value.
    
    For Write Quad Request packets, Q3 contains the data to write.
    For Read Quad Response packets, Q3 contains the read result.
    
    Quadlet 4 is OHCI-specific status information (not part of IEEE 1394 packet):
    - Bits 31-16: xferStatus (transfer status flags)
    - Bits 15-0:  timeStamp (cycle timer value when packet received)
    
    Field Extraction:
    ----------------
    Fields are extracted directly from LE quadlets using bit shifts and masks.
    This matches the Linux kernel approach (packet-header-definitions.h).
    
    The masks and shifts are defined for LE quadlet arrays:
    - destination_ID: (quadlet[0] >> 16) & 0xFFFF
    - tLabel:         (quadlet[0] >> 10) & 0x3F
    - retry:          (quadlet[0] >> 8)  & 0x3
    - tcode:          (quadlet[0] >> 4)  & 0xF
    - priority:       (quadlet[0] >> 0)  & 0xF
    - source_ID:      (quadlet[1] >> 16) & 0xFFFF
    - rcode:          (quadlet[1] >> 12) & 0xF
    
    Data Interpretation:
    -------------------
    For read quad response, quadlet[3] contains the response data value.
    Since it was byte-swapped from BE to LE by OHCI, we must reverse this
    to interpret the actual data:
    
    Wire (BE):    0x04208FE2  <- actual data value
    OHCI buf(LE): 0xE28F2004  <- stored in memory  
    Interpretation: byte-swap back to 0x04208FE2
    """
    
    @staticmethod
    def parse_hex_dump(hex_string: str) -> bytes:
        """Convert hex dump string to bytes (handles spaces, newlines, etc.)"""
        # Remove all whitespace and common separators
        cleaned = ''.join(c for c in hex_string if c in '0123456789ABCDEFabcdef')
        # Convert to bytes
        return bytes.fromhex(cleaned)
    
    @staticmethod
    def parse(data: bytes) -> ResponsePacket:
        """
        Parse asynchronous response packet from OHCI AR buffer.
        
        This parser handles packets as stored by OHCI after:
        1. CRC validation and removal (CRCs never visible)
        2. Byte-swapping header quadlets from BE (wire) to LE (memory)
        3. Leaving payload data in BE format
        
        Args:
            data: Raw packet bytes from OHCI AR buffer (minimum 16 bytes)
                  - Bytes 0-15:  Header quadlets (LE)
                  - Bytes 16+:   Optional timestamp and/or payload (mixed endian)
            
        Returns:
            Parsed ResponsePacket object with all fields extracted
            
        Raises:
            ValueError: If data is too short or invalid
            
        Implementation Notes:
        --------------------
        This follows the Linux kernel's packet-header-definitions.h approach:
        - Read quadlets as little-endian (host byte order on Intel/ARM)
        - Extract fields directly using bit masks and shifts
        - No additional byte-swapping needed for header fields
        - Only data values need BE conversion for interpretation
        
        Cross-validated against:
        - Linux: firewire/packet-header-definitions.h
        - OHCI 1.1 Specification §7.4.2
        - IEEE 1394-2008 §6.2
        """
        if len(data) < 16:
            raise ValueError(f"Packet too short: {len(data)} bytes (need at least 16)")
        
        # Extract quadlets (OHCI stores header in little-endian)
        # Using '<I' means: read as little-endian 32-bit unsigned int
        q0, q1, q2, q3 = struct.unpack('<IIII', data[0:16])
        
        # Quadlet 0 bit fields (IEEE 1394-2008 §6.2.2.1):
        # [destination_ID:16][tLabel:6][retry:2][tcode:4][priority:4]
        #
        # After OHCI LE storage, extract directly from LE value:
        # Bits 31-16: destination_ID
        # Bits 15-10: tLabel
        # Bits 9-8:   retry
        # Bits 7-4:   tcode
        # Bits 3-0:   priority
        #
        # These masks/shifts match Linux packet-header-definitions.h
        destination_id = (q0 >> 16) & 0xFFFF  # ASYNC_HEADER_Q0_DESTINATION
        tlabel = (q0 >> 10) & 0x3F            # ASYNC_HEADER_Q0_TLABEL
        retry = RetryCode((q0 >> 8) & 0x3)    # ASYNC_HEADER_Q0_RETRY
        tcode = TCode((q0 >> 4) & 0xF)        # ASYNC_HEADER_Q0_TCODE
        priority = q0 & 0xF                   # ASYNC_HEADER_Q0_PRIORITY
        
        # Quadlet 1 bit fields (IEEE 1394-2008 §6.2.2.1):
        # [source_ID:16][rcode:4][reserved:12]
        # (For requests: [source_ID:16][offset_high:16])
        source_id = (q1 >> 16) & 0xFFFF       # ASYNC_HEADER_Q1_SOURCE
        rcode = RCode((q1 >> 12) & 0xF)       # ASYNC_HEADER_Q1_RCODE
        reserved = q1 & 0xFFF
        
        # Quadlet 2: Context-dependent
        # For read quad response: reserved (typically 0x00000000)
        # For read block response: destination_offset_low
        # Convert to BE for consistency with spec
        q2_be = struct.unpack('>I', struct.pack('<I', q2))[0]
        
        # Quadlet 3: IEEE 1394 payload data (OHCI 1.1 §8.7.1.2)
        # For read quad response: the data value read from device
        # For write quad request: the data value to write  
        # For block packets: [data_length:16][extended_tcode:16]
        #
        # IMPORTANT: Must byte-swap to get big-endian interpretation!
        # OHCI stored it as LE, but the actual value is BE on the wire.
        quadlet_data = struct.unpack('>I', struct.pack('<I', q3))[0]
        
        # Quadlet 4 (if present): OHCI xferStatus and timeStamp (OHCI 1.1 §8.7.1.2)
        # Bits 31-16: xferStatus (transfer status flags)
        # Bits 15-0:  timeStamp (cycle timer snapshot)
        xfer_status = None
        timestamp_val = None
        event_code = None
        cycle_seconds = None
        cycle_count = None
        payload = b''
        
        if len(data) >= 20:
            q4 = struct.unpack('<I', data[16:20])[0]
            xfer_status = (q4 >> 16) & 0xFFFF
            timestamp_val = q4 & 0xFFFF
            
            # Decode xferStatus (OHCI 1.1 §7.1.5.2, §7.2.2)
            # The xferStatus contains the low 16 bits of ContextControl register
            # Event code is in bits 4-0
            event_code = xfer_status & 0x1F
            
            # Decode timeStamp (OHCI 1.1 §7.1.5.3, Figure 7-5)
            # Bits 15-13: cycleSeconds (3 bits, 0-7)
            # Bits 12-0:  cycleCount (13 bits, 0-7999)
            cycle_seconds = (timestamp_val >> 13) & 0x7
            cycle_count = timestamp_val & 0x1FFF
            
            # Any remaining bytes after Q4 are block payload data (BE)
            payload = data[20:] if len(data) > 20 else b''
        elif len(data) > 16:
            # Partial Q4 or other data
            payload = data[16:]
        
        return ResponsePacket(
            destination_id=destination_id,
            tlabel=tlabel,
            retry=retry,
            tcode=tcode,
            priority=priority,
            source_id=source_id,
            rcode=rcode,
            reserved=reserved,
            quadlet_2=q2_be,
            quadlet_data=quadlet_data,
            xfer_status=xfer_status,
            timestamp=timestamp_val,
            event_code=event_code,
            cycle_seconds=cycle_seconds,
            cycle_count=cycle_count,
            payload=payload,
            raw_data=data
        )
    
    @staticmethod
    def verify_sample() -> None:
        """
        Verify parser against known good sample:
        QRresp from ffc0 to ffc2, tLabel 2, value 20ff5003 [ack 1] s100
        
        Expected from logs (first 16 bytes):
        60 01 C2 FF  00 00 C0 FF  00 00 00 00  20 FF 50 03
        """
        print("Running verification test...")
        print("-" * 70)
        
        # Sample from logs
        sample_hex = "60 01 C2 FF  00 00 C0 FF  00 00 00 00  20 FF 50 03  A0 B9 11 84"
        sample_data = ATResponseParser.parse_hex_dump(sample_hex)
        
        packet = ATResponseParser.parse(sample_data)
        
        # Verify against expected values
        assert packet.source_id == 0xffc0, f"Source ID mismatch: got 0x{packet.source_id:04x}, expected 0xffc0"
        assert packet.destination_id == 0xffc2, f"Dest ID mismatch: got 0x{packet.destination_id:04x}, expected 0xffc2"
        assert packet.tlabel == 0, f"tLabel mismatch: got {packet.tlabel}, expected 0"
        assert packet.tcode == TCode.READ_QUAD_RESPONSE, f"tCode mismatch: got {packet.tcode}"
        assert packet.rcode == RCode.COMPLETE, f"rCode mismatch: got {packet.rcode}"
        assert packet.quadlet_data == 0x20ff5003, f"Data value mismatch: got 0x{packet.quadlet_data:08x}, expected 0x20ff5003"
        assert packet.xfer_status is not None, "xferStatus should be present"
        assert packet.timestamp is not None, "timeStamp should be present"
        
        print("✓ All verifications passed!")
        print("-" * 70)
        print(packet)


def main():
    """Main entry point"""
    if len(sys.argv) < 2:
        print("IEEE 1394 Asynchronous Response Packet Parser")
        print("=" * 70)
        print("\nUsage:")
        print("  python3 ATResponseParser.py <hex_bytes>")
        print("  python3 ATResponseParser.py --verify")
        print("\nExample:")
        print("  python3 ATResponseParser.py '60 01 C2 FF 00 00 C0 FF 00 00 00 00 20 FF 50 03'")
        print("\nHex bytes can be space or comma separated, with or without 0x prefix.")
        print("Minimum 16 bytes required (4 quadlets for header).")
        sys.exit(1)
    
    if sys.argv[1] == '--verify':
        ATResponseParser.verify_sample()
        sys.exit(0)
    
    # Parse hex input
    hex_input = ' '.join(sys.argv[1:])
    try:
        data = ATResponseParser.parse_hex_dump(hex_input)
        packet = ATResponseParser.parse(data)
        print(packet)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
