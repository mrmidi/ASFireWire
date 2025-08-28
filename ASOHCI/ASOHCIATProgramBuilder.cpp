//
// ASOHCIATProgramBuilder.cpp  
// ASOHCI
//
// OHCI 1.1 AT Program Builder Implementation
// Based on OHCI 1.1 Specification §7.1 (AT DMA Context Programs), §7.7 (Data formats)
//

#include "ASOHCIATProgramBuilder.hpp"
#include "LogHelper.hpp"

void ASOHCIATProgramBuilder::Begin(ASOHCIATDescriptorPool& pool, uint32_t maxDescriptors)
{
    // Reset builder state for new program
    _pool = &pool;
    _descUsed = 0;
    _ip = ATIntPolicy::kInterestingOnly;
    
    // Reserve descriptor block from pool  
    // Per OHCI §7.1: Maximum 7 fragments per packet (1 header + 5 payload + 1 last)
    uint32_t reserveCount = (maxDescriptors == 0) ? 7 : maxDescriptors;
    if (reserveCount > 7) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Invalid maxDescriptors %u (max 7)", maxDescriptors);
        reserveCount = 7;
    }
    
    _blk = pool.AllocateBlock(reserveCount);
    if (!_blk.valid) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Failed to allocate %u descriptors from pool", reserveCount);
        return;
    }
    
    os_log(ASLog(), "ASOHCIATProgramBuilder: Reserved %u descriptors (PA=0x%x, Z=%u)", 
                reserveCount, _blk.physicalAddress, _blk.zValue);
}

void ASOHCIATProgramBuilder::AddHeaderImmediate(const uint32_t* header, uint32_t headerBytes, ATIntPolicy ip)
{
    if (!_pool || !_blk.valid || !header) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Invalid state for header immediate");
        return;
    }
    
    // Per OHCI §7.1: First descriptor must be *-Immediate with full 1394 packet header
    // Header size must be 8, 12, or 16 bytes (2, 3, or 4 quadlets)
    if (headerBytes != 8 && headerBytes != 12 && headerBytes != 16) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Invalid header size %u (must be 8, 12, or 16)", headerBytes);
        return;
    }
    
    if (_descUsed >= _blk.descriptorCount) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: No space for header descriptor");
        return;
    }
    
    // Store interrupt policy for final descriptor
    _ip = ip;
    
    // Get pointer to current descriptor in pool
    ATDesc::Descriptor* desc = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
    
    // Clear descriptor memory
    memset(desc, 0, sizeof(ATDesc::Descriptor));
    
    // Build OUTPUT_MORE_Immediate descriptor per OHCI §7.1.2
    // Use safe field encoders to prevent corruption
    desc->quad[0] = EncodeCmd(ATDescField::kCmdOutputMore) |
                   EncodeKey(ATDescField::kKeyImmediate) |
                   EncodeBranch(ATDescField::kBranchNone) |
                   EncodeReqCount(headerBytes);
    
    // Second quadlet: timeStamp (AT response only) - set to 0 for requests
    desc->quad[1] = 0;
    
    // Copy header quadlets (up to 4 quadlets = 16 bytes)
    uint32_t quadlets = headerBytes / 4;
    for (uint32_t i = 0; i < quadlets && i < 4; i++) {
        desc->quad[2 + i] = header[i];
    }
    
    _descUsed++; // OUTPUT_MORE-Immediate counts as 1 descriptor in our allocation
    
    os_log(ASLog(), "ASOHCIATProgramBuilder: Added header immediate (%u bytes, %u quadlets)", 
                headerBytes, quadlets);
}

void ASOHCIATProgramBuilder::AddPayloadFragment(uint32_t payloadPA, uint32_t payloadBytes)
{
    if (!_pool || !_blk.valid) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Invalid state for payload fragment");
        return;
    }
    
    if (payloadBytes == 0) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Skipping zero-length payload fragment");
        return;
    }
    
    if (_descUsed >= _blk.descriptorCount) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: No space for payload descriptor");
        return;
    }
    
    // Get pointer to current descriptor in pool
    ATDesc::Descriptor* desc = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + _descUsed;
    
    // Clear descriptor memory  
    memset(desc, 0, sizeof(ATDesc::Descriptor));
    
    // Build OUTPUT_MORE descriptor per OHCI §7.1.1
    // Use safe field encoders to prevent corruption
    desc->quad[0] = EncodeCmd(ATDescField::kCmdOutputMore) |
                   EncodeKey(ATDescField::kKeyNonImmediate) |
                   EncodeBranch(ATDescField::kBranchNone) |
                   EncodeReqCount(payloadBytes);
    
    // Second quadlet: dataAddress = physical address of payload
    desc->quad[1] = payloadPA;
    
    // Third and fourth quadlets are unused for OUTPUT_MORE
    desc->quad[2] = 0;
    desc->quad[3] = 0;
    
    _descUsed++;
    
    os_log(ASLog(), "ASOHCIATProgramBuilder: Added payload fragment (PA=0x%x, %u bytes)", 
                payloadPA, payloadBytes);
}

