// #pragma once
//
//  ASOHCIATProgramBuilder.hpp
//  Builds a single AT packet program: OUTPUT_MORE* ... OUTPUT_LAST*
//
//  Spec refs: OHCI 1.1 §7.7 (Descriptor fields), §7.5 (interrupt policy ‘i’
//  bits)

#pragma once

#include "ASOHCIATDescriptor.hpp"
#include "ASOHCIATDescriptorPool.hpp"
#include "ASOHCIATTypes.hpp"
#include <stdint.h>

// OHCI §7.1 Descriptor Field Masks and Positions
namespace ATDescField {
// First quadlet common fields (cmd, key, branch control, reqCount)
constexpr uint32_t kCmdMask = 0xF0000000; // Bits [31:28]
constexpr uint32_t kCmdShift = 28;
constexpr uint32_t kKeyMask = 0x0E000000; // Bits [27:25]
constexpr uint32_t kKeyShift = 25;
constexpr uint32_t kBranchMask = 0x01800000; // Bits [24:23]
constexpr uint32_t kBranchShift = 23;
constexpr uint32_t kReqCountMask = 0x0000FFFF; // Bits [15:0]
constexpr uint32_t kReqCountShift = 0;

// OUTPUT_LAST specific fields (ping, interrupt)
constexpr uint32_t kPingMask = 0x00400000; // Bit [22]
constexpr uint32_t kPingShift = 22;
constexpr uint32_t kInterruptMask = 0x00600000; // Bits [22:21]
constexpr uint32_t kInterruptShift = 21;

// Branch address and Z nibble (third quadlet)
constexpr uint32_t kBranchAddrMask = 0xFFFFFFF0; // Bits [31:4]
constexpr uint32_t kBranchAddrShift = 4;
constexpr uint32_t kZNibbleMask = 0x0000000F; // Bits [3:0]
constexpr uint32_t kZNibbleShift = 0;

// Command values
constexpr uint32_t kCmdOutputMore = 0x0;
constexpr uint32_t kCmdOutputLast = 0x1;

// Key values
constexpr uint32_t kKeyNonImmediate = 0x0;
constexpr uint32_t kKeyImmediate = 0x2;

// Branch control values
constexpr uint32_t kBranchNone = 0x0;     // 2'b00
constexpr uint32_t kBranchRequired = 0x3; // 2'b11

// Interrupt control values
constexpr uint32_t kInterruptNever = 0x0;  // 2'b00
constexpr uint32_t kInterruptError = 0x1;  // 2'b01
constexpr uint32_t kInterruptAlways = 0x3; // 2'b11
} // namespace ATDescField

class ASOHCIATProgramBuilder {
public:
  ASOHCIATProgramBuilder() = default;
  ~ASOHCIATProgramBuilder() = default;

  // Required for OSSharedPtr compatibility
  void release() { delete this; }
  // Safe field encoding helpers
  static inline uint32_t EncodeCmd(uint32_t cmd) {
    return (cmd << ATDescField::kCmdShift) & ATDescField::kCmdMask;
  }

  static inline uint32_t EncodeKey(uint32_t key) {
    return (key << ATDescField::kKeyShift) & ATDescField::kKeyMask;
  }

  static inline uint32_t EncodeBranch(uint32_t branch) {
    return (branch << ATDescField::kBranchShift) & ATDescField::kBranchMask;
  }

  static inline uint32_t EncodeReqCount(uint32_t count) {
    return (count << ATDescField::kReqCountShift) & ATDescField::kReqCountMask;
  }

  static inline uint32_t EncodeInterrupt(uint32_t interrupt) {
    return (interrupt << ATDescField::kInterruptShift) &
           ATDescField::kInterruptMask;
  }

  static inline uint32_t EncodeBranchAddr(uint32_t addr, uint32_t z) {
    return ((addr << ATDescField::kBranchAddrShift) &
            ATDescField::kBranchAddrMask) |
           ((z << ATDescField::kZNibbleShift) & ATDescField::kZNibbleMask);
  }

public:
public:
  // Resets internal state and reserves space for the worst-case descriptor
  // count (header immediate + N payload fragments + last). Pool provides
  // aligned memory; we return an ATDesc::Program on Finalize().
  virtual void Begin(ASOHCIATDescriptorPool &pool, uint32_t maxDescriptors);

  // Attach a 1394 header as IMMEDIATE quadlets (8/12/16 bytes per §7.7)
  virtual void AddHeaderImmediate(const uint32_t *header, uint32_t headerBytes,
                                  ATIntPolicy ip);

  // Append a payload fragment. The builder decides OUTPUT_MORE vs LAST
  // placement.
  virtual void AddPayloadFragment(uint32_t payloadPA, uint32_t payloadBytes);

  // Finalize: place OUTPUT_LAST* (IMMEDIATE or not depending on header choice).
  virtual ATDesc::Program Finalize();

  // Cancel (release reserved space) — no-op stub in header
  virtual void Cancel();

private:
  // Opaque working area — only declared here
  ASOHCIATDescriptorPool *_pool = nullptr;
  ASOHCIATDescriptorPool::Block _blk{};
  uint32_t _descUsed = 0;
  ATIntPolicy _ip = ATIntPolicy::kInterestingOnly;
};
