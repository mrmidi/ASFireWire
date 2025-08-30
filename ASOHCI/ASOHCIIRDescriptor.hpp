#pragma once
//
// ASOHCIIRDescriptor.hpp
// IR uses same 16B INPUT_MORE/INPUT_LAST descriptor format as AT for standard
// descriptors. DUALBUFFER descriptors are 32B and specific to IR dual-buffer
// mode.
//
// Spec refs (OHCI 1.1): §10.1 (IR DMA Context Programs), Table 10-1
// (INPUT_MORE/INPUT_LAST),
//   Table 10-2 (DUALBUFFER), §10.2.3 (dual-buffer mode processing)

#include "ASOHCIATDescriptor.hpp" // ATDesc::Descriptor + ATDesc::Program
#include <stdint.h>

namespace IRDesc {
// INPUT_MORE and INPUT_LAST use identical layout to AT descriptors (OHCI Table
// 10-1)
using Descriptor = ATDesc::Descriptor; // 16-byte, cmd/key/i/b fields same as AT
using Program =
    ATDesc::Program; // headPA/tailPA/Z/count (same CommandPtr rules)

// DUALBUFFER descriptor for dual-buffer mode (OHCI Table 10-2)
// 32-byte descriptor, aligned on 16-byte boundary
struct DualBufferDescriptor {
  // First quadlet: control fields
  uint32_t quad0; // s|key|i|b|w + firstSize (bits 31:16 = firstSize)

  // Buffer counts and addresses
  uint32_t firstReqCount_secondReqCount; // bits 31:16 = firstReqCount, bits
                                         // 15:0 = secondReqCount
  uint32_t branchAddress_Z; // bits 31:4 = branchAddress, bits 3:0 = Z
  uint32_t firstResCount_secondResCount; // bits 31:16 = firstResCount, bits
                                         // 15:0 = secondResCount

  // Buffer pointers
  uint32_t firstBuffer;  // Physical address of first buffer (quadlet aligned)
  uint32_t secondBuffer; // Physical address of second buffer

  // Reserved for future use or padding
  uint32_t reserved1;
  uint32_t reserved2;

  // Helper methods for field access (OHCI Table 10-2)
  void SetControl(bool statusEnable, uint8_t interruptPolicy,
                  uint8_t branchControl, uint8_t waitControl,
                  uint16_t firstSize) {
    quad0 = ((statusEnable ? 1 : 0) << 31) |
            (0x0 << 28) | // key = 0 (Table 10-2)
            ((interruptPolicy & 0x3) << 26) | ((branchControl & 0x3) << 24) |
            ((waitControl & 0x3) << 22) | (firstSize & 0xFFFF);
  }

  void SetCounts(uint16_t firstReq, uint16_t secondReq) {
    firstReqCount_secondReqCount =
        ((uint32_t)firstReq << 16) | (secondReq & 0xFFFF);
  }

  void SetBranchAndZ(uint32_t branchAddr, uint8_t zValue) {
    branchAddress_Z = (branchAddr & 0xFFFFFFF0) | (zValue & 0xF);
  }

  void InitializeResCounts(uint16_t firstReq, uint16_t secondReq) {
    firstResCount_secondResCount =
        ((uint32_t)firstReq << 16) | (secondReq & 0xFFFF);
  }
};
} // namespace IRDesc

// IR descriptor command constants (OHCI Table 10-1)
namespace IRDescOps {
static constexpr uint32_t kCmd_INPUT_MORE = 0x2; // cmd=2, key=0
static constexpr uint32_t kCmd_INPUT_LAST = 0x3; // cmd=3, key=0
static constexpr uint32_t kKey_STANDARD =
    0x0; // Standard key for INPUT_MORE/INPUT_LAST

// Branch control values (OHCI Table 10-1)
static constexpr uint32_t kBranch_Never =
    0x0; // INPUT_MORE in packet-per-buffer
static constexpr uint32_t kBranch_Always =
    0x3; // INPUT_LAST and buffer-fill mode

// Wait control values (OHCI Table 10-1)
static constexpr uint32_t kWait_NoWait = 0x0;    // Accept all packets
static constexpr uint32_t kWait_SyncMatch = 0x3; // Wait for sync field match

// DUALBUFFER Z values (OHCI Table 10-2)
static constexpr uint32_t kDualBuffer_Continue = 0x2; // branchAddress valid
static constexpr uint32_t kDualBuffer_End = 0x0;      // End of program
} // namespace IRDescOps

// IR program types for different receive modes
namespace IRProgram {
// Standard program using 16-byte descriptors
using StandardProgram = IRDesc::Program;

// Dual-buffer program using 32-byte descriptors
struct DualBufferProgram {
  uint32_t headPA;   // Physical address of first DUALBUFFER descriptor
  uint32_t tailPA;   // Physical address of last DUALBUFFER descriptor
  void *headVA;      // Virtual address of first descriptor
  void *tailVA;      // Virtual address of last descriptor
  uint8_t zHead;     // Z value for first descriptor block
  uint8_t descCount; // Number of DUALBUFFER descriptors
  bool valid;        // Program is valid and ready to use
};
} // namespace IRProgram