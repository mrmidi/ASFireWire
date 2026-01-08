#include "DescriptorBuilder.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <limits>

#include "../../Shared/Memory/DMAMemoryManager.hpp"
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

namespace ASFW::Async {

using HW::OHCIDescriptor;
using HW::OHCIDescriptorImmediate;

namespace {

// OHCI immediate descriptor immediate data capacity
// (32-byte descriptor - 16-byte header = 16 bytes for packet header)
constexpr std::size_t kImmediateCapacity = 16;
constexpr std::size_t kInvalidRingIndex = std::numeric_limits<std::size_t>::max();

inline void TraceBytes(const char* tag, const void* data, size_t length) {
    if (!DMAMemoryManager::IsTracingEnabled() || data == nullptr || length == 0) {
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t preview = std::min(length, static_cast<size_t>(64));
    char line[3 * 16 + 1];

    for (size_t offset = 0; offset < preview; offset += 16) {
        const size_t chunk = std::min(static_cast<size_t>(16), preview - offset);
        char* cursor = line;
        size_t remaining = sizeof(line);
        for (size_t i = 0; i < chunk && remaining > 3; ++i) {
            const int written = std::snprintf(cursor, remaining, "%02X ", bytes[offset + i]);
            if (written <= 0) {
                break;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        *cursor = '\0';
        ASFW_LOG(Async,
                 "    %{public}s +0x%04zx: %{public}s",
                 tag,
                 offset,
                 line);
    }
}

} // namespace

// Ensure the chain's last descriptor is flushed to memory
// Z encoding and branch control are set by LinkTailTo()/PatchBranchWord() during chain linking
static void FinalizeChainForSubmit(DescriptorBuilder::DescriptorChain& chain, DMAMemoryManager& dmaManager) {
    if (chain.last == nullptr) return;

    // BuildControl() already set b=BranchAlways for OUTPUT_LAST* descriptors
    // EOL is signaled by branchWord=0, not by clearing b bits (Agere/LSI requirement)
    // Z nibble is set by LinkTailTo() when chaining to next packet (not here)
    
    // Flush the last descriptor(s) to memory so hardware sees correct fields
    const size_t flushLength = static_cast<size_t>(chain.lastBlocks) * sizeof(HW::OHCIDescriptor);
    dmaManager.PublishRange(chain.last, flushLength);
}

namespace {

[[nodiscard]] size_t AdvanceIndex(size_t index, size_t delta, size_t capacity) noexcept {
    if (capacity == 0) {
        return 0;
    }
    return (index + delta) % capacity;
}

} // namespace

DescriptorBuilder::DescriptorBuilder(DescriptorRing& ring, DMAMemoryManager& dmaManager)
    : ring_(ring), dmaManager_(dmaManager) {
    // Start allocations after the sentinel descriptor at ring tail
    nextAllocationIndex_ = ring_.Tail();
}

size_t DescriptorBuilder::ReserveBlocks(uint8_t blocks) noexcept {
    const size_t capacity = ring_.Capacity();
    if (capacity == 0 || blocks == 0) {
        return kInvalidRingIndex;
    }

    const size_t tail = ring_.Tail();
    const size_t head = ring_.Head();

    auto recordNext = [&](size_t start) -> size_t {
        nextAllocationIndex_ = (start + blocks) % capacity;
        return start;
    };

    // Case 1: tail ahead of head (free space may be split across end/start)
    if (tail >= head) {
        const size_t spaceToEnd = capacity - tail;
        if (blocks <= spaceToEnd) {
            return recordNext(tail);
        }

        // Wrap: need contiguous space at beginning strictly before head
        if (blocks <= head) {
            return recordNext(0);
        }

        return kInvalidRingIndex;
    }

    // Case 2: tail before head (single contiguous free region [tail, head))
    const size_t spaceAvailable = head - tail;
    if (blocks <= spaceAvailable) {
        return recordNext(tail);
    }

    return kInvalidRingIndex;
}

DescriptorBuilder::DescriptorChain DescriptorBuilder::BuildTransactionChain(const void* headerData,
                                                                            std::size_t headerSize,
                                                                            uint64_t payloadDeviceAddress,
                                                                            std::size_t payloadSize,
                                                                            bool needsFlush) {
    DescriptorChain chain{};
    chain.needsFlush = needsFlush;  // Set Apple offset +40 pattern flag

    const bool tracing = DMAMemoryManager::IsTracingEnabled();
    if (tracing) {
        ASFW_LOG(Async,
                 "🧭 BuildTransactionChain: header=%zu payload=%zu needsFlush=%u head=%zu tail=%zu prevBlocks=%u",
                 headerSize,
                 payloadSize,
                 needsFlush ? 1u : 0u,
                 ring_.Head(),
                 ring_.Tail(),
                 ring_.PrevLastBlocks());
    }

    // Defensive rebase: sync allocation cursor to the ring tail to avoid
    // rare overlap if the builder instance is reused across submissions.
    nextAllocationIndex_ = ring_.Tail();

    if (headerData == nullptr) {
        return chain;
    }

    // Validate header size fits in immediate descriptor
    if (headerSize == 0 || headerSize > kImmediateCapacity) {
        return chain;
    }

    // Validate payload size fits in OHCI reqCount field (16-bit)
    if (payloadSize > 0xFFFFu) {
        return chain;
    }

    // CRITICAL: Always use kIntAlways (i=3) for OUTPUT_LAST descriptors per Apple's pattern
    // (DecompilationAnalysis.md Line 87: asyncRead uses i=3 for all OUTPUT_LAST_Immediate)
    // This ensures we ALWAYS get an AT_req completion IRQ, even if no AR response arrives.
    // Without this, timeout detection depends solely on software timers, which is unreliable.
    // The interrupt policy is the same for both single-descriptor and two-descriptor paths.
    // NOTE: generateInterrupt parameter removed - always interrupt on OUTPUT_LAST per spec.

    const size_t capacity = ring_.Capacity();
    if (capacity == 0) {
        return chain;
    }

    // Single-descriptor path: header-only (read request, write with quadlet data)
    chain.txid = txCounter_.fetch_add(1u, std::memory_order_relaxed);

    if (payloadSize == 0) {
        constexpr uint8_t kImmediateBlocks = 2;

        // Allocate descriptor from ring
        const size_t ringIndex = ReserveBlocks(kImmediateBlocks);
        if (ringIndex == kInvalidRingIndex) {
            const size_t head = ring_.Head();
            const size_t tail = ring_.Tail();
            const size_t used = (tail >= head) ? (tail - head) : (capacity - head + tail);
            ASFW_LOG(Async,
                     "❌ ReserveBlocks failed (txid=%u blocks=%u head=%zu tail=%zu capacity=%zu used=%zu)",
                     chain.txid,
                     kImmediateBlocks,
                     head,
                     tail,
                     capacity,
                     used);
            // Ring is full - likely a descriptor leak. Log ring state for forensics.
            if (used > capacity - 4) {
                ASFW_LOG(Async, "  ⚠️ RING NEARLY FULL: %zu/%zu slots used. Check ScanCompletion is advancing head.", used, capacity);
            }
            return chain;
        }

        auto* descriptor = ring_.At(ringIndex);
        if (!descriptor) {
            return chain;
        }

        // Zero descriptor memory
        std::memset(descriptor, 0, sizeof(OHCIDescriptorImmediate));
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(descriptor);

        // Copy packet header to immediate data area BEFORE publishing control
        std::memcpy(immDesc->immediateData, headerData, headerSize);

        // HEX DUMP: Complete AT packet before transmission
        ASFW_LOG_V3(Async, "🔍 AT TX PACKET (txid=%u headerSize=%zu):", chain.txid, headerSize);
        for (size_t i = 0; i < headerSize; i += 16) {
            const size_t chunkSize = (i + 16 <= headerSize) ? 16 : (headerSize - i);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(immDesc->immediateData) + i;
            ASFW_LOG_V3(Async, "  [%02zu] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                     i,
                     chunkSize > 0 ? bytes[0] : 0, chunkSize > 1 ? bytes[1] : 0,
                     chunkSize > 2 ? bytes[2] : 0, chunkSize > 3 ? bytes[3] : 0,
                     chunkSize > 4 ? bytes[4] : 0, chunkSize > 5 ? bytes[5] : 0,
                     chunkSize > 6 ? bytes[6] : 0, chunkSize > 7 ? bytes[7] : 0,
                     chunkSize > 8 ? bytes[8] : 0, chunkSize > 9 ? bytes[9] : 0,
                     chunkSize > 10 ? bytes[10] : 0, chunkSize > 11 ? bytes[11] : 0,
                     chunkSize > 12 ? bytes[12] : 0, chunkSize > 13 ? bytes[13] : 0,
                     chunkSize > 14 ? bytes[14] : 0, chunkSize > 15 ? bytes[15] : 0);
        }

        if (headerSize < kImmediateCapacity) {
            std::memset(reinterpret_cast<std::byte*>(immDesc->immediateData) + headerSize,
                        0,
                        kImmediateCapacity - headerSize);
        }
        if (tracing) {
            TraceBytes("Immediate header payload", headerData, headerSize);
        }

        // Publish non-control fields first, then release fence before setting control
        immDesc->common.branchWord = 0;           // EOL indicated by branchWord=0
        std::atomic_thread_fence(std::memory_order_release);

        // Configure descriptor control word (ping=false for standard async requests)
        // CRITICAL EOL ENCODING per OHCI spec and Apple implementation:
        // - OUTPUT_LAST must ALWAYS use b=BranchAlways, even at EOL
        // - EOL is indicated SOLELY by branchWord=0
        // - Using b=BranchNever on OUTPUT_LAST triggers evt_unknown on strict controllers
        // - Apple's control word baseline: 0x123C0010 = cmd=1, key=2, i=3, b=3, reqCount=16
        immDesc->common.control = OHCIDescriptor::BuildControl(
            static_cast<uint16_t>(headerSize),    // reqCount
            OHCIDescriptor::kCmdOutputLast,       // cmd
            OHCIDescriptor::kKeyImmediate,        // key
            OHCIDescriptor::kIntAlways,           // i=3: always interrupt
            OHCIDescriptor::kBranchAlways,        // b=3: ALWAYS for OUTPUT_LAST (EOL via branchWord)
            false                                 // ping
        );

        dmaManager_.PublishRange(immDesc, sizeof(OHCIDescriptorImmediate));

        // DIAGNOSTIC: Verify EOL encoding
        // Log control/branchWord/immediate data for hardware debugging
        const uint32_t ctl = immDesc->common.control;
        const uint32_t br = immDesc->common.branchWord;
        const uint16_t reqCountField = static_cast<uint16_t>(ctl & 0xFFFFu);
        const uint8_t* imm = reinterpret_cast<const uint8_t*>(immDesc->immediateData);
        ASFW_LOG_V2(Async,
                 "LAST-Imm: ctl=0x%08x br=0x%08x len=%u data[0..15]=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 ctl,
                 br,
                 reqCountField,
                 imm[0], imm[1], imm[2], imm[3],
                 imm[4], imm[5], imm[6], imm[7],
                 imm[8], imm[9], imm[10], imm[11],
                 imm[12], imm[13], imm[14], imm[15]);

        // CRITICAL TELEMETRY: Parse TX header fields in host byte order
        // This allows verification that tLabel matches between TX and RX paths
        const uint32_t quadlet0 = immDesc->immediateData[0];  // Host byte order
        const uint16_t txDestID = static_cast<uint16_t>((quadlet0 >> 16) & 0xFFFF);
        const uint8_t txTLabel = static_cast<uint8_t>((quadlet0 >> 10) & 0x3F);
        const uint8_t txRetry = static_cast<uint8_t>((quadlet0 >> 8) & 0x03);
        const uint8_t txTCode = static_cast<uint8_t>((quadlet0 >> 4) & 0x0F);
        const uint8_t txPriority = static_cast<uint8_t>(quadlet0 & 0x0F);
        ASFW_LOG_V2(Async,
                 "📤 TX Header (host order): destID=0x%04X tLabel=%u retry=%u tCode=0x%X pri=%u",
                 txDestID, txTLabel, txRetry, txTCode, txPriority);

        if (tracing) {
            TraceBytes("Immediate descriptor (32B)", immDesc, sizeof(OHCIDescriptorImmediate));
        }

        // Assert correct EOL form: b=Always (11), branchWord=0, i=Always (11)
        const uint32_t ctlHi = ctl >> OHCIDescriptor::kControlHighShift;
        const uint8_t bField = (ctlHi >> OHCIDescriptor::kBranchShift) & 0x3;
        const uint8_t iField = (ctlHi >> OHCIDescriptor::kIntShift) & 0x3;
        const uint8_t cmdField = (ctlHi >> OHCIDescriptor::kCmdShift) & 0xF;
        const uint8_t keyField = (ctlHi >> OHCIDescriptor::kKeyShift) & 0x7;

        if (bField != OHCIDescriptor::kBranchAlways) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: b field=%u (expected kBranchAlways=3)", bField);
        }
        if (br != 0) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: branchWord=0x%08x (expected 0 for EOL)", br);
        }
        if (iField != OHCIDescriptor::kIntAlways) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: i field=%u (expected kIntAlways=3)", iField);
        }
        if (cmdField != OHCIDescriptor::kCmdOutputLast) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: cmd field=%u (expected kCmdOutputLast=1)", cmdField);
        }
        if (keyField != OHCIDescriptor::kKeyImmediate) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: key field=%u (expected kKeyImmediate=2)", keyField);
        }
        if (reqCountField != headerSize) {
            ASFW_LOG(Async, "❌ ASSERTION FAILED: reqCount=%u (expected %zu)", reqCountField, headerSize);
        }

