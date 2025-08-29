#pragma once
//
// ASOHCIIRProgramBuilder.hpp  
// Builds INPUT_MORE/INPUT_LAST and DUALBUFFER descriptor chains for IR receive modes.
//
// Spec refs (OHCI 1.1): §10.1 (IR DMA programs), §10.2 (receive modes), 
//   §10.6 (data formats), Table 10-1 (INPUT descriptors), Table 10-2 (DUALBUFFER)

#include <stdint.h>
#include "ASOHCIATDescriptorPool.hpp"  // reuse the existing pool
#include "ASOHCIIRDescriptor.hpp"
#include "ASOHCIIRTypes.hpp"

class ASOHCIIRProgramBuilder {
public:
    // Reserve descriptors for building an IR receive program
    void Begin(ASOHCIATDescriptorPool& pool, uint32_t maxDescriptors = 8);

    // Build Buffer-Fill mode program (§10.2.1)
    // Single large buffer, packets concatenated into stream
    kern_return_t BuildBufferFillProgram(uint32_t bufferPA,
                                        uint32_t bufferSize,
                                        const IRQueueOptions& opts,
                                        IRDesc::Program* outProgram);

    // Build Packet-per-Buffer mode program (§10.2.2)  
    // Each packet goes into separate buffer with INPUT_MORE/INPUT_LAST chain
    kern_return_t BuildPacketPerBufferProgram(const uint32_t bufferPAs[],
                                             const uint32_t bufferSizes[],
                                             uint32_t bufferCount,
                                             const IRQueueOptions& opts,
                                             IRDesc::Program* outProgram);

    // Build Dual-Buffer mode program (§10.2.3)
    // Split packet payload into two separate buffers using DUALBUFFER descriptors
    kern_return_t BuildDualBufferProgram(const IRDualBufferInfo& info,
                                        uint32_t descriptorCount,
                                        const IRQueueOptions& opts,
                                        IRProgram::DualBufferProgram* outProgram);

    // Abort build and return reserved descriptors to pool
    void Cancel();

private:
    ASOHCIATDescriptorPool* _pool = nullptr;
    ASOHCIATDescriptorPool::Block _blk{};
    uint32_t _descUsed = 0;
    
    // Helper methods for building individual descriptors
    
    // Build INPUT_MORE descriptor (§10.1, Table 10-1)
    void BuildInputMore(uint32_t bufferPA,
                       uint32_t reqCount,
                       IRIntPolicy intPolicy,
                       IRSyncMatch syncMatch,
                       bool includeHeader,
                       IRDesc::Descriptor* outDesc);
    
    // Build INPUT_LAST descriptor (§10.1, Table 10-1)  
    void BuildInputLast(uint32_t bufferPA,
                       uint32_t reqCount,
                       IRIntPolicy intPolicy,
                       IRSyncMatch syncMatch,
                       bool includeHeader,
                       bool includeTimestamp,
                       uint32_t branchAddress,
                       IRDesc::Descriptor* outDesc);
    
    // Build DUALBUFFER descriptor (§10.2.3, Table 10-2)
    void BuildDualBuffer(const IRDualBufferInfo& info,
                        IRIntPolicy intPolicy,
                        IRSyncMatch syncMatch,
                        uint32_t branchAddress,
                        uint8_t zValue,
                        IRDesc::DualBufferDescriptor* outDesc);
    
    // Link descriptors into a program chain
    void LinkDescriptors(IRDesc::Descriptor* descriptors, 
                        uint32_t count,
                        uint32_t basePA);
    
    void LinkDualBufferDescriptors(IRDesc::DualBufferDescriptor* descriptors,
                                  uint32_t count, 
                                  uint32_t basePA);
};