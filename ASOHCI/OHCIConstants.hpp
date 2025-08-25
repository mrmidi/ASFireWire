// OHCIConstants.hpp
// Centralized OHCI 1394 register offsets, bit masks, and sizes

#pragma once

#include <stdint.h>
#include <stddef.h>

// ------------------------ Register Offsets ------------------------
static constexpr uint32_t kOHCI_Version                 = 0x000;
static constexpr uint32_t kOHCI_BusOptions              = 0x020;
static constexpr uint32_t kOHCI_GUIDHi                  = 0x024;
static constexpr uint32_t kOHCI_GUIDLo                  = 0x028;
static constexpr uint32_t kOHCI_HCControlSet            = 0x050;
static constexpr uint32_t kOHCI_HCControlClear          = 0x054;
static constexpr uint32_t kOHCI_SelfIDBuffer            = 0x064;
static constexpr uint32_t kOHCI_SelfIDCount             = 0x068;
static constexpr uint32_t kOHCI_IntEvent                = 0x080;
static constexpr uint32_t kOHCI_IntEventClear           = 0x084;
static constexpr uint32_t kOHCI_IntMaskSet              = 0x088;
static constexpr uint32_t kOHCI_IntMaskClear            = 0x08C;
static constexpr uint32_t kOHCI_IsoXmitIntEventClear    = 0x094;
static constexpr uint32_t kOHCI_IsoXmitIntMaskClear     = 0x09C;
static constexpr uint32_t kOHCI_IsoRecvIntEventClear    = 0x0A4;
static constexpr uint32_t kOHCI_IsoRecvIntMaskClear     = 0x0AC;
static constexpr uint32_t kOHCI_NodeID                  = 0x0E8;
static constexpr uint32_t kOHCI_PhyControl              = 0x0EC;
// Link Control (OHCI 1.1 Register Set — Link Control)
static constexpr uint32_t kOHCI_LinkControl             = 0x0F0;
static constexpr uint32_t kOHCI_LinkControlSet          = 0x0F4;
static constexpr uint32_t kOHCI_LinkControlClear        = 0x0F8;

// ------------------------ HCControl Bits --------------------------
static constexpr uint32_t kOHCI_HCControl_SoftReset     = 0x00010000;
static constexpr uint32_t kOHCI_HCControl_LinkEnable    = 0x00020000;
static constexpr uint32_t kOHCI_HCControl_PostedWriteEn = 0x00040000;
static constexpr uint32_t kOHCI_HCControl_LPS           = 0x00080000;
// Endianness: HcControl[noByteSwapData]
static constexpr uint32_t kOHCI_HCControl_NoByteSwap    = 0x40000000;

// ------------------------ Interrupt Bits --------------------------
// (Spec: OHCI 1.1 Interrupts — Interrupt Event/Mask table.)
static constexpr uint32_t kOHCI_Int_SelfIDComplete      = 0x00010000;
static constexpr uint32_t kOHCI_Int_BusReset            = 0x00020000;
static constexpr uint32_t kOHCI_Int_MasterEnable        = 0x80000000;
// Low-order async / error bits (see Interrupt Event/Mask table)
static constexpr uint32_t kOHCI_Int_ARRS                = 0x00000008;  // Async Rcv Rsp Service
static constexpr uint32_t kOHCI_Int_RqPkt               = 0x00000010;  // Async Request packet received
static constexpr uint32_t kOHCI_Int_RsPkt               = 0x00000020;  // Async Response packet received
static constexpr uint32_t kOHCI_Int_IsochTx             = 0x00000040;  // Isochronous transmit
static constexpr uint32_t kOHCI_Int_IsochRx             = 0x00000080;  // Isochronous receive
static constexpr uint32_t kOHCI_Int_PostedWriteErr      = 0x00000100;  // Posted write error
static constexpr uint32_t kOHCI_Int_LockRespErr         = 0x00000200;  // Lock response error
static constexpr uint32_t kOHCI_Int_RegAccessFail       = 0x00040000;  // Register access failure
static constexpr uint32_t kOHCI_Int_Phy                 = 0x00080000;  // PHY event
static constexpr uint32_t kOHCI_Int_CycleSynch          = 0x00100000;  // Cycle start sync
static constexpr uint32_t kOHCI_Int_Cycle64Seconds      = 0x00200000;  // 64-second tick
static constexpr uint32_t kOHCI_Int_CycleLost           = 0x00400000;  // Cycle lost
static constexpr uint32_t kOHCI_Int_CycleInconsistent   = 0x00800000;  // Cycle inconsistent
static constexpr uint32_t kOHCI_Int_UnrecoverableError  = 0x01000000;  // Unrecoverable error
static constexpr uint32_t kOHCI_Int_CycleTooLong        = 0x02000000;  // Cycle too long
static constexpr uint32_t kOHCI_Int_PhyRegRcvd          = 0x04000000;  // PHY register packet received

// ------------------------ LinkControl Bits -------------------------
// OHCI 1.1: rcvSelfID=bit9, rcvPhyPkt=bit10 (Link Control fields)
static constexpr uint32_t kOHCI_LC_RcvSelfID            = (1u << 9);
static constexpr uint32_t kOHCI_LC_RcvPhyPkt            = (1u << 10);
static constexpr uint32_t kOHCI_LC_CycleTimerEnable     = (1u << 20);
static constexpr uint32_t kOHCI_LC_CycleMaster          = (1u << 21);

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
static constexpr uint32_t kOHCI_AsReqRcvContextBase     = 0x0200;
static constexpr uint32_t kOHCI_AsReqRcvContextControlC = 0x0204;
static constexpr uint32_t kOHCI_AsReqRcvContextControlS = 0x0208;
static constexpr uint32_t kOHCI_AsReqRcvCommandPtr      = 0x020C;
static constexpr uint32_t kOHCI_AsRspRcvContextBase     = 0x0220;
static constexpr uint32_t kOHCI_AsRspRcvContextControlC = 0x0224;
static constexpr uint32_t kOHCI_AsRspRcvContextControlS = 0x0228;
static constexpr uint32_t kOHCI_AsRspRcvCommandPtr      = 0x022C;

// Context Control bits (Run only for initial scaffolding)
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
