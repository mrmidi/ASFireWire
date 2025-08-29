//
// ASOHCIITProgramBuilder.cpp
// Isochronous Transmit builder skeleton reusing AT descriptor pool
//
// Spec refs (OHCI 1.1): §9.1 (program layout), §9.4 (appending & tail patch), §9.6 (header quadlets)
//

#include "ASOHCIITProgramBuilder.hpp"
#include "ASOHCIATDescriptor.hpp"
#include "LogHelper.hpp"

#include <string.h>

void ASOHCIITProgramBuilder::Begin(ASOHCIATDescriptorPool& pool, uint32_t maxDescriptors)
{
    _pool = &pool;
    _blk = {};
    _descUsed = 0;
    _ip = ATIntPolicy::kErrorsOnly;

    // Reserve descriptor blocks for a single IT packet.
    // Z nibble encodes 2..8 blocks per packet (0=end) – allow up to 8.
    uint32_t reserve = (maxDescriptors == 0) ? 8 : maxDescriptors;
    if (reserve > 8) reserve = 8; // IT packet: header/immediate + payload frags + last (≤8)
    _blk = pool.AllocateBlock(reserve);
    if (!_blk.valid) {
        os_log(ASLog(), "ITBuilder: failed to allocate %u desc", reserve);
    } else {
        os_log(ASLog(), "ITBuilder: reserved %u desc (PA=0x%x Z=%u)", _blk.descriptorCount, _blk.physicalAddress, _blk.zValue);
    }
}

void ASOHCIITProgramBuilder::AddHeaderImmediate(ITSpeed spd,
                                                uint8_t tag,
                                                uint8_t channel,
                                                uint8_t sy,
                                                uint32_t dataLength,
                                                ATIntPolicy ip)
{
    if (!_pool || !_blk.valid) return;
    _ip = ip;
    if (_descUsed >= _blk.descriptorCount) return;

    // Encode OUTPUT_MORE_Immediate (first descriptor in IT packet) per §9.1/§9.6.
    // Quadlet layout (simplified / representative):
    //  quad0: [31:30]=speed, [29:28]=tag, [27:22]=channel, [21:16]=sy, [15:0]=dataLength bytes
    //  quad1: branchAddress (patched later if chaining) or 0
    //  quad2: interrupt control / misc policy (reuse AT int policy mapping)
    //  quad3: context/hardware reserved (0)
    ATDesc::Descriptor* d = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
    memset(d, 0, sizeof(*d));
    uint32_t q0 = 0;
    uint32_t speedBits = static_cast<uint32_t>(spd) & 0x3;      // assume 2 bits
    uint32_t tagBits   = static_cast<uint32_t>(tag) & 0x3;      // 2 bits per 1394 tag reuse (simplified)
    uint32_t chanBits  = static_cast<uint32_t>(channel) & 0x3F; // 6 bits channel
    uint32_t syBits    = static_cast<uint32_t>(sy) & 0x3F;      // 6 bits sy
    uint32_t lenBits   = dataLength & 0xFFFF;                   // 16-bit length field
    q0 |= speedBits << 30;
    q0 |= tagBits   << 28;
    q0 |= chanBits  << 22;
    q0 |= syBits    << 16;
    q0 |= lenBits;
    d->quad[0] = q0;
    d->quad[1] = 0; // branchAddress (will remain 0 until chaining / tail patch)
    d->quad[2] = static_cast<uint32_t>(_ip); // interrupt policy placeholder
    d->quad[3] = 0;
    _descUsed++;
}

void ASOHCIITProgramBuilder::AddPayloadFragment(uint32_t payloadPA, uint32_t payloadBytes)
{
    (void)payloadPA; (void)payloadBytes;
    if (!_pool || !_blk.valid) return;
    if (_descUsed < _blk.descriptorCount) {
        ATDesc::Descriptor* d = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
        memset(d, 0, sizeof(*d));
        _descUsed++;
    }
}

ITDesc::Program ASOHCIITProgramBuilder::Finalize()
{
    ITDesc::Program p{};
    if (!_blk.valid || _descUsed == 0) return p;
    // Ensure last descriptor is OUTPUT_LAST (or convert first if only one) by setting a marker.
    ATDesc::Descriptor* base = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress);
    ATDesc::Descriptor* last = base + (_descUsed - 1);
    // For now we simply set a high bit in quad2 to denote LAST (placeholder for true format bitfield per §9.1)
    last->quad[2] |= (1u << 31);

    // Provide a future-append patch point: quad1 (branchAddress) left 0; enqueue path will patch if chaining.
    p.headPA = _blk.physicalAddress;
    p.tailPA = _blk.physicalAddress + ((_descUsed - 1) * sizeof(ATDesc::Descriptor));
    p.zHead = _blk.zValue;
    p.descCount = _descUsed;

    // Release builder state
    _pool = nullptr;
    _blk = {};
    _descUsed = 0;
    _ip = ATIntPolicy::kErrorsOnly;
    return p;
}

void ASOHCIITProgramBuilder::Cancel()
{
    if (_pool && _blk.valid) {
        _pool->FreeBlock(_blk);
    }
    _pool = nullptr;
    _blk = {};
    _descUsed = 0;
    _ip = ATIntPolicy::kErrorsOnly;
}

