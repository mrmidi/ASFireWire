//
// ASOHCIARDescriptorRing.cpp
// ASOHCI
//
// Asynchronous Receive (AR) descriptor ring setup & management
//
// Spec refs (OHCI 1.1):
//  - §8.1  AR DMA context programs (INPUT_* descriptors, list rules)
//  - §3.1  Context registers (CommandPtr [31:4]=addr, [3:0]=Z; run/active/wake)
//  - §8.2  AR context registers (As Req/Rsp Rcv Context Base/Control/CommandPtr)
//  - §8.4  AR interrupts & completion (xferStatus/resCount semantics)
//

#include "ASOHCIARDescriptorRing.hpp"
#include "OHCIConstants.hpp"
#include "LogHelper.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryMap.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>

struct ASOHCIARDescriptorRing::Impl {
    IOPCIDevice*              pci = nullptr;  // retained
    uint32_t                  buffers = 0;
    uint32_t                  bufSize = 0;
    ARBufferFillMode          fill = ARBufferFillMode::kImmediate;

    // Descriptor chain (contiguous)
    IOBufferMemoryDescriptor* descsMD = nullptr;
    IOMemoryMap*              descsMap = nullptr;
    OHCI_ARInputMoreDescriptor* descsVA = nullptr;
    IODMACommand*             descsDMA = nullptr;
    uint64_t                  descsDMABase = 0; // 64-bit phys addr of desc[0]

    // Payload buffers (each a separate mapping/DMA command)
    IOBufferMemoryDescriptor** bufMD = nullptr;   // [buffers]
    IOMemoryMap**              bufMap = nullptr;  // [buffers]
    IODMACommand**             bufDMA = nullptr;  // [buffers]
    IOAddressSegment*          bufSeg = nullptr;  // [buffers]

    uint32_t                  consumeIdx = 0;
    bool                      armed = false;

    // Ready index single-slot (simple pop model)
    bool                      haveReady = false;
    uint32_t                  readyIndex = 0;
};

ASOHCIARDescriptorRing::~ASOHCIARDescriptorRing() { (void)Deallocate(); }

static inline uint32_t min_u32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

