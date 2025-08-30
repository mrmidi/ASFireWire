#pragma once
//
// OHCIListOps.hpp
// Small helpers for CommandPtr/Branch/Z packing used by AT & AR builders/rings.
//
// Spec refs: OHCI 1.1 §3.1.2 (CommandPtr [31:4] addr, [3:0] Z),
//            AT §7.1.* (OUTPUT_* list rules), AR §8.1.* (INPUT_* list rules)
//

#include <stdint.h>

namespace OHCIListOps {

// Returns false if addr not 16B-aligned or Z invalid for the program.
bool EncodeCommandPtr(uint32_t dmaAddress, uint8_t zNibble,
                      uint32_t *outCmdPtr);

// Extractors
inline uint8_t ZFromCommandPtr(uint32_t cmdPtr) {
  return static_cast<uint8_t>(cmdPtr & 0xF);
}
inline uint32_t AddressFromCommandPtr(uint32_t cmdPtr) {
  return (cmdPtr & 0xFFFFFFF0u);
}

// Branch+Z field used in *LAST* descriptors (upper 28: branchAddress, lower 4:
// Z)
bool PackBranchAndZ(uint32_t branchAddress, uint8_t zNibble,
                    uint32_t *outQuadlet);
void UnpackBranchAndZ(uint32_t quadlet, uint32_t *branchAddress,
                      uint8_t *zNibble);

// Common completion word splitter used by both sides (status/timestamp quadlet)
// AT §7.1.5 / AR §8.1.5 describe where xferStatus/timeStamp land for their LAST
// descriptor.
void SplitStatusTimestamp(uint32_t word, uint16_t *xferStatus,
                          uint16_t *timeStamp);
} // namespace OHCIListOps
