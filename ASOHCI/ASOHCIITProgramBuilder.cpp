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
    _headerQuadlets = 0;

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

    // Encode OUTPUT_MORE_Immediate descriptor (cmd=0,key=2) followed by header quadlets.
    // Immediate format (reuse AT immediate layout): first 2 or 4 quadlets of header after control quadlets.
    // We support 2 quadlet header for basic isoch (Header, HeaderCRC/optional) – for now always emit 2.
    ATDesc::Descriptor* d = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
    memset(d, 0, sizeof(*d));
    // Control quadlet 0: cmd/key + interrupt policy bits approximated via _ip (reuse AT semantics of 'i').
    // For now: bits 3:0 cmd, 6:4 key, bits 9:8 interrupt policy (reuse mapping), others 0.
    uint32_t ctrl0 = 0;
    ctrl0 |= ITDescOps::kCmd_OUTPUT_MORE;             // cmd=0
    ctrl0 |= (ITDescOps::kKey_IMMEDIATE << 4);        // key=2 at bits 6:4
    ctrl0 |= (static_cast<uint32_t>(_ip) & 0x3) << 8; // simplistic mapping of interrupt policy
    d->quad[0] = ctrl0;
    // Control quadlet 1: reserved for branchAddress and Z when converted to *_LAST during append.
    d->quad[1] = 0; // will be patched if chaining (OUTPUT_LAST_IMMEDIATE)
    // Header quadlets (2). Layout (quad2): [31:30] speed, [29:28] tag, [27:22] channel, [21:16] sy, [15:0] length
    uint32_t hdr0 = ((static_cast<uint32_t>(spd) & 0x3) << 30) |
                    ((static_cast<uint32_t>(tag) & 0x3) << 28) |
                    ((static_cast<uint32_t>(channel) & 0x3F) << 22) |
                    ((static_cast<uint32_t>(sy) & 0x3F) << 16) |
                    (dataLength & 0xFFFF);
    d->quad[2] = hdr0;
    d->quad[3] = 0; // second header quadlet placeholder (could hold extended info/FDF/etc.)
    _headerQuadlets = 2;
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
    // Convert final descriptor from OUTPUT_MORE* to OUTPUT_LAST* if needed (cmd bit 0->1).
    ATDesc::Descriptor* base = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress);
    ATDesc::Descriptor* last = base + (_descUsed - 1);
    // Only if header immediate (key=2) we toggle cmd in quad0 bit0.
    last->quad[0] = (last->quad[0] & ~0xF) | ITDescOps::kCmd_OUTPUT_LAST; // preserve key/interrupt bits
    // Write reqCount (bytes of header immediate data) in high halfword of control quadlet? (Reuse OUTPUT_LAST-Immediate rule: 8,12,16)
    // Here we emitted 2 header quadlets (8 bytes) => reqCount=8.
    // Map: reqCount stored in bits 31:16 of quad0 for AT; replicate semantics.
    last->quad[0] &= 0x0000FFFF; // clear prior count
    last->quad[0] |= (8u << 16); // 2 header quadlets

    // Provide a future-append patch point: quad1 (branchAddress) left 0; enqueue path will patch if chaining.
    p.headPA = _blk.physicalAddress;
    p.tailPA = _blk.physicalAddress + ((_descUsed - 1) * sizeof(ATDesc::Descriptor));
    p.zHead = _blk.zValue;
    p.descCount = _descUsed;
    p.headVA = _blk.virtualAddress;
    p.tailVA = static_cast<uint8_t*>(_blk.virtualAddress) + ((_descUsed - 1) * sizeof(ATDesc::Descriptor));

    // Release builder state
    _pool = nullptr;
    _blk = {};
    _descUsed = 0;
    _ip = ATIntPolicy::kErrorsOnly;
    _headerQuadlets = 0;
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
    _headerQuadlets = 0;
}

