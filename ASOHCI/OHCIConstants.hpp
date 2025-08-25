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
static constexpr uint32_t kOHCI_LinkControlSet          = 0x0E0;
static constexpr uint32_t kOHCI_LinkControlClear        = 0x0E4;

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
// Per OHCI 1.1 and Linux OHCI: rcvSelfID=bit9, rcvPhyPkt=bit10
static constexpr uint32_t kOHCI_LC_RcvSelfID            = (1u << 9);
static constexpr uint32_t kOHCI_LC_RcvPhyPkt            = (1u << 10);

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