        // Get device-visible address from DMAMemoryManager
        const uint64_t descriptorIOVA = dmaManager_.VirtToIOVA(descriptor);
        ASFW_LOG_V3(Async,
                 "DescriptorBuilder: txid=%u ring[%zu] virt=%p -> iova=0x%llx (slabBase=0x%llx)",
                 chain.txid,
                 ringIndex,
                 descriptor,
                 descriptorIOVA,
                 dmaManager_.BaseIOVA());
        if (descriptorIOVA == 0 || descriptorIOVA > 0xFFFFFFFFu || (descriptorIOVA & 0xFULL) != 0) {
            // Device address invalid: zero, not 32-bit, or not 16-byte aligned
            return chain;
        }

        chain.first = &immDesc->common;
        chain.last = &immDesc->common;
        chain.firstIOVA32 = static_cast<uint32_t>(descriptorIOVA);
        chain.lastIOVA32 = chain.firstIOVA32;
        chain.firstBlocks = kImmediateBlocks;
        chain.lastBlocks = chain.firstBlocks;
        chain.firstRingIndex = ringIndex;
        chain.lastRingIndex = AdvanceIndex(ringIndex, kImmediateBlocks - 1, capacity);
        if (tracing) {
            ASFW_LOG(Async,
                     "🧭 Chain summary: txid=%u firstIOVA=0x%08x lastIOVA=0x%08x firstIdx=%zu lastIdx=%zu blocks=%u",
                     chain.txid,
                     chain.firstIOVA32,
                     chain.lastIOVA32,
                     chain.firstRingIndex,
                     chain.lastRingIndex,
                     chain.firstBlocks);
        }
        // Finalize last descriptor (normalize branch/Z nibble and flush) before returning
        FinalizeChainForSubmit(chain, dmaManager_);
        return chain;
    }

    // Two-descriptor path: header + payload (write with block data, lock requests)

    // Validate payload device address
    // OHCI requires quadlet alignment (4 bytes) for dataAddress, not 16 bytes.
    if (payloadDeviceAddress == 0 || (payloadDeviceAddress & 0x3ULL) != 0 || payloadDeviceAddress > 0xFFFFFFFFu) {
        return chain;
    }

    // CRITICAL: Reserve all 3 blocks contiguously to ensure OUTPUT_MORE→OUTPUT_LAST contiguity
    // OUTPUT_MORE (b=00) requires physical contiguity per OHCI spec Table 7-2
    // Allocating 2+1 separately can break contiguity if first allocation ends at ring boundary
    constexpr uint8_t kImmediateBlocks = 2;
    constexpr uint8_t kStandardBlocks = 1;
    constexpr uint8_t kTotalBlocks = kImmediateBlocks + kStandardBlocks;  // 3 blocks

    const size_t chainStart = ReserveBlocks(kTotalBlocks);
    if (chainStart == kInvalidRingIndex) {
        const size_t head = ring_.Head();
        const size_t tail = ring_.Tail();
        const size_t used = (tail >= head) ? (tail - head) : (capacity - head + tail);
        ASFW_LOG(Async,
                 "❌ ReserveBlocks failed (txid=%u blocks=%u head=%zu tail=%zu capacity=%zu used=%zu)",
                 chain.txid,
                 kTotalBlocks,
                 head,
                 tail,
                 capacity,
                 used);
        if (used > capacity - 4) {
            ASFW_LOG(Async, "  ⚠️ RING NEARLY FULL: %zu/%zu slots used. Check ScanCompletion is advancing head.", used, capacity);
        }
        return chain;
    }
    const size_t headerRingIndex = chainStart;
    const size_t payloadRingIndex = (chainStart + kImmediateBlocks) % capacity;

    auto* headerDescriptor = ring_.At(headerRingIndex);
    if (!headerDescriptor) {
        return chain;
    }

    auto* payloadDescriptor = ring_.At(payloadRingIndex);
    if (!payloadDescriptor) {
        return chain;
    }

    // Zero descriptors
    std::memset(headerDescriptor, 0, sizeof(OHCIDescriptorImmediate));
    std::memset(payloadDescriptor, 0, sizeof(OHCIDescriptor));

    auto* headerImmDesc = reinterpret_cast<OHCIDescriptorImmediate*>(headerDescriptor);
    if (tracing) {
        TraceBytes("Immediate header payload", headerData, headerSize);
    }

    // Get device-visible addresses
    const uint64_t headerPhys = dmaManager_.VirtToIOVA(headerDescriptor);
    const uint64_t payloadDescriptorPhys = dmaManager_.VirtToIOVA(payloadDescriptor);
    ASFW_LOG_V3(Async,
             "DescriptorBuilder: txid=%u header ring[%zu] virt=%p -> iova=0x%llx; payload ring[%zu] virt=%p -> iova=0x%llx (slabBase=0x%llx)",
             chain.txid,
             headerRingIndex,
             headerDescriptor,
             headerPhys,
             payloadRingIndex,
             payloadDescriptor,
             payloadDescriptorPhys,
             dmaManager_.BaseIOVA());

    if (headerPhys == 0 || payloadDescriptorPhys == 0 ||
        headerPhys > 0xFFFFFFFFu || payloadDescriptorPhys > 0xFFFFFFFFu ||
        (headerPhys & 0xFULL) != 0 || (payloadDescriptorPhys & 0xFULL) != 0) {
        return chain;
    }

    // Copy packet header to header descriptor immediate data BEFORE publishing control
    std::memcpy(headerImmDesc->immediateData, headerData, headerSize);
    if (headerSize < kImmediateCapacity) {
        std::memset(reinterpret_cast<std::byte*>(headerImmDesc->immediateData) + headerSize,
                    0,
                    kImmediateCapacity - headerSize);
    }

    // DIAGNOSTIC: Log header quadlets for 16-byte header transactions
    // Extract tCode from Q0 bits[7:4] to determine transaction type
    uint32_t q3_initial = 0;
    uint8_t tCode = 0;
    const char* txTypeName = "Unknown";
    if (headerSize == 16) {
        const uint32_t q0 = headerImmDesc->immediateData[0];
        tCode = static_cast<uint8_t>((q0 >> 4) & 0x0F);

        // Determine transaction type from tCode
        switch (tCode) {
            case 0x0: txTypeName = "Write Quadlet"; break;
            case 0x1: txTypeName = "Block Write"; break;
            case 0x9: txTypeName = "Lock Request (CAS)"; break;
            default: txTypeName = "Unknown"; break;
        }

        ASFW_LOG_V3(Async,
                 "🔍 %{public}s descriptor header (tCode=0x%X): Q0=0x%08x Q1=0x%08x Q2=0x%08x Q3=0x%08x",
                 txTypeName, tCode,
                 headerImmDesc->immediateData[0],
                 headerImmDesc->immediateData[1],
                 headerImmDesc->immediateData[2],
                 headerImmDesc->immediateData[3]);

        // Parse Q3 to show dataLength + extTcode
        q3_initial = headerImmDesc->immediateData[3];
        const uint16_t dataLength = static_cast<uint16_t>(q3_initial >> 16);
        const uint16_t extTcode = static_cast<uint16_t>(q3_initial & 0xFFFFu);

        if (tCode == 0x9) {
            // LOCK: expect dataLength=8, extTcode=0x0002
            ASFW_LOG_V3(Async,
                     "   Q3 decode: dataLength=%u extTcode=0x%04x (expected: dataLength=8 extTcode=0x0002 for CAS)",
                     dataLength, extTcode);
        } else {
            // Block Write or Write Quadlet: just show values
            ASFW_LOG_V3(Async,
                     "   Q3 decode: dataLength=%u extTcode=0x%04x",
                     dataLength, extTcode);
        }
    }

    // OUTPUT_MORE relies on physical contiguity; branchWord is ignored per OHCI §7.1.
    // Keep b=00 and a zero branchWord to match spec.
    headerImmDesc->common.branchWord = 0;

    std::atomic_thread_fence(std::memory_order_release);

    // Configure header descriptor (OUTPUT_MORE_Immediate, ping=false for standard async)
    // OHCI Table 7-2: OUTPUT_MORE* descriptors MUST have b=00 per spec
    // Hardware links to next descriptor via contiguity, not via branchWord
    headerImmDesc->common.control = OHCIDescriptor::BuildControl(
        static_cast<uint16_t>(headerSize),        // reqCount
        OHCIDescriptor::kCmdOutputMore,           // cmd
        OHCIDescriptor::kKeyImmediate,            // key
        OHCIDescriptor::kIntNever,                // i (interrupt control)
        OHCIDescriptor::kBranchNever,             // b=00: REQUIRED by OHCI spec for OUTPUT_MORE*
        false                                     // ping
    );

    dmaManager_.PublishRange(headerImmDesc, sizeof(OHCIDescriptorImmediate));

    // Configure payload descriptor (OUTPUT_LAST, ping=false for standard async)
    // CRITICAL EOL ENCODING: b=BranchAlways even at EOL (per OHCI spec)
    // EOL indicated by branchWord=0, NOT by b=BranchNever
    const uint32_t payloadIOVA32 = static_cast<uint32_t>(payloadDeviceAddress);
    payloadDescriptor->dataAddress = payloadIOVA32;
    payloadDescriptor->branchWord = 0;            // EOL signaled by zero branchWord

    std::atomic_thread_fence(std::memory_order_release);

    payloadDescriptor->control = OHCIDescriptor::BuildControl(
        static_cast<uint16_t>(payloadSize),       // reqCount
        OHCIDescriptor::kCmdOutputLast,           // cmd
        OHCIDescriptor::kKeyStandard,             // key
        OHCIDescriptor::kIntAlways,               // i=3: always interrupt on OUTPUT_LAST
        OHCIDescriptor::kBranchAlways,            // b=3: ALWAYS for OUTPUT_LAST (even at EOL)
        false                                     // ping
    );

    dmaManager_.PublishRange(payloadDescriptor, sizeof(OHCIDescriptor));

    // DIAGNOSTIC: Log descriptor control words
    if (headerSize == 16) {
        const uint16_t headerReqCount = static_cast<uint16_t>(headerImmDesc->common.control & 0xFFFFu);
        const uint16_t payloadReqCount = static_cast<uint16_t>(payloadDescriptor->control & 0xFFFFu);
        ASFW_LOG_V3(Async,
                 "🔍 %{public}s descriptor chain configured:",
                 txTypeName);
        ASFW_LOG_V3(Async,
                 "   Header descriptor: reqCount=%u (expected 16 for all 16-byte headers)",
                 headerReqCount);
        ASFW_LOG_V3(Async,
                 "   Payload descriptor: reqCount=%u dataAddr=0x%08x",
                 payloadReqCount, payloadDescriptor->dataAddress);

        if (headerReqCount != 16) {
            ASFW_LOG_V1(Async, "   ❌ ERROR: Header reqCount is %u, should be 16!", headerReqCount);
        }

        // Only validate payload size for LOCK transactions (tCode 0x9)
        if (tCode == 0x9 && payloadReqCount != 8) {
            ASFW_LOG_V1(Async, "   ❌ ERROR: LOCK payload reqCount is %u, should be 8!", payloadReqCount);
        }

        // Re-check Q3 after descriptor configuration (ensure it wasn't corrupted)
        const uint32_t q3_after = headerImmDesc->immediateData[3];
        if (q3_after != q3_initial) {
            ASFW_LOG_V1(Async,
                     "   ❌ CRITICAL: Q3 changed after descriptor config! was=0x%08x now=0x%08x",
                     q3_initial, q3_after);
        }
    }

    chain.first = &headerImmDesc->common;
    chain.last = payloadDescriptor;
    chain.firstIOVA32 = static_cast<uint32_t>(headerPhys);
    chain.lastIOVA32 = static_cast<uint32_t>(payloadDescriptorPhys);
    chain.firstBlocks = kImmediateBlocks;  // Immediate descriptor = 32 bytes = 2 blocks
    chain.lastBlocks = 1;                  // Standard descriptor = 16 bytes = 1 block
    chain.firstRingIndex = headerRingIndex;
    chain.lastRingIndex = AdvanceIndex(payloadRingIndex, chain.lastBlocks - 1, capacity);
    if (tracing) {
        TraceBytes("Immediate descriptor (32B)", headerImmDesc, sizeof(OHCIDescriptorImmediate));
        TraceBytes("Payload descriptor (16B)", payloadDescriptor, sizeof(OHCIDescriptor));
        ASFW_LOG(Async,
                 "🧭 Chain summary: firstIOVA=0x%08x lastIOVA=0x%08x firstIdx=%zu lastIdx=%zu blocks=%u",
                 chain.firstIOVA32,
                 chain.lastIOVA32,
                 chain.firstRingIndex,
                 chain.lastRingIndex,
                 chain.TotalBlocks());
    }
    // Finalize last descriptor (normalize branch/Z nibble and flush) before returning
    FinalizeChainForSubmit(chain, dmaManager_);
    return chain;
}

