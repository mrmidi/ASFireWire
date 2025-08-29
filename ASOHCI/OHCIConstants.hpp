// OHCIConstants.hpp
// Centralized OHCI 1394 register offsets, bit masks, and sizes

#pragma once

#include <stdint.h>
#include <stddef.h>

// ------------------------ Register Offsets ------------------------
// Identification / ROM / CSR block
static constexpr uint32_t kOHCI_Version                 = 0x000;
static constexpr uint32_t kOHCI_CSRData                 = 0x00C;
static constexpr uint32_t kOHCI_CSRCompareData          = 0x010;
static constexpr uint32_t kOHCI_CSRControl              = 0x014;
static constexpr uint32_t kOHCI_ConfigROMhdr            = 0x018;
static constexpr uint32_t kOHCI_BusID                   = 0x01C;
static constexpr uint32_t kOHCI_BusOptions              = 0x020;
static constexpr uint32_t kOHCI_GUIDHi                  = 0x024;
static constexpr uint32_t kOHCI_GUIDLo                  = 0x028;
static constexpr uint32_t kOHCI_PostedWriteAddressLo    = 0x038;
static constexpr uint32_t kOHCI_PostedWriteAddressHi    = 0x03C;
static constexpr uint32_t kOHCI_VendorID                = 0x040;
static constexpr uint32_t kOHCI_ConfigROMmap            = 0x034; // Config ROM base address (device view)

// Host Control
static constexpr uint32_t kOHCI_HCControlSet            = 0x050;
static constexpr uint32_t kOHCI_HCControlClear          = 0x054;

// Self-ID DMA
static constexpr uint32_t kOHCI_SelfIDBuffer            = 0x064;
static constexpr uint32_t kOHCI_SelfIDCount             = 0x068;

// AT Retries Register (OHCI 1.1 §5.4)
static constexpr uint32_t kOHCI_ATRetries               = 0x06C;

// Interrupt event / mask sets
// Some code refers to kOHCI_IntEvent for a read of the raw event register at 0x080.
// OHCI defines separate write-1-to-set / write-1-to-clear views; reading either
// address (0x080) yields current event bits. Provide both names for clarity.
static constexpr uint32_t kOHCI_IntEvent               = 0x080; // read current events
static constexpr uint32_t kOHCI_IntEventSet            = 0x080; // write-1-to-set
static constexpr uint32_t kOHCI_IntEventClear           = 0x084; // write-1-to-clear
static constexpr uint32_t kOHCI_IntMaskSet              = 0x088;
static constexpr uint32_t kOHCI_IntMaskClear            = 0x08C;
static constexpr uint32_t kOHCI_IsoXmitIntEventSet      = 0x090;
static constexpr uint32_t kOHCI_IsoXmitIntEventClear    = 0x094;
static constexpr uint32_t kOHCI_IsoXmitIntMaskSet       = 0x098;
static constexpr uint32_t kOHCI_IsoXmitIntMaskClear     = 0x09C;
static constexpr uint32_t kOHCI_IsoRecvIntEventSet      = 0x0A0;
static constexpr uint32_t kOHCI_IsoRecvIntEventClear    = 0x0A4;
static constexpr uint32_t kOHCI_IsoRecvIntMaskSet       = 0x0A8;
static constexpr uint32_t kOHCI_IsoRecvIntMaskClear     = 0x0AC;

// Fairness / bandwidth
static constexpr uint32_t kOHCI_InitialBandwidthAvail   = 0x0B0;
static constexpr uint32_t kOHCI_InitialChannelsAvailHi  = 0x0B4;
static constexpr uint32_t kOHCI_InitialChannelsAvailLo  = 0x0B8;
static constexpr uint32_t kOHCI_FairnessControl         = 0x0DC;

// Link / Node / PHY / Cycle timer
static constexpr uint32_t kOHCI_LinkControlSet          = 0x0E0; // corrected
static constexpr uint32_t kOHCI_LinkControlClear        = 0x0E4; // corrected
static constexpr uint32_t kOHCI_NodeID                  = 0x0E8;
static constexpr uint32_t kOHCI_PhyControl              = 0x0EC;
static constexpr uint32_t kOHCI_CycleTimer              = 0x0F0; // was kOHCI_LinkControl

