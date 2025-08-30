#pragma once
//
// ASOHCIITProgramBuilder.hpp
// Builds OUTPUT_MORE/OUTPUT_LAST* (and *_Immediate) chains for IT packets.
//
// Spec refs (OHCI 1.1): §9.1 (list building), §9.4 (appending), §9.6 (IT
// header/data format)

#include "ASOHCIATDescriptorPool.hpp" // reuse the existing pool
#include "ASOHCIITDescriptor.hpp"
#include "ASOHCIITTypes.hpp"
#include <stdint.h>

// Descriptor opcode/key constants for IT reuse (mirrors AT forms but separated
// for clarity)
namespace ITDescOps {
static constexpr uint32_t kCmd_OUTPUT_MORE = 0x0;           // cmd=0,key=0
static constexpr uint32_t kCmd_OUTPUT_LAST = 0x1;           // cmd=1,key=0
static constexpr uint32_t kCmd_OUTPUT_MORE_IMMEDIATE = 0x0; // cmd=0,key=2
static constexpr uint32_t kCmd_OUTPUT_LAST_IMMEDIATE = 0x1; // cmd=1,key=2
static constexpr uint32_t kKey_STANDARD = 0x0;
static constexpr uint32_t kKey_IMMEDIATE = 0x2;
} // namespace ITDescOps

class ASOHCIITProgramBuilder {
public:
  // Reserve up to 'maxDescriptors' (header/immediate + payload frags + last),
  // max 8 (Z range 2..8) (§9.1)
  void Begin(ASOHCIATDescriptorPool &pool, uint32_t maxDescriptors = 7);

  // Build the IT immediate header (controller emits the wire header from these
  // fields) (§9.6) dataLength = payload bytes for this packet; controller pads
  // to quadlet if needed.
  void AddHeaderImmediate(ITSpeed spd, uint8_t tag, uint8_t channel, uint8_t sy,
                          uint32_t dataLength,
                          ITIntPolicy ip = ITIntPolicy::kNever,
                          bool isLast = false);

  // Append a payload fragment by physical address (§9.1)
  void AddPayloadFragment(uint32_t payloadPA, uint32_t payloadBytes,
                          bool isLast = false);

  // Close the packet with OUTPUT_LAST*; returns a ready-to-enqueue program
  // (§9.1)
  ITDesc::Program Finalize();

  // Abort build and return reserved descriptors to pool
  void Cancel();

private:
  ASOHCIATDescriptorPool *_pool = nullptr;
  ASOHCIATDescriptorPool::Block _blk{};
  uint32_t _descUsed = 0;
  ITIntPolicy _ip = ITIntPolicy::kNever;
  uint32_t _headerQuadlets =
      0; // number of header quadlets used in immediate descriptor
};
