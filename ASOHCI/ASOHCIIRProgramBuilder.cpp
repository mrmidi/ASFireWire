//
// ASOHCIIRProgramBuilder.cpp
// Implementation of IR receive program builders for three modes.
//
// Spec refs (OHCI 1.1): §10.1 (IR DMA programs), §10.2 (receive modes),
//   §10.6 (data formats), Table 10-1 (INPUT descriptors), Table 10-2 (DUALBUFFER)

#include "ASOHCIIRProgramBuilder.hpp"
#include "ASOHCIDescriptorUtils.hpp"
#include "ASOHCIMemoryBarrier.hpp"
#include "LogHelper.hpp"
#include <DriverKit/IOReturn.h>
#include <os/log.h>
#include <string.h>

void ASOHCIIRProgramBuilder::Begin(ASOHCIATDescriptorPool& pool, uint32_t maxDescriptors)
{
    _pool = &pool;
    _descUsed = 0;
    
    // Allocate descriptor block from pool
    _blk = pool.AllocateBlock(maxDescriptors);
    if (!_blk.virtualAddress || !_blk.physicalAddress) {
        os_log(ASLog(), "IRProgramBuilder: Failed to allocate descriptor block");
    }
}

kern_return_t ASOHCIIRProgramBuilder::BuildBufferFillProgram(uint32_t bufferPA,
                                                           uint32_t bufferSize,
                                                           const IRQueueOptions& opts,
                                                           IRDesc::Program* outProgram)
{
    if (!_pool || !_blk.virtualAddress || !outProgram) return kIOReturnBadArgument;
    if (_descUsed >= _blk.descriptorCount) return kIOReturnNoSpace;

    // Buffer-Fill mode uses single INPUT_LAST descriptor (§10.2.1)
    IRDesc::Descriptor* desc = &static_cast<IRDesc::Descriptor*>(_blk.virtualAddress)[_descUsed];
    
    BuildInputLast(bufferPA, 
                  bufferSize,
                  opts.interruptPolicy,
                  opts.syncMatch,
                  opts.includeHeader,
                  opts.includeTimestamp,
                  0, // No branch for single descriptor
                  desc);
    
    _descUsed++;
    
    // Memory barrier to ensure descriptor is written before hardware access
    OHCI_MEMORY_BARRIER();
    
    // Build program structure
    outProgram->headPA = _blk.physicalAddress;
    outProgram->tailPA = _blk.physicalAddress;
    outProgram->headVA = desc;
    outProgram->tailVA = desc;
    outProgram->zHead = 1; // Single descriptor
    outProgram->descCount = 1;
    
    os_log(ASLog(), "IRProgramBuilder: Built buffer-fill program - bufferPA=0x%x, size=%u", 
           bufferPA, bufferSize);
    
    return kIOReturnSuccess;
}

kern_return_t ASOHCIIRProgramBuilder::BuildPacketPerBufferProgram(const uint32_t bufferPAs[],
                                                                 const uint32_t bufferSizes[],
                                                                 uint32_t bufferCount,
                                                                 const IRQueueOptions& opts,
                                                                 IRDesc::Program* outProgram)
{
    if (!_pool || !_blk.virtualAddress || !outProgram) return kIOReturnBadArgument;
    if (bufferCount == 0 || bufferCount > _blk.descriptorCount) return kIOReturnBadArgument;
    if (_descUsed + bufferCount > _blk.descriptorCount) return kIOReturnNoSpace;

    IRDesc::Descriptor* descriptors = &static_cast<IRDesc::Descriptor*>(_blk.virtualAddress)[_descUsed];
    
    // Build INPUT_MORE descriptors for all but last buffer
    for (uint32_t i = 0; i < bufferCount - 1; i++) {
        BuildInputMore(bufferPAs[i],
                      bufferSizes[i],
                      IRIntPolicy::kNever, // Only interrupt on last
                      opts.syncMatch,
                      opts.includeHeader,
                      &descriptors[i]);
    }
    
    // Build INPUT_LAST descriptor for final buffer
    uint32_t lastIdx = bufferCount - 1;
    BuildInputLast(bufferPAs[lastIdx],
                  bufferSizes[lastIdx],
                  opts.interruptPolicy,
                  opts.syncMatch,
                  opts.includeHeader,
                  opts.includeTimestamp,
                  0, // No branch for final descriptor
                  &descriptors[lastIdx]);
    
    // Link descriptors into chain
    LinkDescriptors(descriptors, bufferCount, _blk.physicalAddress + (_descUsed * sizeof(IRDesc::Descriptor)));
    
    _descUsed += bufferCount;
    
    // Memory barrier to ensure all descriptors are written
    OHCI_MEMORY_BARRIER();
    
    // Build program structure
    outProgram->headPA = _blk.physicalAddress + (_descUsed - bufferCount) * sizeof(IRDesc::Descriptor);
    outProgram->tailPA = _blk.physicalAddress + (_descUsed - 1) * sizeof(IRDesc::Descriptor);
    outProgram->headVA = &descriptors[0];
    outProgram->tailVA = &descriptors[lastIdx];
    outProgram->zHead = static_cast<uint8_t>(bufferCount);
    outProgram->descCount = bufferCount;
    
    os_log(ASLog(), "IRProgramBuilder: Built packet-per-buffer program - buffers=%u", bufferCount);
    
    return kIOReturnSuccess;
}