// ------------------------ HCControl Bits --------------------------
static constexpr uint32_t kOHCI_HCControl_SoftReset     = 0x00010000;
static constexpr uint32_t kOHCI_HCControl_LinkEnable    = 0x00020000;
static constexpr uint32_t kOHCI_HCControl_PostedWriteEn = 0x00040000;
static constexpr uint32_t kOHCI_HCControl_LPS           = 0x00080000;
// Endianness: HcControl[noByteSwapData]
static constexpr uint32_t kOHCI_HCControl_NoByteSwap      = 0x40000000;
// Aliases for spec terminology (reuse same bit values)
static constexpr uint32_t kOHCI_HCControl_CycleSynch      = 0x00100000;
static constexpr uint32_t kOHCI_HCControl_Cycle64Seconds  = 0x00200000;
static constexpr uint32_t kOHCI_HCControl_aPhyEnhanceEnable = 0x00400000;
static constexpr uint32_t kOHCI_HCControl_programPhyEnable = 0x00800000;
static constexpr uint32_t kOHCI_HCControl_BIBimageValid     = 0x80000000;

// ------------------------ Interrupt Bits --------------------------
// Complete OHCI 1.1 IntEvent register bit definitions (§6.1 Table 6-1)
// Register offset: 0x080 (set) / 0x084 (clear)

// DMA Completion Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 0-7)
static constexpr uint32_t kOHCI_Int_ReqTxComplete       = 0x00000001;  // Bit 0: AT request DMA complete (§7.6)
static constexpr uint32_t kOHCI_Int_RespTxComplete      = 0x00000002;  // Bit 1: AT response DMA complete (§7.6)
static constexpr uint32_t kOHCI_Int_ARRQ                = 0x00000004;  // Bit 2: AR request DMA complete (§8.6)
static constexpr uint32_t kOHCI_Int_ARRS                = 0x00000008;  // Bit 3: AR response DMA complete (§8.6)
static constexpr uint32_t kOHCI_Int_RqPkt               = 0x00000010;  // Bit 4: AR request packet received (§8.6)
static constexpr uint32_t kOHCI_Int_RsPkt               = 0x00000020;  // Bit 5: AR response packet received (§8.6)
static constexpr uint32_t kOHCI_Int_IsochTx             = 0x00000040;  // Bit 6: Isochronous transmit (§6.3)
static constexpr uint32_t kOHCI_Int_IsochRx             = 0x00000080;  // Bit 7: Isochronous receive (§6.4)

// Error Condition Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 8-9)
static constexpr uint32_t kOHCI_Int_PostedWriteErr      = 0x00000100;  // Bit 8: Posted write error (§13.2.8.1)
static constexpr uint32_t kOHCI_Int_LockRespErr         = 0x00000200;  // Bit 9: Lock response error (§5.5.1)

// Bits 10-14: Reserved per OHCI 1.1 specification

// Self-ID and Bus Management Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 15-17)
static constexpr uint32_t kOHCI_Int_SelfIDComplete2     = 0x00008000;  // Bit 15: Secondary Self-ID complete (§11.5)
static constexpr uint32_t kOHCI_Int_SelfIDComplete      = 0x00010000;  // Bit 16: Self-ID complete (§11.5)
static constexpr uint32_t kOHCI_Int_BusReset            = 0x00020000;  // Bit 17: PHY bus reset mode (§6.1.1)

// System and PHY Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 18-19)
static constexpr uint32_t kOHCI_Int_RegAccessFail       = 0x00040000;  // Bit 18: Register access failed (missing SCLK)
static constexpr uint32_t kOHCI_Int_Phy                 = 0x00080000;  // Bit 19: PHY status transfer request

// Cycle Timer and Management Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 20-25)
static constexpr uint32_t kOHCI_Int_CycleSynch          = 0x00100000;  // Bit 20: New isochronous cycle started
static constexpr uint32_t kOHCI_Int_Cycle64Seconds      = 0x00200000;  // Bit 21: 64-second counter bit 7 changed
static constexpr uint32_t kOHCI_Int_CycleLost           = 0x00400000;  // Bit 22: No cycle_start between cycleSynch events
static constexpr uint32_t kOHCI_Int_CycleInconsistent   = 0x00800000;  // Bit 23: Cycle timer mismatch (§5.13)
static constexpr uint32_t kOHCI_Int_UnrecoverableError  = 0x01000000;  // Bit 24: Context dead or fatal error
static constexpr uint32_t kOHCI_Int_CycleTooLong        = 0x02000000;  // Bit 25: Cycle >120μsec (root node only)