void DescriptorBuilder::LinkChain(DescriptorChain& chainToModify,
                                  uint64_t nextChainIOVA,
                                  uint8_t nextChainBlockCount) {
    if (chainToModify.last == nullptr) {
        return;
    }

    const uint32_t branch = HW::MakeBranchWordAT(nextChainIOVA, nextChainBlockCount);
    if (branch == 0) {
        // Invalid parameters: MakeBranchWordAT validates alignment, 32-bit range, and Z ∈ [2,8]
        return;
    }

    // Use PatchBranchWord() helper: writes branchWord first, sets b=11, then flushes
    // This ensures correct memory ordering (branch ptr visible before branch-always bit)
    PatchBranchWord(chainToModify.last, branch);
}

// Patch a descriptor's branchWord (and set BranchAlways control) and flush the descriptor
// Write branchWord BEFORE modifying control to ensure proper memory ordering
// Hardware must see the link pointer before we mark it as branch-always
//
// Flushing only 4 bytes leaves stale data in controller's prefetch buffer.
// Must flush entire descriptor structure (16B for standard, 32B for immediate) to bust prefetch.
void DescriptorBuilder::PatchBranchWord(HW::OHCIDescriptor* descriptor, uint32_t branchWord) noexcept {
    if (!descriptor) return;
    
    // Step 1: Publish the new branch target before touching control metadata.
    descriptor->branchWord = branchWord;
    std::atomic_thread_fence(std::memory_order_release);

    // Step 2: Ensure the b-field remains BranchAlways without recomposing the control word.
    uint32_t control = descriptor->control;
    const uint32_t branchMask = 0x3u << (HW::OHCIDescriptor::kControlHighShift + HW::OHCIDescriptor::kBranchShift);
    const uint32_t desiredBranch = static_cast<uint32_t>(HW::OHCIDescriptor::kBranchAlways) << (HW::OHCIDescriptor::kControlHighShift + HW::OHCIDescriptor::kBranchShift);

    if ((control & branchMask) != desiredBranch) {
        if (control == 0) {
            ASFW_LOG_V2(Async, "⚠️ PatchBranchWord: descriptor control word unexpectedly zero while linking");
        }
        control &= ~branchMask;
        control |= desiredBranch;
        descriptor->control = control;
    }

    // Step 3: Flush descriptor - 16B for standard, 32B for immediate
    // Check if descriptor is immediate (key=Immediate) to determine flush size
    const bool isImm = HW::IsImmediate(*descriptor);
    dmaManager_.PublishRange(descriptor, isImm ? sizeof(HW::OHCIDescriptorImmediate) : sizeof(HW::OHCIDescriptor));
}