kern_return_t ASOHCIIRProgramBuilder::BuildDualBufferProgram(const IRDualBufferInfo& info,
                                                           uint32_t descriptorCount,
                                                           const IRQueueOptions& opts,
                                                           IRProgram::DualBufferProgram* outProgram)
{
    if (!_pool || !_blk.virtualAddress || !outProgram) return kIOReturnBadArgument;
    if (descriptorCount == 0 || descriptorCount > (_blk.descriptorCount / 2)) return kIOReturnBadArgument;
    
    // DUALBUFFER descriptors are 32 bytes each (double size)
    uint32_t dualBufferSpace = descriptorCount * 2; // Each dual-buffer needs 2 regular descriptor slots
    if (_descUsed + dualBufferSpace > _blk.descriptorCount) return kIOReturnNoSpace;

    IRDesc::DualBufferDescriptor* dualDescriptors = 
        reinterpret_cast<IRDesc::DualBufferDescriptor*>(&static_cast<IRDesc::Descriptor*>(_blk.virtualAddress)[_descUsed]);
    
    // Build DUALBUFFER descriptors
    for (uint32_t i = 0; i < descriptorCount; i++) {
        uint32_t branchAddress = 0;
        uint8_t zValue = IRDescOps::kDualBuffer_End;
        
        // Link to next descriptor if not last
        if (i < descriptorCount - 1) {
            branchAddress = _blk.physicalAddress + (_descUsed + (i + 1) * 2) * sizeof(IRDesc::Descriptor);
            zValue = IRDescOps::kDualBuffer_Continue;
        }
        
        BuildDualBuffer(info,
                       (i == descriptorCount - 1) ? opts.interruptPolicy : IRIntPolicy::kNever,
                       opts.syncMatch,
                       branchAddress,
                       zValue,
                       &dualDescriptors[i]);
    }
    
    _descUsed += dualBufferSpace;
    
    // Memory barrier to ensure all descriptors are written
    OHCI_MEMORY_BARRIER();
    
    // Build dual-buffer program structure
    outProgram->headPA = _blk.physicalAddress + (_descUsed - dualBufferSpace) * sizeof(IRDesc::Descriptor);
    outProgram->tailPA = _blk.physicalAddress + (_descUsed - 2) * sizeof(IRDesc::Descriptor);
    outProgram->headVA = &dualDescriptors[0];
    outProgram->tailVA = &dualDescriptors[descriptorCount - 1];
    outProgram->zHead = static_cast<uint8_t>(descriptorCount);
    outProgram->descCount = static_cast<uint8_t>(descriptorCount);
    outProgram->valid = true;
    
    os_log(ASLog(), "IRProgramBuilder: Built dual-buffer program - descriptors=%u, firstSize=%u", 
           descriptorCount, info.firstSize);
    
    return kIOReturnSuccess;
}

void ASOHCIIRProgramBuilder::Cancel()
{
    if (_pool && _blk.virtualAddress) {
        _pool->FreeBlock(_blk);
        _blk = {};
        _descUsed = 0;
    }
}