// High-Order Interrupts (OHCI 1.1 §6.1 Table 6-1 bits 26-31)
static constexpr uint32_t kOHCI_Int_PhyRegRcvd          = 0x04000000;  // Bit 26: PHY register packet received
static constexpr uint32_t kOHCI_Int_AckTardy            = 0x08000000;  // Bit 27: Late acknowledgment received
static constexpr uint32_t kOHCI_Int_SoftInterrupt       = 0x10000000;  // Bit 28: Software-generated interrupt
static constexpr uint32_t kOHCI_Int_VendorSpecific      = 0x20000000;  // Bit 29: Implementation-specific interrupt
// Bit 30: Reserved per OHCI 1.1 specification
static constexpr uint32_t kOHCI_Int_MasterEnable        = 0x80000000;  // Bit 31: Master interrupt enable

// ------------------------ LinkControl Bits -------------------------
static constexpr uint32_t kOHCI_LC_RcvSelfID            = (1u << 9);
static constexpr uint32_t kOHCI_LC_RcvPhyPkt            = (1u << 10);
static constexpr uint32_t kOHCI_LC_CycleTimerEnable     = (1u << 20);
static constexpr uint32_t kOHCI_LC_CycleMaster          = (1u << 21);

// ------------------------ PHY Request Filter & Upper Bound ---------
// NOTE: Original definitions overlapped Async Response Filter (0x0110-0x011C).
// OHCI 1.1 places Async Request/Response filters at 0x0100-0x011C and the Physical Response Upper Bound at 0x0120.
// There is no separate PHY Request Filter block occupying 0x0110..0x011C; retain only Upper Bound at 0x0120.
// If implementation provides vendor-specific PHY request filtering, map it outside standard filter window.
// (Removed conflicting kOHCI_PhyReqFilter* aliases.)
static constexpr uint32_t kOHCI_PhyUpperBound           = 0x0120;

// ------------------------ Async Receive Filters (OHCI 1.1 §7.*) ---
static constexpr uint32_t kOHCI_AsReqFilterHiSet        = 0x0100;
static constexpr uint32_t kOHCI_AsReqFilterHiClear      = 0x0104;
static constexpr uint32_t kOHCI_AsReqFilterLoSet        = 0x0108;
static constexpr uint32_t kOHCI_AsReqFilterLoClear      = 0x010C;
static constexpr uint32_t kOHCI_AsRspFilterHiSet        = 0x0110;
static constexpr uint32_t kOHCI_AsRspFilterHiClear      = 0x0114;
static constexpr uint32_t kOHCI_AsRspFilterLoSet        = 0x0118;
static constexpr uint32_t kOHCI_AsRspFilterLoClear      = 0x011C;

// ------------------------ Async Contexts (OHCI 1.1 §7.*) ----------
// Transmit (ATReq / ATRsp)
static constexpr uint32_t kOHCI_AsReqTrContextBase      = 0x0180;
static constexpr uint32_t kOHCI_AsReqTrContextControlC  = 0x0184; // ControlClear
static constexpr uint32_t kOHCI_AsReqTrContextControlS  = 0x0188; // ControlSet
static constexpr uint32_t kOHCI_AsReqTrCommandPtr       = 0x018C;
static constexpr uint32_t kOHCI_AsRspTrContextBase      = 0x01A0;
static constexpr uint32_t kOHCI_AsRspTrContextControlC  = 0x01A4;
static constexpr uint32_t kOHCI_AsRspTrContextControlS  = 0x01A8;
static constexpr uint32_t kOHCI_AsRspTrCommandPtr       = 0x01AC;
// Receive (ARReq / ARRsp)
// Async Receive Request Context (OHCI 1.1 §8.3) base 0x1C0 block
static constexpr uint32_t kOHCI_AsReqRcvContextBase     = 0x01C0;
static constexpr uint32_t kOHCI_AsReqRcvContextControlC = 0x01C4;
static constexpr uint32_t kOHCI_AsReqRcvContextControlS = 0x01C8;
static constexpr uint32_t kOHCI_AsReqRcvCommandPtr      = 0x01CC;
// Async Receive Response Context (OHCI 1.1 §8.3) base 0x1E0 block
static constexpr uint32_t kOHCI_AsRspRcvContextBase     = 0x01E0;
static constexpr uint32_t kOHCI_AsRspRcvContextControlC = 0x01E4;
static constexpr uint32_t kOHCI_AsRspRcvContextControlS = 0x01E8;
static constexpr uint32_t kOHCI_AsRspRcvCommandPtr      = 0x01EC;