// Flush a contiguous descriptor range starting at `start` for `blocks` 16-byte units
void DescriptorBuilder::FlushDescriptorRange(HW::OHCIDescriptor* start, uint8_t blocks) noexcept {
    if (!start || blocks == 0) return;
    const size_t length = static_cast<size_t>(blocks) * sizeof(HW::OHCIDescriptor);
    dmaManager_.PublishRange(start, length);
}

// Flush the chain (first..last descriptors)
void DescriptorBuilder::FlushChain(const DescriptorChain& chain) noexcept {
    if (chain.Empty()) return;
    FlushDescriptorRange(chain.first, chain.firstBlocks);
    if (chain.last && chain.last != chain.first) {
        FlushDescriptorRange(chain.last, chain.lastBlocks);
    }
}

// Tag descriptor with software tag (for completion matching)
void DescriptorBuilder::TagSoftware(HW::OHCIDescriptor* tail, uint32_t /*tag*/) noexcept {
    if (!tail) return;
    // Hardware expects status/xfer fields to start at zero. Leave them untouched here.
    // Retaining a software tag inside the descriptor risks corrupting branch/status metadata.
}

// Patch the existing tail descriptor at tailIndex to point to newChain
// CRITICAL: Per OHCI spec and Apple's implementation, must patch the LAST descriptor
// of the previous chain, because only OUTPUT_LAST* descriptors read branchWord.
// OUTPUT_MORE* descriptors have b=00 and hardware ignores their branchWord field.
bool DescriptorBuilder::LinkTailTo(size_t tailIndex, const DescriptorChain& newChain) noexcept {
    if (ring_.Capacity() == 0 || newChain.Empty()) {
        return false;
    }

    HW::OHCIDescriptor* prevLast = nullptr;
    size_t prevLastIndex = 0;
    uint8_t prevBlocks = 0;
    if (!ring_.LocatePreviousLast(tailIndex, prevLast, prevLastIndex, prevBlocks)) {
        ASFW_LOG_V2(Async,
                 "LinkTailTo: no previous LAST descriptor to link (txid=%u tail=%zu)",
                 newChain.txid,
                 tailIndex);
        return false;
    }

    const bool prevImmediate = HW::IsImmediate(*prevLast);
    const uint8_t nextPacketBlocks = newChain.TotalBlocks();
    const uint32_t branch = HW::MakeBranchWordAT(static_cast<uint64_t>(newChain.firstIOVA32), nextPacketBlocks);
    if (branch == 0) {
        ASFW_LOG_V2(Async,
                 "LinkTailTo: invalid branch encoding (txid=%u blocks=%u iova=0x%08x)",
                 newChain.txid,
                 nextPacketBlocks,
                 newChain.firstIOVA32);
        return false;
    }

    const uint8_t zNibble = static_cast<uint8_t>(branch & 0xFu);
    if (zNibble != (nextPacketBlocks & 0xFu)) {
        ASFW_LOG_V2(Async,
                 "LinkTailTo: Z mismatch (txid=%u zNibble=%u blocks=%u)",
                 newChain.txid,
                 zNibble,
                 nextPacketBlocks);
    }

    const uint32_t controlBefore = prevLast->control;
    const uint32_t branchBefore = prevLast->branchWord;

    const bool tracing = DMAMemoryManager::IsTracingEnabled();
    if (tracing) {
        ASFW_LOG_V4(Async,
                 "LinkTailTo: txid=%u prevLast[%zu] prevBlocks=%u imm=%d ctrl_before=0x%08x br_before=0x%08x -> firstIOVA=0x%08x blocks=%u Z=%u branch=0x%08x",
                 newChain.txid,
                 prevLastIndex,
                 prevBlocks,
                 prevImmediate ? 1 : 0,
                 controlBefore,
                 branchBefore,
                 newChain.firstIOVA32,
                 nextPacketBlocks,
                 zNibble,
                 branch);
    } else {
        ASFW_LOG_V3(Async,
                 "LinkTailTo: prevIdx=%zu branch=0x%08x -> 0x%08x blocks=%u",
                 prevLastIndex,
                 branchBefore,
                 branch,
                 nextPacketBlocks);
    }

    // Patch the LAST descriptor's branchWord (where hardware actually reads it)
    // PatchBranchWord() writes branchWord first, then sets b=11, then flushes
    PatchBranchWord(prevLast, branch);

    if (tracing) {
        const uint32_t controlAfter = prevLast->control;
        const uint32_t branchAfter = prevLast->branchWord;
        ASFW_LOG_V4(Async,
                 "LinkTailTo: txid=%u patched prevLast[%zu] ctrl_after=0x%08x br_after=0x%08x",
                 newChain.txid,
                 prevLastIndex,
                 controlAfter,
                 branchAfter);
    }

    return true;
}