void ASOHCIIRProgramBuilder::BuildInputMore(uint32_t bufferPA,
                                          uint32_t reqCount,
                                          IRIntPolicy intPolicy,
                                          IRSyncMatch syncMatch,
                                          bool includeHeader,
                                          IRDesc::Descriptor* outDesc)
{
    if (!outDesc) return;
    
    // Clear descriptor
    memset(outDesc, 0, sizeof(IRDesc::Descriptor));
    
    // Build quadlet 0: cmd | key | i | b | w fields (§10.1, Table 10-1)
    outDesc->quad[0] = (IRDescOps::kCmd_INPUT_MORE << 28) |
                       (IRDescOps::kKey_STANDARD << 24) |
                       (static_cast<uint32_t>(intPolicy) << 20) |
                       (IRDescOps::kBranch_Never << 16) |
                       (static_cast<uint32_t>(syncMatch) << 14) |
                       (reqCount & 0xFFFF);
    
    // Buffer address
    outDesc->quad[1] = bufferPA;
    
    // Branch address (not used for INPUT_MORE)
    outDesc->quad[2] = 0;
    
    // Status/timestamp (initialized by hardware)
    outDesc->quad[3] = 0;
}

void ASOHCIIRProgramBuilder::BuildInputLast(uint32_t bufferPA,
                                          uint32_t reqCount,
                                          IRIntPolicy intPolicy,
                                          IRSyncMatch syncMatch,
                                          bool includeHeader,
                                          bool includeTimestamp,
                                          uint32_t branchAddress,
                                          IRDesc::Descriptor* outDesc)
{
    if (!outDesc) return;
    
    // Clear descriptor
    memset(outDesc, 0, sizeof(IRDesc::Descriptor));
    
    // Build quadlet 0: cmd | key | i | b | w fields (§10.1, Table 10-1)
    outDesc->quad[0] = (IRDescOps::kCmd_INPUT_LAST << 28) |
                       (IRDescOps::kKey_STANDARD << 24) |
                       (static_cast<uint32_t>(intPolicy) << 20) |
                       (IRDescOps::kBranch_Always << 16) |
                       (static_cast<uint32_t>(syncMatch) << 14) |
                       (reqCount & 0xFFFF);
    
    // Buffer address
    outDesc->quad[1] = bufferPA;
    
    // Branch address
    outDesc->quad[2] = branchAddress;
    
    // Status/timestamp (initialized by hardware)
    outDesc->quad[3] = 0;
}

void ASOHCIIRProgramBuilder::BuildDualBuffer(const IRDualBufferInfo& info,
                                           IRIntPolicy intPolicy,
                                           IRSyncMatch syncMatch,
                                           uint32_t branchAddress,
                                           uint8_t zValue,
                                           IRDesc::DualBufferDescriptor* outDesc)
{
    if (!outDesc) return;
    
    // Clear descriptor
    memset(outDesc, 0, sizeof(IRDesc::DualBufferDescriptor));
    
    // Set control fields using helper method
    outDesc->SetControl(intPolicy == IRIntPolicy::kAlways,
                       static_cast<uint8_t>(intPolicy),
                       (zValue == IRDescOps::kDualBuffer_Continue) ? 1 : 0,
                       static_cast<uint8_t>(syncMatch),
                       info.firstSize);
    
    // Set buffer counts
    outDesc->SetCounts(info.firstReqCount, info.secondReqCount);
    
    // Set branch address and Z
    outDesc->SetBranchAndZ(branchAddress, zValue);
    
    // Initialize residual counts (hardware will update these)
    outDesc->InitializeResCounts(info.firstReqCount, info.secondReqCount);
    
    // Set buffer pointers
    outDesc->firstBuffer = info.firstBufferPA;
    outDesc->secondBuffer = info.secondBufferPA;
}

void ASOHCIIRProgramBuilder::LinkDescriptors(IRDesc::Descriptor* descriptors,
                                           uint32_t count,
                                           uint32_t basePA)
{
    // Standard INPUT descriptors don't use explicit linking like OUTPUT descriptors
    // The hardware processes them sequentially based on the branch control field
    // Only need to ensure proper branch control is set (done in BuildInputMore/BuildInputLast)
}

void ASOHCIIRProgramBuilder::LinkDualBufferDescriptors(IRDesc::DualBufferDescriptor* descriptors,
                                                      uint32_t count,
                                                      uint32_t basePA)
{
    // DUALBUFFER descriptors use explicit branch addresses set in BuildDualBuffer
    // No additional linking needed here
}