// ------------------------ Isochronous Transmit Contexts (OHCI 1.1 §9.2) ---------
// Each IT context n has 16-byte spaced registers: ControlSet(n), ControlClear(n), CommandPtr(n)
// Base pattern (matches common Linux mapping): base 0x0200 + 0x10 * n
// (Separate from Async Receive which follows later in map.)
static constexpr uint32_t kOHCI_IsoXmitContextBase(uint32_t n)        { return 0x0200 + 0x10 * n; }
static constexpr uint32_t kOHCI_IsoXmitContextControlSet(uint32_t n)  { return 0x0200 + 0x10 * n; }   // §9.2
static constexpr uint32_t kOHCI_IsoXmitContextControlClear(uint32_t n){ return 0x0204 + 0x10 * n; }   // §9.2
static constexpr uint32_t kOHCI_IsoXmitCommandPtr(uint32_t n)         { return 0x020C + 0x10 * n; }   // §9.1/§9.2

// Legacy context control bit (deprecated - use kOHCI_ContextControl_* definitions above)
static constexpr uint32_t kOHCI_Context_Run             = (1u << 15);

// ------------------------ Self‑ID parse (IEEE 1394-2008 Alpha §16.3.2.1) ---------------------------
// Top two bits identify self-ID quadlets: b31..b30 == 10b
static constexpr uint32_t kSelfID_Tag_Mask          = 0xC0000000;
static constexpr uint32_t kSelfID_Tag_SelfID        = 0x80000000; // 10b

// Common header fields (#0/#1/#2)
static constexpr uint32_t kSelfID_PhyID_Mask        = 0x3F000000; // b29..b24
static constexpr uint32_t kSelfID_PhyID_Shift       = 24;
static constexpr uint32_t kSelfID_IsExtended_Mask   = 0x00800000; // b23: 0=#0, 1=#1/#2

// Packet #0 fields (Table 16-4)
static constexpr uint32_t kSelfID_LinkActive_Mask   = 0x00400000; // L, b22
static constexpr uint32_t kSelfID_GapCount_Mask     = 0x003F8000; // gap_cnt, b21..b16
static constexpr uint32_t kSelfID_GapCount_Shift    = 16;
static constexpr uint32_t kSelfID_Speed_Mask        = 0x0000C000; // sp, b15..b14 (00=S100,01=S200,10=S400,11=reserved)
static constexpr uint32_t kSelfID_Speed_Shift       = 14;
static constexpr uint32_t kSelfID_Delay_Mask        = 0x00001000; // del, b12
static constexpr uint32_t kSelfID_Contender_Mask    = 0x00000800; // c, b11
static constexpr uint32_t kSelfID_PowerClass_Mask   = 0x00000700; // pwr, b10..b8 (Alpha 3/7/15W)
static constexpr uint32_t kSelfID_P0_Mask           = 0x000000C0; // b7..b6
static constexpr uint32_t kSelfID_P1_Mask           = 0x00000030; // b5..b4
static constexpr uint32_t kSelfID_P2_Mask           = 0x0000000C; // b3..b2
static constexpr uint32_t kSelfID_Initiated_Mask    = 0x00000002; // i, b1
static constexpr uint32_t kSelfID_More_Mask         = 0x00000001; // m, b0

// Extended packets (#1/#2): sequence n (valid n=0 for #1, n=1 for #2)
static constexpr uint32_t kSelfID_SeqN_Mask         = 0x00700000; // b22..b20
static constexpr uint32_t kSelfID_SeqN_Shift        = 20;

// ------------------------ Driver constants ------------------------
static constexpr size_t   kSelfIDBufferSize  = 2048; // 1–2KB typical
static constexpr size_t   kSelfIDBufferAlign = 4;

// Port status codes (Alpha) for 2-bit pX fields (Table 16-4)
enum : uint8_t {
    kSelfIDPort_NotPresent = 0,
    kSelfIDPort_NotActive  = 1,
    kSelfIDPort_Parent     = 2,
    kSelfIDPort_Child      = 3,
};

// ------------------------ NodeID Register Field Masks -------------
static constexpr uint32_t kOHCI_NodeID_busNumber        = 0x0000FFC0;
static constexpr uint32_t kOHCI_NodeID_nodeNumber       = 0x0000003F;
static constexpr uint32_t kOHCI_NodeID_idValid          = 0x80000000; // alias to Int_MasterEnable
static constexpr uint32_t kOHCI_NodeID_root             = 0x40000000;

