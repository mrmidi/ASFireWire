#pragma once
//
// ASOHCIDescriptorUtils.hpp
// Shared descriptor field extraction utilities for AT/AR/IT contexts
//
// Spec refs (OHCI 1.1): §7.1, §8.1, §9.1 (descriptor formats)
//   All context types use same cmd/key/i/b field layout in quadlet 0

#include <stdint.h>

// Descriptor quadlet 0 field extraction (OHCI common format)
// Bits [3:0]   = cmd (command opcode)
// Bits [6:4]   = key (descriptor variant)
// Bits [9:8]   = i (interrupt policy)
// Bits [11:10] = b (branch control)
// Bits [31:16] = reqCount (or other fields)

static inline uint32_t DescGetCmd(uint32_t q0) { return q0 & 0xF; }
static inline uint32_t DescGetKey(uint32_t q0) { return (q0 >> 4) & 0x7; }
static inline uint32_t DescGetInterrupt(uint32_t q0) { return (q0 >> 8) & 0x3; }
static inline uint32_t DescGetBranch(uint32_t q0) { return (q0 >> 10) & 0x3; }
static inline uint32_t DescGetReqCount(uint32_t q0) {
  return (q0 >> 16) & 0xFFFF;
}

// Descriptor quadlet 0 field setting
static inline uint32_t DescSetCmd(uint32_t q0, uint32_t cmd) {
  return (q0 & ~0xF) | (cmd & 0xF);
}
static inline uint32_t DescSetKey(uint32_t q0, uint32_t key) {
  return (q0 & ~0x70) | ((key & 0x7) << 4);
}
static inline uint32_t DescSetInterrupt(uint32_t q0, uint32_t i) {
  return (q0 & ~0x300) | ((i & 0x3) << 8);
}
static inline uint32_t DescSetBranch(uint32_t q0, uint32_t b) {
  return (q0 & ~0xC00) | ((b & 0x3) << 10);
}
static inline uint32_t DescSetReqCount(uint32_t q0, uint32_t reqCount) {
  return (q0 & ~0xFFFF0000) | ((reqCount & 0xFFFF) << 16);
}