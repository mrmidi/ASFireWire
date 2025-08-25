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

// ------------------------ Interrupt Bits --------------------------
static constexpr uint32_t kOHCI_Int_SelfIDComplete      = 0x00010000;
static constexpr uint32_t kOHCI_Int_BusReset            = 0x00020000;
static constexpr uint32_t kOHCI_Int_MasterEnable        = 0x80000000;

// ------------------------ LinkControl Bits -------------------------
// OHCI 1.1: rcvSelfID=bit9, rcvPhyPkt=bit10 (Link Control fields)
static constexpr uint32_t kOHCI_LC_RcvSelfID            = (1u << 9);
static constexpr uint32_t kOHCI_LC_RcvPhyPkt            = (1u << 10);

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

// ------------------------ Self‑ID parse ---------------------------
static constexpr uint32_t kSelfID_PhyID_Mask        = 0xFC000000;
static constexpr uint32_t kSelfID_PhyID_Shift       = 26;
static constexpr uint32_t kSelfID_LinkActive_Mask   = 0x02000000;
static constexpr uint32_t kSelfID_GapCount_Mask     = 0x01FC0000;
static constexpr uint32_t kSelfID_GapCount_Shift    = 18;
static constexpr uint32_t kSelfID_Speed_Mask        = 0x0000C000;
static constexpr uint32_t kSelfID_Speed_Shift       = 14;
static constexpr uint32_t kSelfID_Contender_Mask    = 0x00000800;
static constexpr uint32_t kSelfID_PowerClass_Mask   = 0x00000700;

// ------------------------ Driver constants ------------------------
static constexpr size_t   kSelfIDBufferSize  = 2048; // 1–2KB typical
static constexpr size_t   kSelfIDBufferAlign = 4;
