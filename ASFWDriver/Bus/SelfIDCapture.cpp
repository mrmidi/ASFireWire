#include "SelfIDCapture.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "../Common/BarrierUtils.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "Logging.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "TopologyTypes.hpp"

namespace {

constexpr size_t kSelfIDAlignment = 2048; // OHCI 1.1 Table 11-1

constexpr size_t RoundUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

namespace ASFW::Driver {

SelfIDCapture::SelfIDCapture() = default;
SelfIDCapture::~SelfIDCapture() {
    ReleaseBuffers();
}

kern_return_t SelfIDCapture::PrepareBuffers(size_t quadCapacity, HardwareInterface& hw) {
    ReleaseBuffers();

    if (quadCapacity == 0) {
        return kIOReturnBadArgument;
    }

    const size_t requestedBytes = quadCapacity * sizeof(uint32_t);
    const size_t allocBytes = RoundUp(std::max(requestedBytes, static_cast<size_t>(2048)), kSelfIDAlignment);

    IOBufferMemoryDescriptor* descriptor = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, allocBytes, kSelfIDAlignment, &descriptor);
    if (kr != kIOReturnSuccess || descriptor == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    descriptor->SetLength(allocBytes);
    buffer_ = OSSharedPtr(descriptor, OSNoRetain);
    bufferBytes_ = allocBytes;

    IOMemoryMap* map = nullptr;
    kr = buffer_->CreateMapping(0, 0, 0, 0, 0, &map);
    if (kr != kIOReturnSuccess || map == nullptr) {
        ReleaseBuffers();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }
    map_ = OSSharedPtr(map, OSNoRetain);

    dmaCommand_ = hw.CreateDMACommand();
    if (!dmaCommand_) {
        ReleaseBuffers();
        return kIOReturnNoResources;
    }

    uint32_t segmentCount = 1;
    IOAddressSegment segment{};
    uint64_t flags = 0;
    kr = dmaCommand_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                    buffer_.get(),
                                    0,
                                    allocBytes,
                                    &flags,
                                    &segmentCount,
                                    &segment);
    (void)flags;
    if (kr != kIOReturnSuccess || segmentCount < 1 || segment.address == 0 || segment.length < allocBytes) {
        ReleaseBuffers();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    }

    if ((segment.address & (kSelfIDAlignment - 1)) != 0) {
        ReleaseBuffers();
        return kIOReturnNotAligned;
    }

    segment_ = segment;
    segmentValid_ = true;
    quadCapacity_ = allocBytes / sizeof(uint32_t);

    if (map_) {
        const uint64_t addr = map_->GetAddress();
        if (addr != 0) {
            std::memset(reinterpret_cast<void*>(addr), 0, allocBytes);
            // Clearing the buffer upfront prevents stale generation metadata from a
            // previous capture from confusing the first post-reset decode. Linux
            // keeps its self_id_buffer zeroed for the same reason before arming.
        }
    }

    // Per OHCI §11.3: Controller owns the buffer once armed; don't CPU-write to it
    // Linux never touches the buffer between arming and Self-ID completion
    // Hardware writes generation|timestamp header at buffer[0] during Self-ID
    // Any CPU writes race with hardware DMA and can cause postedWriteErr

