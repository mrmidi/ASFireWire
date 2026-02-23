// IsochRxDmaRing.cpp

#include "IsochRxDmaRing.hpp"

#include "../../Hardware/OHCIConstants.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Logging/Logging.hpp"

#include <span>

namespace ASFW::Isoch::Rx {

kern_return_t IsochRxDmaRing::SetupRings(Memory::IIsochDMAMemory& dma,
                                        size_t numDescriptors,
                                        size_t maxPacketSizeBytes) noexcept {
    if (numDescriptors == 0 || maxPacketSizeBytes == 0) {
        return kIOReturnBadArgument;
    }
    if (maxPacketSizeBytes > 0xFFFFu) {
        return kIOReturnBadArgument;
    }

    // Allocate-once policy:
    // IsochService keeps the IR context (and its dedicated DMA slabs) alive across start/stop.
    // Re-allocating on every Configure() will exhaust the bump-pointer allocator and fail on
    // the second StartDevice (seen as "AllocateRegion would overflow ... cursor=2097152").
    //
    // If we already have a ring, just reinitialize the descriptor program and status words.
    if (bufferRing_.Capacity() != 0) {
        if (bufferRing_.Capacity() != numDescriptors || bufferRing_.BufferSize() != maxPacketSizeBytes) {
            ASFW_LOG(Isoch,
                     "IR: SetupRings reconfigure unsupported (have cap=%zu maxPkt=%zu, want cap=%zu maxPkt=%zu)",
                     bufferRing_.Capacity(),
                     bufferRing_.BufferSize(),
                     numDescriptors,
                     maxPacketSizeBytes);
            return kIOReturnUnsupported;
        }

        bufferRing_.BindDma(&dma);

        const uint32_t count = static_cast<uint32_t>(bufferRing_.Capacity());
        const uint16_t reqCount = static_cast<uint16_t>(maxPacketSizeBytes);

        for (uint32_t i = 0; i < count; ++i) {
            auto* desc = bufferRing_.GetDescriptor(i);
            if (!desc) {
                return kIOReturnInternalError;
            }

            const uint8_t interruptBits =
                (i % 8 == 7) ? Async::HW::OHCIDescriptor::kIntAlways : Async::HW::OHCIDescriptor::kIntNever;

            uint32_t control = Async::HW::OHCIDescriptor::BuildControl(
                reqCount,
                Async::HW::OHCIDescriptor::kCmdInputLast,
                Async::HW::OHCIDescriptor::kKeyStandard,
                interruptBits,
                Async::HW::OHCIDescriptor::kBranchAlways);
            control |= (1u << (Async::HW::OHCIDescriptor::kStatusShift + Async::HW::OHCIDescriptor::kControlHighShift));
            desc->control = control;

            const uint64_t dataIOVA = bufferRing_.GetElementIOVA(i);
            if (dataIOVA == 0 || dataIOVA > 0xFFFFFFFFULL) {
                return kIOReturnInternalError;
            }
            desc->dataAddress = static_cast<uint32_t>(dataIOVA);

            const uint64_t nextIOVA = bufferRing_.GetDescriptorIOVA((i + 1) % count);
            if (nextIOVA == 0 || nextIOVA > 0xFFFFFFFFULL || (nextIOVA & 0xF) != 0) {
                return kIOReturnInternalError;
            }

            desc->branchWord = Async::HW::MakeBranchWordAR(static_cast<uint32_t>(nextIOVA), 1);
            Async::HW::AR_init_status(*desc, reqCount);
        }

        bufferRing_.PublishAllDescriptorsOnce();

        maxPacketSizeBytes_ = maxPacketSizeBytes;
        lastProcessedIndex_ = 0;

        return kIOReturnSuccess;
    }

    const size_t descriptorsSize = numDescriptors * sizeof(Async::HW::OHCIDescriptor);
    const size_t buffersSize = numDescriptors * maxPacketSizeBytes;

    auto descRegion = dma.AllocateDescriptor(descriptorsSize);
    if (!descRegion) {
        return kIOReturnNoMemory;
    }

    auto bufRegion = dma.AllocatePayloadBuffer(buffersSize);
    if (!bufRegion) {
        return kIOReturnNoMemory;
    }

    auto descSpan = std::span<Async::HW::OHCIDescriptor>(
        reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase),
        numDescriptors);
    auto bufSpan = std::span<uint8_t>(bufRegion->virtualBase, buffersSize);

    if (!bufferRing_.Initialize(descSpan, bufSpan, numDescriptors, maxPacketSizeBytes)) {
        return kIOReturnInternalError;
    }

    bufferRing_.BindDma(&dma);
    if (!bufferRing_.Finalize(descRegion->deviceBase, bufRegion->deviceBase)) {
        return kIOReturnInternalError;
    }

    // Program initial descriptor ring.
    const uint32_t count = static_cast<uint32_t>(bufferRing_.Capacity());
    const uint16_t reqCount = static_cast<uint16_t>(maxPacketSizeBytes);

    for (uint32_t i = 0; i < count; ++i) {
        auto* desc = bufferRing_.GetDescriptor(i);
        if (!desc) {
            return kIOReturnInternalError;
        }

        const uint8_t interruptBits =
            (i % 8 == 7) ? Async::HW::OHCIDescriptor::kIntAlways : Async::HW::OHCIDescriptor::kIntNever;

        uint32_t control = Async::HW::OHCIDescriptor::BuildControl(
            reqCount,
            Async::HW::OHCIDescriptor::kCmdInputLast,
            Async::HW::OHCIDescriptor::kKeyStandard,
            interruptBits,
            Async::HW::OHCIDescriptor::kBranchAlways);
        control |= (1u << (Async::HW::OHCIDescriptor::kStatusShift + Async::HW::OHCIDescriptor::kControlHighShift));
        desc->control = control;

        const uint64_t dataIOVA = bufferRing_.GetElementIOVA(i);
        if (dataIOVA == 0 || dataIOVA > 0xFFFFFFFFULL) {
            return kIOReturnInternalError;
        }
        desc->dataAddress = static_cast<uint32_t>(dataIOVA);

        const uint64_t nextIOVA = bufferRing_.GetDescriptorIOVA((i + 1) % count);
        if (nextIOVA == 0 || nextIOVA > 0xFFFFFFFFULL || (nextIOVA & 0xF) != 0) {
            return kIOReturnInternalError;
        }

        desc->branchWord = Async::HW::MakeBranchWordAR(static_cast<uint32_t>(nextIOVA), 1);
        Async::HW::AR_init_status(*desc, reqCount);
    }

    bufferRing_.PublishAllDescriptorsOnce();

    maxPacketSizeBytes_ = maxPacketSizeBytes;
    lastProcessedIndex_ = 0;

    return kIOReturnSuccess;
}

uint32_t IsochRxDmaRing::Descriptor0IOVA() const noexcept {
    const uint64_t iova = bufferRing_.GetDescriptorIOVA(0);
    if (iova == 0 || iova > 0xFFFFFFFFULL) {
        return 0;
    }
    return static_cast<uint32_t>(iova);
}

uint32_t IsochRxDmaRing::InitialCommandPtrWord() const noexcept {
    const uint32_t base = Descriptor0IOVA();
    if (base == 0 || (base & 0xF) != 0) {
        return 0;
    }
    return base | 1u; // Z=1 (fetch 1 descriptor)
}

} // namespace ASFW::Isoch::Rx
