// OHCIListOps.cpp
#include "OHCIListOps.hpp"
#include <string.h>

namespace OHCIListOps {

bool EncodeCommandPtr(uint32_t dmaAddress, uint8_t zNibble,
                      uint32_t *outCmdPtr) {
  if (!outCmdPtr)
    return false;
  if ((dmaAddress & 0xF) != 0)
    return false; // 16B alignment (§3.1.2)
  // Valid Z: 0=end, or 2..8 for a descriptor block used as a packet (§9.1
  // mirrors AT rules)
  if (!(zNibble == 0 || (zNibble >= 2 && zNibble <= 8)))
    return false;
  *outCmdPtr = (dmaAddress & 0xFFFFFFF0u) | (zNibble & 0x0F);
  return true;
}

bool PackBranchAndZ(uint32_t branchAddress, uint8_t zNibble,
                    uint32_t *outQuadlet) {
  if (!outQuadlet)
    return false;
  if ((branchAddress & 0xF) != 0)
    return false;
  if (!(zNibble == 0 || (zNibble >= 2 && zNibble <= 8)))
    return false;
  *outQuadlet = (branchAddress & 0xFFFFFFF0u) | (zNibble & 0x0F);
  return true;
}

void UnpackBranchAndZ(uint32_t quadlet, uint32_t *branchAddress,
                      uint8_t *zNibble) {
  if (branchAddress)
    *branchAddress = (quadlet & 0xFFFFFFF0u);
  if (zNibble)
    *zNibble = static_cast<uint8_t>(quadlet & 0x0F);
}

void SplitStatusTimestamp(uint32_t word, uint16_t *xferStatus,
                          uint16_t *timeStamp) {
  // Matches the “status/timestamp” quadlet layout for LAST descriptors.
  if (timeStamp)
    *timeStamp = static_cast<uint16_t>((word >> 16) & 0xFFFF);
  if (xferStatus)
    *xferStatus = static_cast<uint16_t>(word & 0xFFFF);
}

} // namespace OHCIListOps