kern_return_t ASOHCIARDescriptorRing::Initialize(IOPCIDevice* pci,
                                                 uint32_t bufferCount,
                                                 uint32_t bufferBytes,
                                                 ARBufferFillMode fillMode)
{
    if (!pci || bufferCount < 2U) return kIOReturnBadArgument;
    if (bufferBytes < 512U || bufferBytes > (256U * 1024U) || (bufferBytes & 0x3U)) return kIOReturnBadArgument;

    if (_impl) (void)Deallocate();
    _impl = new Impl();
    if (!_impl) return kIOReturnNoMemory;

    _impl->pci = pci;
    _impl->pci->retain();
    _impl->buffers = bufferCount;
    _impl->bufSize = bufferBytes;
    _impl->fill    = fillMode;

    // Allocate payload buffers and DMA maps
    _impl->bufMD  = new IOBufferMemoryDescriptor*[_impl->buffers]();
    _impl->bufMap = new IOMemoryMap*[_impl->buffers]();
    _impl->bufDMA = new IODMACommand*[_impl->buffers]();
    _impl->bufSeg = new IOAddressSegment[_impl->buffers]();
    if (!_impl->bufMD || !_impl->bufMap || !_impl->bufDMA || !_impl->bufSeg) return kIOReturnNoMemory;

    kern_return_t kr = kIOReturnSuccess;
    for (uint32_t i = 0; i < _impl->buffers; ++i) {
        kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                              _impl->bufSize,
                                              4, /* quadlet alignment */
                                              &_impl->bufMD[i]);
        if (kr != kIOReturnSuccess || !_impl->bufMD[i]) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

        kr = _impl->bufMD[i]->CreateMapping(0, 0, 0, 0, 0, &_impl->bufMap[i]);
        if (kr != kIOReturnSuccess || !_impl->bufMap[i]) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

        IODMACommandSpecification spec{};
        spec.options        = kIODMACommandSpecificationNoOptions;
        spec.maxAddressBits = 32;
        kr = IODMACommand::Create(_impl->pci, kIODMACommandCreateNoOptions, &spec, &_impl->bufDMA[i]);
        if (kr != kIOReturnSuccess || !_impl->bufDMA[i]) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

        uint64_t flags = 0;
        uint32_t segCount = 1;
        IOAddressSegment segs[1] = {};
        kr = _impl->bufDMA[i]->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                             _impl->bufMD[i], 0, _impl->bufSize,
                                             &flags, &segCount, segs);
        if (kr != kIOReturnSuccess || segCount < 1 || segs[0].address == 0) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
        _impl->bufSeg[i] = segs[0];
    }

    // Descriptor chain
    const size_t chainBytes = (size_t)_impl->buffers * sizeof(OHCI_ARInputMoreDescriptor);
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, chainBytes, kOHCI_DescriptorAlign, &_impl->descsMD);
    if (kr != kIOReturnSuccess || !_impl->descsMD) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

    kr = _impl->descsMD->CreateMapping(0, 0, 0, 0, 0, &_impl->descsMap);
    if (kr != kIOReturnSuccess || !_impl->descsMap) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

    _impl->descsVA = (OHCI_ARInputMoreDescriptor*)_impl->descsMap->GetAddress();
    if (!_impl->descsVA) return kIOReturnNoMemory;
    memset(_impl->descsVA, 0, chainBytes);

    IODMACommandSpecification dspec{};
    dspec.options        = kIODMACommandSpecificationNoOptions;
    dspec.maxAddressBits = 32;
    kr = IODMACommand::Create(_impl->pci, kIODMACommandCreateNoOptions, &dspec, &_impl->descsDMA);
    if (kr != kIOReturnSuccess || !_impl->descsDMA) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;

    uint64_t dflags = 0;
    uint32_t dsegCount = 1;
    IOAddressSegment dsegs[1] = {};
    kr = _impl->descsDMA->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                        _impl->descsMD, 0, chainBytes,
                                        &dflags, &dsegCount, dsegs);
    if (kr != kIOReturnSuccess || dsegCount < 1 || dsegs[0].address == 0) return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    _impl->descsDMABase = dsegs[0].address;

    // Build chain: one INPUT_MORE per buffer, ring back to head
    for (uint32_t i = 0; i < _impl->buffers; ++i) {
        auto* d = &_impl->descsVA[i];
        memset(d, 0, sizeof(*d));
        d->cmd      = 0x2;             // INPUT_MORE (AR)
        d->key      = 0x0;             // must be 0 for AR
        d->i        = 0x1;             // interrupt on interesting events (§8.4)
        d->b        = 0x3;             // branch control '11' for INPUT_* (§8.1)
        d->reqCount = _impl->bufSize;
        d->dataAddress = (uint32_t)_impl->bufSeg[i].address; // 32-bit DMA

        const uint64_t next = (i + 1U < _impl->buffers)
                                ? (_impl->descsDMABase + (uint64_t)(i + 1U) * sizeof(*d))
                                : (_impl->descsDMABase); // ring
        d->branchAddress = (uint32_t)(next >> 4);  // [31:4]
        d->Z             = 1;                      // next block has 1 descriptor

        d->resCount   = _impl->bufSize;  // nothing written yet
        d->xferStatus = 0;
    }

    _impl->consumeIdx = 0;
    _impl->armed = true;
    _impl->haveReady = false;

    os_log(ASLog(), "ARring: init ok (%u buffers x %u bytes) DMA=0x%llx",
          _impl->buffers, _impl->bufSize, (unsigned long long)_impl->descsDMABase);
    return kIOReturnSuccess;
}

kern_return_t ASOHCIARDescriptorRing::Deallocate()
{
    if (!_impl) return kIOReturnSuccess;

    if (_impl->descsDMA) {
        _impl->descsDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        _impl->descsDMA->release();
        _impl->descsDMA = nullptr;
    }
    if (_impl->descsMap) { _impl->descsMap->release(); _impl->descsMap = nullptr; }
    if (_impl->descsMD)  { _impl->descsMD->release();  _impl->descsMD = nullptr; }
    _impl->descsVA = nullptr;
    _impl->descsDMABase = 0;

    if (_impl->bufDMA) {
        for (uint32_t i = 0; i < _impl->buffers; ++i) {
            if (_impl->bufDMA[i]) {
                _impl->bufDMA[i]->CompleteDMA(kIODMACommandCompleteDMANoOptions);
                _impl->bufDMA[i]->release();
            }
        }
        delete [] _impl->bufDMA; _impl->bufDMA = nullptr;
    }
    if (_impl->bufMap) {
        for (uint32_t i = 0; i < _impl->buffers; ++i) {
            if (_impl->bufMap[i]) _impl->bufMap[i]->release();
        }
        delete [] _impl->bufMap; _impl->bufMap = nullptr;
    }
    if (_impl->bufMD) {
        for (uint32_t i = 0; i < _impl->buffers; ++i) {
            if (_impl->bufMD[i]) _impl->bufMD[i]->release();
        }
        delete [] _impl->bufMD; _impl->bufMD = nullptr;
    }
    if (_impl->bufSeg) { delete [] _impl->bufSeg; _impl->bufSeg = nullptr; }

    if (_impl->pci) { _impl->pci->release(); _impl->pci = nullptr; }

    _impl->buffers = _impl->bufSize = 0;
    _impl->consumeIdx = 0;
    _impl->armed = false;

    delete _impl; _impl = nullptr;
    return kIOReturnSuccess;
}

