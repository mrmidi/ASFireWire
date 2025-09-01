//
// ASOHCIITProgramBuilder.cpp
// Isochronous Transmit builder skeleton reusing AT descriptor pool
//
// Spec refs (OHCI 1.1): §9.1 (program layout), §9.4 (appending & tail patch),
// §9.6 (header quadlets)
//

#include "ASOHCIITProgramBuilder.hpp"
#include "ASOHCIATDescriptor.hpp"
#include "ASOHCIDescriptorUtils.hpp"
#include "LogHelper.hpp"

#include <string.h>

// IT interrupt policy mapping
static inline uint32_t mapInterruptPolicy(ITIntPolicy p) {
  return (p == ITIntPolicy::kAlways) ? 0x3 : 0x0;
}

void ASOHCIITProgramBuilder::Begin(ASOHCIATDescriptorPool &pool,
                                   uint32_t maxDescriptors) {
  _pool = &pool;
  _blk = {};
  _descUsed = 0;
  _ip = ITIntPolicy::kNever;
  _headerQuadlets = 0;

  uint32_t reserve = (maxDescriptors == 0) ? 8 : maxDescriptors;
  if (reserve > 8)
    reserve = 8;
  if (reserve < 2)
    reserve = 2;

  _blk = pool.AllocateBlock(reserve);
  if (!_blk.valid) {
    os_log(ASLog(), "ITBuilder: failed to allocate %u desc", reserve);
  } else {
    os_log(ASLog(), "ITBuilder: reserved %u desc (PA=0x%{public}x Z=%u)",
           _blk.descriptorCount, _blk.physicalAddress, _blk.zValue);
  }
}

void ASOHCIITProgramBuilder::AddHeaderImmediate(ITSpeed spd, uint8_t tag,
                                                uint8_t channel, uint8_t sy,
                                                uint32_t dataLength,
                                                ITIntPolicy ip, bool isLast) {
  if (!_pool || !_blk.valid)
    return;
  if (_descUsed + 2 > _blk.descriptorCount)
    return;

  _ip = ip;

  ATDesc::Descriptor *im0 =
      static_cast<ATDesc::Descriptor *>(_blk.virtualAddress) + _descUsed;
  ATDesc::Descriptor *im1 = im0 + 1;
  memset(im0, 0, sizeof(*im0));
  memset(im1, 0, sizeof(*im1));

  uint32_t cmd =
      isLast ? ITDescOps::kCmd_OUTPUT_LAST : ITDescOps::kCmd_OUTPUT_MORE;
  uint32_t b = isLast ? 0x3 : 0x0;
  uint32_t i = mapInterruptPolicy(ip);
  uint32_t reqCount = 8; // 2 quadlets header

  uint32_t ctrl0 = (cmd & 0xF) | ((ITDescOps::kKey_IMMEDIATE & 0x7) << 4) |
                   ((i & 0x3) << 8) | ((b & 0x3) << 10) |
                   ((reqCount & 0xFFFF) << 16);
  im0->quad[0] = ctrl0;
  im0->quad[1] = 0; // Reserved for IT

  uint32_t hdr0 = ((uint32_t)sy & 0xF) << 12 | (0xA << 8) |
                  ((uint32_t)channel & 0x3F) << 2 | ((uint32_t)tag & 0x3);
  uint32_t hdr1 = ((uint32_t)spd & 0x7) << 29 | (dataLength & 0xFFFF);

  im1->quad[0] = 0; // skipAddress + Z
  im1->quad[1] = hdr0;
  im1->quad[2] = hdr1;
  im1->quad[3] = 0; // Reserved

  _headerQuadlets = 2;
  _descUsed += 2;
}

void ASOHCIITProgramBuilder::AddPayloadFragment(uint32_t payloadPA,
                                                uint32_t payloadBytes,
                                                bool isLast) {
  if (!_pool || !_blk.valid)
    return;
  if (_descUsed >= _blk.descriptorCount)
    return;

  ATDesc::Descriptor *d =
      static_cast<ATDesc::Descriptor *>(_blk.virtualAddress) + _descUsed;
  memset(d, 0, sizeof(*d));

  uint32_t cmd =
      isLast ? ITDescOps::kCmd_OUTPUT_LAST : ITDescOps::kCmd_OUTPUT_MORE;
  uint32_t b = isLast ? 0x3 : 0x0;
  uint32_t i = isLast ? mapInterruptPolicy(_ip) : 0;

  uint32_t ctrl0 = (cmd & 0xF) | ((b & 0x3) << 10) | ((i & 0x3) << 8) |
                   ((payloadBytes & 0xFFFF) << 16);
  d->quad[0] = ctrl0;
  d->quad[1] = payloadPA;
  d->quad[2] = 0;
  d->quad[3] = 0;

  _descUsed++;
}

ITDesc::Program ASOHCIITProgramBuilder::Finalize() {
  ITDesc::Program p{};
  if (!_blk.valid || _descUsed == 0)
    return p;

  p.headPA = _blk.physicalAddress;
  p.descCount = _descUsed;
  p.zHead = _descUsed <= 8 ? _descUsed : 8;
  p.headVA = _blk.virtualAddress;

  ATDesc::Descriptor *lastDesc =
      static_cast<ATDesc::Descriptor *>(_blk.virtualAddress) + (_descUsed - 1);
  if (DescGetKey(lastDesc->quad[0]) == ITDescOps::kKey_IMMEDIATE) {
    p.tailPA =
        _blk.physicalAddress + ((_descUsed - 2) * sizeof(ATDesc::Descriptor));
    p.tailVA = static_cast<uint8_t *>(_blk.virtualAddress) +
               ((_descUsed - 2) * sizeof(ATDesc::Descriptor));

    ATDesc::Descriptor *im1 =
        static_cast<ATDesc::Descriptor *>(_blk.virtualAddress) +
        (_descUsed - 1);
    im1->quad[0] = (p.zHead & 0xF); // skipAddress is 0, Z is total blocks
  } else {
    p.tailPA =
        _blk.physicalAddress + ((_descUsed - 1) * sizeof(ATDesc::Descriptor));
    p.tailVA = static_cast<uint8_t *>(_blk.virtualAddress) +
               ((_descUsed - 1) * sizeof(ATDesc::Descriptor));
  }

  // Release builder state
  _pool = nullptr;
  _blk = {};
  _descUsed = 0;
  _ip = ITIntPolicy::kNever;
  _headerQuadlets = 0;
  return p;
}

void ASOHCIITProgramBuilder::Cancel() {
  if (_pool && _blk.valid) {
    _pool->FreeBlock(_blk);
  }
  _pool = nullptr;
  _blk = {};
  _descUsed = 0;
  _ip = ITIntPolicy::kNever;
  _headerQuadlets = 0;
}