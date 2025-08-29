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
    (void)spd; (void)tag; (void)channel; (void)sy; (void)dataLength;
    if (!_pool || !_blk.valid) return;
    _ip = ip;
    // Stub: don’t encode for now; just consume a slot so Finalize can run later
    if (_descUsed < _blk.descriptorCount) {
        ATDesc::Descriptor* d = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
        memset(d, 0, sizeof(*d));
        _descUsed++;
    }
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

    // Minimal: expose head, Z from pool; leave tail equal to last descriptor position
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