    armed_ = false;
    return kIOReturnSuccess;
}

void SelfIDCapture::ReleaseBuffers() {
    if (dmaCommand_) {
        dmaCommand_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    dmaCommand_.reset();
    map_.reset();
    buffer_.reset();
    segmentValid_ = false;
    bufferBytes_ = 0;
    quadCapacity_ = 0;
    armed_ = false;
}

kern_return_t SelfIDCapture::Arm(HardwareInterface& hw) {
    if (!segmentValid_) {
        return kIOReturnNotReady;
    }
    if (segment_.address > 0xFFFFFFFFULL) {
        return kIOReturnUnsupported;
    }

    // Per OHCI §11.2, SelfIDCount register is hardware-managed (all fields "ru")
    // Software MUST NOT write to it - hardware updates it automatically after DMA
    const uint32_t paddr = static_cast<uint32_t>(segment_.address);
    hw.WriteAndFlush(Register32::kSelfIDBuffer, paddr);
    ASFW_LOG(Hardware, "Self-ID buffer armed: paddr=0x%08x size=%llu bytes",
             paddr, segment_.length);

    armed_ = true;
    return kIOReturnSuccess;
}

void SelfIDCapture::Disarm(HardwareInterface& hw) {
    if (armed_) {
        // Per OHCI §11.1: Writing 0 to SelfIDBuffer disables Self-ID DMA
        // Do NOT write to SelfIDCount - it's hardware-managed per §11.2
        hw.WriteAndFlush(Register32::kSelfIDBuffer, 0);
    }
    armed_ = false;
}

std::optional<SelfIDCapture::Result> SelfIDCapture::Decode(uint32_t selfIDCountReg, HardwareInterface& hw) const {
    if (!segmentValid_ || !map_) {
        return std::nullopt;
    }

    const uint32_t quadCount = (selfIDCountReg & SelfIDCountBits::kSizeMask) >> SelfIDCountBits::kSizeShift;
    const uint32_t generation = (selfIDCountReg & SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;
    const bool error = (selfIDCountReg & SelfIDCountBits::kError) != 0;

    Result result;
    result.generation = generation;
    result.crcError = error;

    if (quadCount == 0 || error) {
        result.timedOut = (quadCount == 0);
        result.valid = false;
        return result;
    }
    
    if (quadCount > quadCapacity_) {
        ASFW_LOG(Hardware, "Self-ID quadCount=%u exceeds buffer capacity=%zu", quadCount, quadCapacity_);
        result.valid = false;
        return result;
    }

    const size_t cappedQuads = std::min<size_t>(quadCount, quadCapacity_);

    // DMA cache coherency for CPU reads after device DMA
    // Per DriverKit documentation: PerformOperation with kIODMACommandPerformOperationOptionRead
    // ensures cache coherency by copying from DMA buffer (with cache invalidation) to dest buffer
    // We pass NULL dest buffer because we want to read directly, but the operation still
    // triggers the necessary cache invalidation on the source DMA buffer
    // Alternative: We could PerformOperation to temp buffer, but that's wasteful
    // Instead, we rely on the fact that PrepareForDMA set up the mapping with proper coherency
    // and FullBarrier() ensures memory ordering for direct buffer access
    
    // Memory ordering barrier ensures DMA completion before CPU reads
    // This is the DriverKit equivalent of cache synchronization
    FullBarrier();

    // Null check for map address before dereference
    // GetAddress() returns uint64_t, not pointer - cast to check validity
    const uint64_t addr = map_->GetAddress();
    if (addr == 0) {
        ASFW_LOG(Hardware, "Self-ID map address is NULL - buffer mapping failed");
        result.valid = false;
        result.timedOut = false;
        return result;
    }
    
    const auto* base = reinterpret_cast<const uint32_t*>(addr);
    
    // ENHANCED VALIDATION per OHCI §11.3: Double-read generation check to detect racing bus resets
    // Algorithm: Read buffer generation → read register generation → compare all three values
    // If any mismatch occurs, a bus reset happened between DMA completion and our read
    if (cappedQuads > 0) {
        const uint32_t headerQuad = base[0];
        const uint32_t genMem = (headerQuad >> 16) & 0xFF;
        
        // CRITICAL: Re-read SelfIDCount register AFTER reading buffer to detect racing resets
        // If hardware started a new bus reset between DMA completion and now, the generation
        // in the register will have incremented while the buffer still contains old data
        const uint32_t selfIDCountReg2 = hw.Read(Register32::kSelfIDCount);
        const uint32_t generation2 = (selfIDCountReg2 & SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;
        
        if (generation != genMem) {
            ASFW_LOG(Hardware, "Self-ID generation mismatch (buffer vs initial read): buffer=%u register1=%u (racing bus reset detected)", 
                     genMem, generation);
            result.valid = false;
            result.crcError = false;
            result.timedOut = false;
            return result;
        }
        
        if (generation != generation2) {
            ASFW_LOG(Hardware, "Self-ID generation mismatch (initial vs double-read): register1=%u register2=%u (racing bus reset detected)",
                     generation, generation2);
            result.valid = false;
            result.crcError = false;
            result.timedOut = false;
            return result;
        }
        
        ASFW_LOG(Hardware, "Self-ID generation VALIDATED (double-read): %u matches (buffer=register1=register2)", generation);
        
#if ASFW_DEBUG_SELF_ID
        // Debug: Log first few quadlets to verify data
        ASFW_LOG_SELF_ID("Self-ID buffer header[0]=0x%08x (gen=%u ts=%u)",
                          headerQuad, genMem, (headerQuad & 0xFFFF));
        if (cappedQuads > 1) {
            ASFW_LOG_SELF_ID("Self-ID buffer[1]=0x%08x tag=%u", base[1], (base[1] >> 30) & 0x3);
        }
        if (cappedQuads > 2) {
            ASFW_LOG_SELF_ID("Self-ID buffer[2]=0x%08x tag=%u", base[2], (base[2] >> 30) & 0x3);
        }

        ASFW_LOG_SELF_ID("=== 🧾 Self-ID Debug ===");
        ASFW_LOG_SELF_ID("🧮 SelfIDCount=0x%08x generation=%u quadlets=%zu",
                          selfIDCountReg, generation, static_cast<size_t>(cappedQuads));

        const size_t preview = std::min<size_t>(cappedQuads, static_cast<size_t>(8));
        for (size_t index = 0; index < preview; ++index) {
            const uint32_t quad = base[index];
            const uint32_t tag = (quad >> 30) & 0x3u;
            ASFW_LOG_SELF_ID("  • [%02zu] 0x%08x tag=%u more=%u", index, quad, tag, static_cast<uint32_t>(quad & 0x1u));
        }
#endif

    }
    
    result.quads.assign(base, base + cappedQuads);

    // Enumerate Self-ID sequences inside the captured quad buffer and validate
    // extended chaining/sequence numbers using SelfIDSequenceEnumerator.
    // IMPORTANT: Skip header quadlet (quads[0]) - enumerator expects Self-ID packets only (start at quads[1])
    SelfIDSequenceEnumerator enumerator;
    if (result.quads.size() > 1) {
        enumerator.cursor = result.quads.data() + 1;  // Skip header at [0]
        enumerator.quadlet_count = static_cast<unsigned int>(result.quads.size() - 1);
    } else {
        enumerator.cursor = nullptr;
        enumerator.quadlet_count = 0;
    }

    bool enumeratorError = false;
    while (enumerator.quadlet_count > 0) {
        // Skip non-Self-ID quadlets (e.g., link-on packets with tag=01b)
        // OHCI §11: Self-ID buffer may contain other packet types
        if (enumerator.cursor && !IsSelfIDTag(*enumerator.cursor)) {
            ASFW_LOG_SELF_ID("Skipping non-Self-ID quadlet: 0x%08x tag=%u",
                              *enumerator.cursor, (*enumerator.cursor >> 30) & 0x3);
            enumerator.cursor++;
            enumerator.quadlet_count--;
            continue;
        }
        
        auto item = enumerator.next();
        if (!item.has_value()) {
            enumeratorError = true;
            break;
        }
        const auto [ptr, count] = *item;
        size_t startIndex = static_cast<size_t>(ptr - result.quads.data());
        result.sequences.emplace_back(startIndex, count);
    }

    // Validation: Don't check Self-ID tag on header quadlet (quads[0]) - it has NO tag field!
    // Header is generation|timestamp metadata (OHCI §11.3). Self-ID packets with tag=10b start at quads[1].
    result.valid = !result.quads.empty() && !enumeratorError;
    result.timedOut = false;
    
    ASFW_LOG(Hardware, "Self-ID decode complete: valid=%d quads=%zu sequences=%zu enumeratorError=%d",
             result.valid, result.quads.size(), result.sequences.size(), enumeratorError);
    std::string seqSummary;
    for (size_t i = 0; i < result.sequences.size(); ++i) {
        if (!seqSummary.empty()) {
            seqSummary.append(", ");
        }
        seqSummary.append("start=");
        seqSummary.append(std::to_string(result.sequences[i].first));
        seqSummary.append(" count=");
        seqSummary.append(std::to_string(result.sequences[i].second));
    }
    if (seqSummary.empty()) {
        seqSummary = "none";
    }
    ASFW_LOG(Hardware, "🧵 Sequences: %{public}s", seqSummary.c_str());
    if (!result.valid) {
        ASFW_LOG(Hardware, "❌ Self-ID decode flagged invalid data - inspect sequences above");
    } else {
        ASFW_LOG(Hardware, "✅ Self-ID decode valid");
    }
#if ASFW_DEBUG_SELF_ID
    ASFW_LOG_SELF_ID("=== End Self-ID Debug ===");
#endif

    return result;
}

} // namespace ASFW::Driver