// ------------------------ SelfIDCount Field Masks -----------------
static constexpr uint32_t kOHCI_SelfIDCount_selfIDError      = 0x80000000;
static constexpr uint32_t kOHCI_SelfIDCount_selfIDGeneration = 0x00FF0000;
static constexpr uint32_t kOHCI_SelfIDCount_selfIDSize       = 0x000007FC;

// ------------------------ PHY Control Bits (OHCI 1.1 §5.12) ------------------------
// PHY Control Register bit definitions (Table 5-19)
static constexpr uint32_t kOHCI_PhyControl_rdDone       = 0x80000000; // Bit 31: read completion status
static constexpr uint32_t kOHCI_PhyControl_rdReg        = 0x00008000; // Bit 15: initiate read request
static constexpr uint32_t kOHCI_PhyControl_wrReg        = 0x00004000; // Bit 14: initiate write request
// Field masks
static constexpr uint32_t kOHCI_PhyControl_rdAddr_Mask  = 0x0F000000; // Bits 27-24: read address
static constexpr uint32_t kOHCI_PhyControl_rdData_Mask  = 0x00FF0000; // Bits 23-16: read data
static constexpr uint32_t kOHCI_PhyControl_regAddr_Mask = 0x000007C0; // Bits 10-6: register address  
static constexpr uint32_t kOHCI_PhyControl_wrData_Mask  = 0x000000FF; // Bits 7-0: write data
// Field shift constants
static constexpr uint32_t kOHCI_PhyControl_rdAddr_Shift = 24;
static constexpr uint32_t kOHCI_PhyControl_rdData_Shift = 16;
static constexpr uint32_t kOHCI_PhyControl_regAddr_Shift = 6;
static constexpr uint32_t kOHCI_PhyControl_wrData_Shift = 0;
// Legacy compatibility aliases
static constexpr uint32_t kOHCI_PhyControl_ReadDone     = kOHCI_PhyControl_rdDone;
static constexpr uint32_t kOHCI_PhyControl_WritePending = kOHCI_PhyControl_wrReg;

// ------------------------ PHY Register Constants ------------------
// PHY Register 4 bit definitions (IEEE 1394a PHY)
static constexpr uint8_t  kPHY_REG_4                    = 4;          // PHY register 4 address
static constexpr uint8_t  kPHY_LINK_ACTIVE              = 0x40;       // Link Active bit (L)
static constexpr uint8_t  kPHY_CONTENDER               = 0x08;       // Contender bit (C)

// Commonly used additional PHY registers/bits
static constexpr uint8_t  kPHY_REG_1                    = 1;          // PHY register 1 address (vendor/ID/reset bits)
static constexpr uint8_t  kPHY_INITIATE_BUS_RESET       = 0x40;       // IBR bit: Initiate Bus Reset (IEEE 1394a)

// ------------------------ OHCI DMA Context Control Bits (OHCI 1.1 §3.1.1) ------------------------
// ContextControl register bit definitions - used by all DMA contexts
static constexpr uint32_t kOHCI_ContextControl_run      = 0x00008000; // Bit 15: Enable descriptor processing
static constexpr uint32_t kOHCI_ContextControl_wake     = 0x00000400; // Bit 10: Resume/continue processing
static constexpr uint32_t kOHCI_ContextControl_dead     = 0x00000800; // Bit 11: Fatal error occurred
static constexpr uint32_t kOHCI_ContextControl_active   = 0x00000200; // Bit 9: Currently processing descriptors

// Event code field (bits 4-0) per OHCI 1.1 Table 3-2
static constexpr uint32_t kOHCI_ContextControl_evtCode_Mask  = 0x0000001F; // Bits 4-0: Event code
static constexpr uint32_t kOHCI_ContextControl_spd_Mask     = 0x000000E0; // Bits 7-5: Speed indication