// Revert (unlink) the tail descriptor's branch back to EOL state
// Used when PATH 2→1 fallback occurs: removes stale linkage before re-arming via CommandPtr
void DescriptorBuilder::UnlinkTail(size_t tailIndex) noexcept {
    if (ring_.Capacity() == 0) return;

    HW::OHCIDescriptor* prevLast = nullptr;
    size_t prevLastIndex = 0;
    uint8_t prevBlocks = 0;
    if (!ring_.LocatePreviousLast(tailIndex, prevLast, prevLastIndex, prevBlocks)) {
        return;
    }

    // Revert to EOL: branchWord=0 (leave b=BranchAlways unchanged)
    // Per OHCI spec: EOL indicated by branchWord==0, b field stays BranchAlways
    // This removes the stale linkage written during failed WAKE attempt
    prevLast->branchWord = 0;

    // Flush descriptor - 16B for standard, 32B for immediate (match PatchBranchWord logic)
    // Flushing correct size avoids unnecessary cache pollution on picky controllers
    const bool isImm = HW::IsImmediate(*prevLast);
    dmaManager_.PublishRange(prevLast, isImm ? sizeof(HW::OHCIDescriptorImmediate) : sizeof(HW::OHCIDescriptor));

    ASFW_LOG_V3(Async, "UnlinkTail: Reverted prevLast[%zu] to EOL (branchWord=0, b=Always unchanged, flushed %zu bytes)",
             prevLastIndex, isImm ? sizeof(HW::OHCIDescriptorImmediate) : sizeof(HW::OHCIDescriptor));
    
    // Verify b field is still BranchAlways (should not have been modified)
    const uint32_t ctlHi = prevLast->control >> HW::OHCIDescriptor::kControlHighShift;
    const uint8_t bField = (ctlHi >> HW::OHCIDescriptor::kBranchShift) & 0x3;
    if (bField != HW::OHCIDescriptor::kBranchAlways) {
        ASFW_LOG_V1(Async, "❌ UnlinkTail: prevLast has b=%u (expected kBranchAlways=3)", bField);
    }
}

void DescriptorBuilder::FlushTail(size_t tailIndex, uint8_t blocks) noexcept {
    if (ring_.Capacity() == 0) return;
    HW::OHCIDescriptor* desc = ring_.At(tailIndex);
    if (!desc) return;
    FlushDescriptorRange(desc, blocks);
}

} // namespace ASFW::Async
