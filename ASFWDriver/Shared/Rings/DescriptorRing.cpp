#include "DescriptorRing.hpp"
#include "RingHelpers.hpp"

#include <cstring>

namespace ASFW::Shared {

bool DescriptorRing::Initialize(std::span<HW::OHCIDescriptor> descriptors) noexcept {
    if (descriptors.empty()) return false;
    const auto virtAddr = reinterpret_cast<uintptr_t>(descriptors.data());
    if ((virtAddr & 0xF) != 0) return false;
    storage_ = descriptors;
    capacity_ = descriptors.size();
    std::memset(descriptors.data(), 0, descriptors.size_bytes());
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    prev_last_blocks_.store(0, std::memory_order_relaxed);
    return true;
}

bool DescriptorRing::Finalize(uint64_t descriptorsIOVABase) noexcept {
    if (storage_.empty() || capacity_ == 0) return false;
    if ((descriptorsIOVABase & 0xF) != 0) return false;
    descIOVABase_ = descriptorsIOVABase;
    return true;
}

uint32_t DescriptorRing::CommandPtrWordTo(const HW::OHCIDescriptor* target, uint8_t zBlocks) const noexcept {
    if (storage_.empty() || target == nullptr || descIOVABase_ == 0) return 0;
    const auto base = storage_.data();
    const ptrdiff_t idx = target - base;
    if (idx < 0) return 0;
    const size_t idxu = static_cast<size_t>(idx);
    if (idxu >= storage_.size()) return 0;
    const uint64_t addr = descIOVABase_ + static_cast<uint64_t>(idxu) * sizeof(HW::OHCIDescriptor);
    if (addr == 0 || addr > 0xFFFFFFFFull) return 0;
    const uint32_t z = static_cast<uint32_t>(zBlocks & 0xF);
    return (static_cast<uint32_t>(addr) & 0xFFFFFFF0u) | z;
}

uint32_t DescriptorRing::CommandPtrWordFromIOVA(uint32_t iova32, uint8_t zBlocks) const noexcept {
    if (storage_.empty() || descIOVABase_ == 0) return 0;
    if ((iova32 & 0xFULL) != 0) return 0;
    const uint64_t iova64 = static_cast<uint64_t>(iova32);
    if (iova64 < descIOVABase_) return 0;
    const uint64_t offset = iova64 - descIOVABase_;
    if ((offset % sizeof(HW::OHCIDescriptor)) != 0) return 0;
    const uint64_t idxu = offset / sizeof(HW::OHCIDescriptor);
    if (idxu >= storage_.size()) return 0;
    const uint32_t z = static_cast<uint32_t>(zBlocks & 0xF);
    return (iova32 & 0xFFFFFFF0u) | z;
}

bool DescriptorRing::IsFull() const noexcept {
    return RingHelpers::IsFullAtomic(head_, tail_, capacity_);
}

size_t DescriptorRing::Size() const noexcept {
    return RingHelpers::CountAtomic(head_, tail_, capacity_);
}

HW::OHCIDescriptor* DescriptorRing::At(size_t index) noexcept {
    if (index >= capacity_) return nullptr;
    return &storage_[index];
}

const HW::OHCIDescriptor* DescriptorRing::At(size_t index) const noexcept {
    if (index >= capacity_) return nullptr;
    return &storage_[index];
}

bool DescriptorRing::LocatePreviousLast(size_t tailIndex, HW::OHCIDescriptor*& outDescriptor, size_t& outIndex, uint8_t& outBlocks) noexcept {
    outDescriptor = nullptr; outIndex = 0; outBlocks = 0;
    if (capacity_ == 0) return false;
    const uint8_t prevBlocks = PrevLastBlocks();
    if (prevBlocks == 0) return false;
    if (prevBlocks != 2 && prevBlocks != 3) return false;
    const size_t capacity = capacity_;
    const size_t prevStart = (tailIndex + capacity - prevBlocks) % capacity;
    const size_t prevTailOffset = (prevBlocks == 2) ? 0 : (prevBlocks - 1);
    size_t index = (prevStart + prevTailOffset) % capacity;
    HW::OHCIDescriptor* descriptor = At(index);
    if (!descriptor) return false;
    if (prevBlocks == 2 && !HW::IsImmediate(*descriptor)) {
        const size_t headerIndex = (index + capacity - 1) % capacity;
        descriptor = At(headerIndex);
        if (!descriptor || !HW::IsImmediate(*descriptor)) return false;
        index = headerIndex;
    }
    outDescriptor = descriptor; outIndex = index; outBlocks = prevBlocks; return true;
}

bool DescriptorRing::LocatePreviousLast(size_t tailIndex, const HW::OHCIDescriptor*& outDescriptor, size_t& outIndex, uint8_t& outBlocks) const noexcept {
    HW::OHCIDescriptor* mutableDescriptor = nullptr;
    const bool found = const_cast<DescriptorRing*>(this)->LocatePreviousLast(tailIndex, mutableDescriptor, outIndex, outBlocks);
    outDescriptor = mutableDescriptor; return found;
}

} // namespace ASFW::Shared