// Event codes for context completion (OHCI 1.1 Table 3-2)
static constexpr uint32_t kOHCI_EvtCode_NoStatus        = 0x00; // evt_no_status
static constexpr uint32_t kOHCI_EvtCode_MissingAck      = 0x03; // evt_missing_ack
static constexpr uint32_t kOHCI_EvtCode_Underrun       = 0x04; // evt_underrun
static constexpr uint32_t kOHCI_EvtCode_Overrun        = 0x05; // evt_overrun
static constexpr uint32_t kOHCI_EvtCode_DescriptorRead = 0x06; // evt_descriptor_read
static constexpr uint32_t kOHCI_EvtCode_DataRead       = 0x07; // evt_data_read
static constexpr uint32_t kOHCI_EvtCode_DataWrite      = 0x08; // evt_data_write
static constexpr uint32_t kOHCI_EvtCode_BusReset       = 0x09; // evt_bus_reset
static constexpr uint32_t kOHCI_EvtCode_AckComplete    = 0x11; // ack_complete
static constexpr uint32_t kOHCI_EvtCode_AckPending     = 0x12; // ack_pending

// ------------------------ OHCI DMA Descriptor Structures (OHCI 1.1 §7-8) ------------------------
// All descriptors are 16-byte aligned and use quadlet (32-bit) fields

// AR INPUT_MORE Descriptor (OHCI 1.1 §8.1.1) - 16 bytes
struct OHCI_ARInputMoreDescriptor {
    // First quadlet: command and control fields
    uint32_t cmd         : 4;   // Must be 0x2 for INPUT_MORE
    uint32_t key         : 3;   // Must be 0x0
    uint32_t reserved1   : 1;   // Reserved, must be 0
    uint32_t i           : 2;   // Interrupt control (0x3=interrupt on completion, 0x0=no interrupt)
    uint32_t b           : 2;   // Branch control (must be 0x3)
    uint32_t reserved2   : 4;   // Reserved, must be 0
    uint32_t reqCount    : 16;  // Buffer size in bytes (multiple of 4)
    
    // Second quadlet: data address
    uint32_t dataAddress;       // Host memory buffer address (quadlet-aligned)
    
    // Third quadlet: branch address and Z
    uint32_t branchAddress : 28; // Next descriptor block address (16-byte aligned)
    uint32_t Z            : 4;   // Descriptor count: 0=end, 1=next block
    
    // Fourth quadlet: status fields (updated by hardware)
    uint32_t resCount     : 16;  // Residual count (bytes not yet filled)
    uint32_t xferStatus   : 16;  // Copy of ContextControl[15:0] on completion
} __attribute__((packed, aligned(16)));

// AT OUTPUT_MORE Descriptor (OHCI 1.1 §7.1.1) - 16 bytes
struct OHCI_ATOutputMoreDescriptor {
    // First quadlet: command and control fields
    uint32_t cmd         : 4;   // Must be 0x0 for OUTPUT_MORE
    uint32_t key         : 3;   // Must be 0x0
    uint32_t reserved1   : 1;   // Reserved, must be 0
    uint32_t reserved2   : 2;   // Reserved, must be 0
    uint32_t b           : 2;   // Branch control (must be 0x0)
    uint32_t reserved3   : 4;   // Reserved, must be 0
    uint32_t reqCount    : 16;  // Transmit data size in bytes
    
    // Second quadlet: data address
    uint32_t dataAddress;       // Transmit data address (no alignment restrictions)
    
    // Third and fourth quadlets: reserved/unused for non-branch descriptors
    uint32_t reserved4;
    uint32_t reserved5;
} __attribute__((packed, aligned(16)));

// AT OUTPUT_LAST Descriptor (OHCI 1.1 §7.1.3) - 16 bytes  
struct OHCI_ATOutputLastDescriptor {
    // First quadlet: command and control fields
    uint32_t cmd         : 4;   // Must be 0x1 for OUTPUT_LAST
    uint32_t key         : 3;   // Must be 0x0
    uint32_t p           : 1;   // Ping timing (AT request only)
    uint32_t i           : 2;   // Interrupt control (0x3=always, 0x1=on error, 0x0=never)
    uint32_t b           : 2;   // Branch control (must be 0x3)
    uint32_t reserved1   : 4;   // Reserved, must be 0
    uint32_t reqCount    : 16;  // Transmit data size in bytes
    
    // Second quadlet: data address
    uint32_t dataAddress;       // Transmit data address (no alignment restrictions)
    
    // Third quadlet: branch address and Z
    uint32_t branchAddress : 28; // Next descriptor block address (16-byte aligned) 
    uint32_t Z            : 4;   // Next packet size: 0=end, 2-8=descriptor count
    