ATDesc::Program ASOHCIATProgramBuilder::Finalize()
{
    ATDesc::Program program = {}; // Initialize with zeros
    
    if (!_pool || !_blk.valid || _descUsed == 0) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Cannot finalize - invalid state or no descriptors");
        return program;
    }
    
    // Convert last OUTPUT_MORE to OUTPUT_LAST
    // Per OHCI §7.1: Each packet ends with OUTPUT_LAST* descriptor
    ATDesc::Descriptor* lastDesc = static_cast<ATDesc::Descriptor*>(_blk.virtualAddress) + (_descUsed - 1);
    
    // Check if last descriptor was OUTPUT_MORE-Immediate (key=2) or OUTPUT_MORE (key=0)
    uint32_t key = (lastDesc->quad[0] >> 25) & 0x7;
    bool isImmediate = (key == 0x2);
    
    // Convert to OUTPUT_LAST by changing cmd from 0 to 1 using safe mask
    lastDesc->quad[0] = (lastDesc->quad[0] & ~ATDescField::kCmdMask) | EncodeCmd(ATDescField::kCmdOutputLast);
    
    // Add required OUTPUT_LAST fields per OHCI §7.1.3 and §7.1.4
    if (isImmediate) {
        // OUTPUT_LAST-Immediate: Add branch control, Z value, interrupt control
        // Modify first quadlet to add interrupt control and branch control
        uint32_t interruptBits = 0x1; // default: interesting only (2'b01)
        switch (_ip) {
            case ATIntPolicy::kAlways:          interruptBits = 0x3; break; // 2'b11
            case ATIntPolicy::kInterestingOnly: interruptBits = 0x1; break; // 2'b01
        }
        
        lastDesc->quad[0] |= EncodeInterrupt(interruptBits) |  // i field [22:21]
                            EncodeBranch(ATDescField::kBranchRequired);    // b field = 2'b11
        
        // Add branchAddress and Z in appropriate quadlet  
        // For OUTPUT_LAST-Immediate, branchAddress+Z is in third quadlet (index 2)
        lastDesc->quad[2] = (lastDesc->quad[2] & 0xFFFF0000) |  // Preserve upper 16 bits (xferStatus)
                           EncodeBranchAddr(0, 0);              // branchAddress=0, Z=0 (end of list)
        
    } else {
        // OUTPUT_LAST: Add all required fields using safe encoders
        lastDesc->quad[0] |= EncodeBranch(ATDescField::kBranchRequired);  // b field = 2'b11
        
        // Set interrupt control based on policy
        uint32_t interruptBits = 0x1; // default: interesting only
        switch (_ip) {
            case ATIntPolicy::kAlways:          interruptBits = 0x3; break;
            case ATIntPolicy::kInterestingOnly: interruptBits = 0x1; break;
        }
        lastDesc->quad[0] |= EncodeInterrupt(interruptBits);
        
        // Third quadlet: branchAddress (upper 28 bits) + Z (lower 4 bits)
        lastDesc->quad[2] = EncodeBranchAddr(0, 0);  // branchAddress=0, Z=0 (end of list)
        
        // Fourth quadlet: xferStatus (upper 16) + timeStamp (lower 16) - will be filled by hardware
        lastDesc->quad[3] = 0;
    }
    
    // Build program descriptor
    program.headPA = _blk.physicalAddress;
    program.tailPA = _blk.physicalAddress + ((_descUsed - 1) * sizeof(ATDesc::Descriptor));
    program.zHead = _blk.zValue;
    program.descCount = _descUsed;
    
    os_log(ASLog(), "ASOHCIATProgramBuilder: Finalized program with %u descriptors (head=0x%x, tail=0x%x, Z=%u)", 
           _descUsed, program.headPA, program.tailPA, program.zHead);
    
    // Transfer ownership - don't call Cancel() now
    _pool = nullptr;
    _blk = {};
    _descUsed = 0;
    
    return program;
}

void ASOHCIATProgramBuilder::Cancel()
{
    if (_pool && _blk.valid) {
        os_log(ASLog(), "ASOHCIATProgramBuilder: Canceling program build, returning %u descriptors to pool", 
                    _blk.descriptorCount);
        _pool->FreeBlock(_blk);
    }
    
    _pool = nullptr;
    _blk = {};  
    _descUsed = 0;
    _ip = ATIntPolicy::kInterestingOnly;
}