kern_return_t ASOHCIARDescriptorRing::GetCommandPtrSeed(uint32_t* outDescriptorAddress,
                                                        uint8_t*  outZ) const
{
    if (!_impl || !_impl->armed || _impl->descsDMABase == 0) return kIOReturnNotReady;
    if (outDescriptorAddress) *outDescriptorAddress = (uint32_t)_impl->descsDMABase;
    if (outZ) *outZ = 1; // one descriptor in each block
    return kIOReturnSuccess;
}

bool ASOHCIARDescriptorRing::TryPopCompleted(ARPacketView* outView, uint32_t* outRingIndex)
{
    if (!_impl || !_impl->armed) return false;

    // If we have a ready cached, pop it
    if (!_impl->haveReady) {
        // scan once across buffers
        uint32_t idx = _impl->consumeIdx;
        for (uint32_t n = 0; n < _impl->buffers; ++n) {
            auto* d = &_impl->descsVA[idx];
            uint32_t req = d->reqCount; if (req == 0 || req > _impl->bufSize) req = _impl->bufSize;
            uint32_t res = d->resCount;
            if (res < req) { _impl->haveReady = true; _impl->readyIndex = idx; break; }
            idx = (idx + 1U) % _impl->buffers;
        }
        if (!_impl->haveReady) return false;
    }

    const uint32_t idx = _impl->readyIndex;
    if (outRingIndex) *outRingIndex = idx;
    if (outView) {
        auto* d = &_impl->descsVA[idx];
        uint32_t req = d->reqCount; if (req == 0 || req > _impl->bufSize) req = _impl->bufSize;
        uint32_t res = d->resCount;
        outView->data = (_impl->bufMap && _impl->bufMap[idx]) ? (const uint8_t*)(uintptr_t)_impl->bufMap[idx]->GetAddress() : nullptr;
        outView->length = (req >= res) ? (req - res) : 0U;
        outView->timeStamp = 0;   // optional: derive if needed per §8.1.5
        outView->xferStatus = (uint16_t)(d->xferStatus & 0xFFFF);
    }
    _impl->haveReady = false;
    return true;
}

kern_return_t ASOHCIARDescriptorRing::Recycle(uint32_t ringIndex)
{
    if (!_impl || !_impl->armed || ringIndex >= _impl->buffers) return kIOReturnBadArgument;
    auto* d = &_impl->descsVA[ringIndex];
    uint32_t req = d->reqCount; if (req == 0 || req > _impl->bufSize) req = _impl->bufSize;
    d->resCount = req;
    d->xferStatus = 0;
    if (_impl->consumeIdx == ringIndex) _impl->consumeIdx = (_impl->consumeIdx + 1U) % _impl->buffers;
    return kIOReturnSuccess;
}

kern_return_t ASOHCIARDescriptorRing::ReArmAfterBusReset()
{
    if (!_impl) return kIOReturnNotReady;
    for (uint32_t i = 0; i < _impl->buffers; ++i) {
        auto* d = &_impl->descsVA[i];
        uint32_t req = d->reqCount; if (req == 0 || req > _impl->bufSize) req = _impl->bufSize;
        d->resCount = req;
        d->xferStatus = 0;
    }
    _impl->consumeIdx = 0;
    _impl->haveReady = false;
    _impl->armed = true;
    return kIOReturnSuccess;
}

uint32_t ASOHCIARDescriptorRing::BufferCount() const { return _impl ? _impl->buffers : 0; }
uint32_t ASOHCIARDescriptorRing::BufferBytes() const { return _impl ? _impl->bufSize : 0; }
ARBufferFillMode ASOHCIARDescriptorRing::FillMode() const { return _impl ? _impl->fill : ARBufferFillMode::kImmediate; }