    // Fourth quadlet: status fields (updated by hardware)
    uint32_t timeStamp    : 16;  // Transmission timestamp or ping duration
    uint32_t xferStatus   : 16;  // Copy of ContextControl[15:0] on completion
} __attribute__((packed, aligned(16)));

// AT OUTPUT_MORE_Immediate Descriptor (OHCI 1.1 §7.1.2) - 32 bytes
struct OHCI_ATOutputMoreImmediateDescriptor {
    // First quadlet: command and control fields
    uint32_t cmd         : 4;   // Must be 0x0 for OUTPUT_MORE-Immediate
    uint32_t key         : 3;   // Must be 0x2 for OUTPUT_MORE-Immediate  
    uint32_t reserved1   : 1;   // Reserved, must be 0
    uint32_t reserved2   : 2;   // Reserved, must be 0
    uint32_t b           : 2;   // Branch control (must be 0x0)
    uint32_t reserved3   : 4;   // Reserved, must be 0
    uint32_t reqCount    : 16;  // Must be 8 (2 quadlets) or 16 (4 quadlets)
    
    // Second quadlet: timestamp (AT response only)
    uint32_t timeStamp   : 16;  // Expiration time for responses
    uint32_t reserved4   : 16;  // Reserved
    
    // Quadlets 3-6: packet header data (up to 4 quadlets)
    uint32_t firstQuadlet;      // First packet header quadlet
    uint32_t secondQuadlet;     // Second packet header quadlet  
    uint32_t thirdQuadlet;      // Third packet header quadlet (optional)
    uint32_t fourthQuadlet;     // Fourth packet header quadlet (optional)
} __attribute__((packed, aligned(16)));

// AT OUTPUT_LAST_Immediate Descriptor (OHCI 1.1 §7.1.4) - 32 bytes
struct OHCI_ATOutputLastImmediateDescriptor {
    // First quadlet: command and control fields
    uint32_t cmd         : 4;   // Must be 0x1 for OUTPUT_LAST-Immediate
    uint32_t key         : 3;   // Must be 0x2 for OUTPUT_LAST-Immediate
    uint32_t p           : 1;   // Ping timing (AT request only)
    uint32_t i           : 2;   // Interrupt control (0x3=always, 0x1=on error, 0x0=never)
    uint32_t b           : 2;   // Branch control (must be 0x3)
    uint32_t reserved1   : 4;   // Reserved, must be 0
    uint32_t reqCount    : 16;  // Must be 8, 12, or 16 bytes
    
    // Second quadlet: branch address and Z
    uint32_t branchAddress : 28; // Next descriptor block address (16-byte aligned)
    uint32_t Z            : 4;   // Next packet size: 0=end, 2-8=descriptor count
    
    // Third quadlet: status fields (updated by hardware)
    uint32_t timeStamp    : 16;  // Transmission timestamp or ping duration
    uint32_t xferStatus   : 16;  // Copy of ContextControl[15:0] on completion
    
    // Fourth quadlet: reserved
    uint32_t reserved2;
    
    // Quadlets 5-8: packet header data (2-4 quadlets)
    uint32_t firstQuadlet;      // First packet header quadlet
    uint32_t secondQuadlet;     // Second packet header quadlet
    uint32_t thirdQuadlet;      // Third packet header quadlet (optional)
    uint32_t fourthQuadlet;     // Fourth packet header quadlet (optional)
} __attribute__((packed, aligned(16)));

// CommandPtr Register Structure (OHCI 1.1 §3.1.2)
struct OHCI_CommandPtr {
    uint32_t descriptorAddress : 28; // Upper 28 bits of descriptor block address
    uint32_t Z                 : 4;  // Number of 16-byte blocks at address
} __attribute__((packed));

// Z value encoding constants (OHCI 1.1 Table 7-5)
static constexpr uint32_t kOHCI_Z_EndOfProgram    = 0;  // Last descriptor in program
static constexpr uint32_t kOHCI_Z_MinPacketSize   = 2;  // Minimum packet size (2 blocks)
static constexpr uint32_t kOHCI_Z_MaxPacketSize   = 8;  // Maximum packet size (8 blocks)

// Descriptor alignment requirements
static constexpr size_t   kOHCI_DescriptorAlign   = 16;  // All descriptors 16-byte aligned
static constexpr size_t   kOHCI_DescriptorSize    = 16;  // Standard descriptor size
static constexpr size_t   kOHCI_ImmediateDescSize = 32;  // *-Immediate descriptor size
