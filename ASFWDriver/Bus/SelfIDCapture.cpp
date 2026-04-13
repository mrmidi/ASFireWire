#include "SelfIDCapture.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

#include "../Common/BarrierUtils.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "Logging.hpp"
#include "TopologyTypes.hpp"

namespace {

constexpr size_t kSelfIDAlignment = 2048; // OHCI 1.1 Table 11-1

constexpr size_t RoundUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

using DecodeError = ASFW::Driver::SelfIDCapture::DecodeError;
using DecodeErrorCode = ASFW::Driver::SelfIDCapture::DecodeErrorCode;

[[nodiscard]] DecodeError MakeDecodeError(DecodeErrorCode code, uint32_t countRegister,
                                          uint32_t generation, size_t quadletIndex = 0) {
    return DecodeError{code, countRegister, generation, quadletIndex};
}

[[nodiscard]] std::expected<std::vector<uint32_t>, DecodeError>
NormalizeSelfIDQuadlets(std::span<const uint32_t> rawQuadlets, uint32_t countRegister,
                        uint32_t generation) {
    if (rawQuadlets.size() <= 1) {
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::EmptyCapture, countRegister, generation));
    }

    const auto payload = rawQuadlets.subspan(1);
    if ((payload.size() % 2U) != 0U) {
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::InvalidInversePair, countRegister, generation,
                            rawQuadlets.size() - 1));
    }

    std::vector<uint32_t> normalized;
    normalized.reserve(1 + payload.size() / 2U);
    normalized.push_back(rawQuadlets.front());

    for (size_t index = 0; index < payload.size(); index += 2U) {
        const uint32_t quadlet = payload[index];
        const uint32_t inverse = payload[index + 1U];
        if (quadlet != ~inverse) {
            return std::unexpected(
                MakeDecodeError(DecodeErrorCode::InvalidInversePair, countRegister, generation,
                                index + 1U));
        }
        normalized.push_back(quadlet);
    }

    return normalized;
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
        }
    }

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

const char* SelfIDCapture::DecodeErrorCodeString(DecodeErrorCode code) noexcept {
    switch (code) {
    case DecodeErrorCode::BufferUnavailable:
        return "BufferUnavailable";
    case DecodeErrorCode::ControllerErrorBit:
        return "ControllerErrorBit";
    case DecodeErrorCode::EmptyCapture:
        return "EmptyCapture";
    case DecodeErrorCode::CountOverflow:
        return "CountOverflow";
    case DecodeErrorCode::NullMapAddress:
        return "NullMapAddress";
    case DecodeErrorCode::GenerationMismatch:
        return "GenerationMismatch";
    case DecodeErrorCode::InvalidInversePair:
        return "InvalidInversePair";
    case DecodeErrorCode::MalformedSequence:
        return "MalformedSequence";
    }
    return "Unknown";
}

std::expected<SelfIDCapture::Result, SelfIDCapture::DecodeError>
SelfIDCapture::Decode(uint32_t selfIDCountReg, HardwareInterface& hw) const {
    if (!segmentValid_ || !map_) {
        return std::unexpected(MakeDecodeError(DecodeErrorCode::BufferUnavailable, selfIDCountReg,
                                               0));
    }

    const uint32_t quadCount =
        (selfIDCountReg & SelfIDCountBits::kSizeMask) >> SelfIDCountBits::kSizeShift;
    const uint32_t generation =
        (selfIDCountReg & SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;
    const bool error = (selfIDCountReg & SelfIDCountBits::kError) != 0;

    if (quadCount == 0) {
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::EmptyCapture, selfIDCountReg, generation));
    }

    if (error) {
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::ControllerErrorBit, selfIDCountReg, generation));
    }

    if (quadCount > quadCapacity_) {
        ASFW_LOG(Hardware, "Self-ID quadCount=%u exceeds buffer capacity=%zu", quadCount, quadCapacity_);
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::CountOverflow, selfIDCountReg, generation));
    }

    const size_t cappedQuads = std::min<size_t>(quadCount, quadCapacity_);

    FullBarrier();

    const uint64_t addr = map_->GetAddress();
    if (addr == 0) {
        ASFW_LOG(Hardware, "Self-ID map address is NULL - buffer mapping failed");
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::NullMapAddress, selfIDCountReg, generation));
    }

    const auto* base = reinterpret_cast<const uint32_t*>(addr);

    if (cappedQuads > 0) {
        const uint32_t headerQuad = base[0];
        const uint32_t genMem = (headerQuad >> 16) & 0xFF;

        const uint32_t selfIDCountReg2 = hw.Read(Register32::kSelfIDCount);
        const uint32_t generation2 =
            (selfIDCountReg2 & SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;

        if (generation != genMem) {
            ASFW_LOG(Hardware,
                     "Self-ID generation mismatch (buffer vs initial read): buffer=%u register1=%u",
                     genMem, generation);
            return std::unexpected(
                MakeDecodeError(DecodeErrorCode::GenerationMismatch, selfIDCountReg, generation));
        }

        if (generation != generation2) {
            ASFW_LOG(Hardware, "Self-ID generation mismatch (initial vs double-read): register1=%u register2=%u",
                     generation, generation2);
            return std::unexpected(
                MakeDecodeError(DecodeErrorCode::GenerationMismatch, selfIDCountReg, generation));
        }

        ASFW_LOG(Hardware, "Self-ID generation VALIDATED: %u", generation);
    }

    const auto rawQuadlets = std::span<const uint32_t>(base, cappedQuads);
    auto normalized = NormalizeSelfIDQuadlets(rawQuadlets, selfIDCountReg, generation);
    if (!normalized) {
        ASFW_LOG(Hardware, "Self-ID normalization failed: %{public}s at quadlet=%zu",
                 DecodeErrorCodeString(normalized.error().code), normalized.error().quadletIndex);
        return std::unexpected(normalized.error());
    }

    Result result;
    result.generation = generation;
    result.valid = true;
    result.timedOut = false;
    result.crcError = false;
    result.quads = std::move(*normalized);

    SelfIDSequenceEnumerator enumerator;
    if (result.quads.size() > 1) {
        enumerator.cursor = result.quads.data() + 1;
        enumerator.quadlet_count = static_cast<unsigned int>(result.quads.size() - 1);
    } else {
        enumerator.cursor = nullptr;
        enumerator.quadlet_count = 0;
    }

    bool enumeratorError = false;
    while (enumerator.quadlet_count > 0) {
        if (enumerator.cursor == nullptr || !IsSelfIDTag(*enumerator.cursor)) {
            enumeratorError = true;
            break;
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

    if (enumeratorError) {
        return std::unexpected(
            MakeDecodeError(DecodeErrorCode::MalformedSequence, selfIDCountReg, generation));
    }

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
    ASFW_LOG(Hardware, "✅ Self-ID decode valid");
#if ASFW_DEBUG_SELF_ID
    ASFW_LOG_SELF_ID("=== End Self-ID Debug ===");
#endif

    return result;
}

} // namespace ASFW::Driver
