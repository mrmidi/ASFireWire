#pragma once

#include <algorithm>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include <functional>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#endif
#include <DriverKit/IOLib.h>

#include "../../Async/ResponseCode.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Protocols::SBP2 {

#ifdef ASFW_HOST_TEST
#define ASFW_ADDRSPACE_LOG(fmt, ...)
#else
#define ASFW_ADDRSPACE_LOG(fmt, ...) ASFW_LOG_V2(Async, fmt, ##__VA_ARGS__)
#endif

class AddressSpaceManager {
public:
    // Callback invoked when a remote write arrives for a registered range.
    // Parameters: handle, offset within range, payload data.
    using RemoteWriteCallback = std::function<void(uint64_t handle,
                                                   uint32_t offset,
                                                   std::span<const uint8_t> payload)>;
    struct AddressRangeMeta {
        uint64_t handle{0};
        uint64_t address{0};
        uint16_t addressHi{0};
        uint32_t addressLo{0};
        uint32_t length{0};
    };

    struct ReadSlice {
        uint64_t payloadDeviceAddress{0};
        uint32_t payloadLength{0};
    };

    explicit AddressSpaceManager(ASFW::Driver::HardwareInterface* hardware) noexcept
        : hardware_(hardware)
        , lock_(IOLockAlloc()) {}

    ~AddressSpaceManager() {
        ClearAll();
        if (lock_) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
    }

    AddressSpaceManager(const AddressSpaceManager&) = delete;
    AddressSpaceManager& operator=(const AddressSpaceManager&) = delete;

    [[nodiscard]] bool IsReady() const noexcept {
        return lock_ != nullptr;
    }

    kern_return_t AllocateAddressRange(void* owner,
                                       uint16_t addressHi,
                                       uint32_t addressLo,
                                       uint32_t length,
                                       uint64_t* outHandle,
                                       AddressRangeMeta* outMeta = nullptr) {
        if (!lock_ || !outHandle || length == 0) {
            return kIOReturnBadArgument;
        }

        IOLockLock(lock_);
        const kern_return_t kr = AllocateAddressRangeLocked(
            owner, addressHi, addressLo, length, outHandle, outMeta);
        IOLockUnlock(lock_);
        return kr;
    }

    kern_return_t AllocateAddressRangeAuto(void* owner,
                                           uint16_t addressHi,
                                           uint32_t length,
                                           uint64_t* outHandle,
                                           AddressRangeMeta* outMeta = nullptr) {
        if (!lock_ || !outHandle || length == 0) {
            return kIOReturnBadArgument;
        }
        if (addressHi != kAutoAddressHi) {
            return kIOReturnBadArgument;
        }

        const uint64_t windowStart = ComposeAddress(addressHi, kAutoAddressWindowStartLo);
        const uint64_t windowEndExclusive = ComposeAddress(addressHi, kAutoAddressWindowEndLo) + 1ULL;
        const uint64_t windowLength = windowEndExclusive - windowStart;
        if (static_cast<uint64_t>(length) > windowLength) {
            return kIOReturnNoSpace;
        }

        IOLockLock(lock_);

        std::vector<std::pair<uint64_t, uint64_t>> occupied;
        occupied.reserve(ranges_.size());

        for (const auto& entry : ranges_) {
            const auto& meta = entry.second.meta;
            if (meta.addressHi != addressHi) {
                continue;
            }

            const uint64_t rangeStart = meta.address;
            const uint64_t rangeEnd = rangeStart + static_cast<uint64_t>(meta.length);
            if (rangeEnd <= windowStart || rangeStart >= windowEndExclusive) {
                continue;
            }

            occupied.emplace_back(rangeStart, rangeEnd);
        }

        std::sort(occupied.begin(), occupied.end());

        uint64_t candidate = AlignUp(windowStart, kAutoAddressAlignment);
        for (const auto& [rangeStart, rangeEnd] : occupied) {
            if (rangeEnd <= candidate) {
                continue;
            }
            if (CanFitRange(candidate, length, rangeStart)) {
                break;
            }
            candidate = AlignUp(rangeEnd, kAutoAddressAlignment);
        }

        if (!CanFitRange(candidate, length, windowEndExclusive)) {
            IOLockUnlock(lock_);
            return kIOReturnNoSpace;
        }

        const kern_return_t kr = AllocateAddressRangeLocked(
            owner,
            addressHi,
            static_cast<uint32_t>(candidate & 0xFFFF'FFFFULL),
            length,
            outHandle,
            outMeta);
        IOLockUnlock(lock_);
        return kr;
    }

    kern_return_t DeallocateAddressRange(void* owner, uint64_t handle) {
        if (!lock_ || handle == 0) {
            return kIOReturnBadArgument;
        }

        IOLockLock(lock_);
        auto it = ranges_.find(handle);
        if (it == ranges_.end()) {
            ASFW_ADDRSPACE_LOG("AddressSpaceManager[%p] dealloc miss owner=%p handle=0x%llx ranges=%lu",
                               this,
                               owner,
                               static_cast<unsigned long long>(handle),
                               static_cast<unsigned long>(ranges_.size()));
            IOLockUnlock(lock_);
            return kIOReturnNotFound;
        }
        if (it->second.owner != owner) {
            ASFW_ADDRSPACE_LOG("AddressSpaceManager[%p] dealloc denied owner=%p handle=0x%llx actualOwner=%p",
                               this,
                               owner,
                               static_cast<unsigned long long>(handle),
                               it->second.owner);
            IOLockUnlock(lock_);
            return kIOReturnNotPermitted;
        }

        ASFW_ADDRSPACE_LOG("AddressSpaceManager[%p] dealloc owner=%p handle=0x%llx addr=0x%012llx len=%u",
                           this,
                           owner,
                           static_cast<unsigned long long>(handle),
                           static_cast<unsigned long long>(it->second.meta.address),
                           it->second.meta.length);
        CleanupBacking(it->second);
        ranges_.erase(it);
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }

    kern_return_t ReadIncomingData(void* owner,
                                   uint64_t handle,
                                   uint32_t offset,
                                   uint32_t length,
                                   std::vector<uint8_t>* outData) {
        if (!lock_ || !outData) {
            return kIOReturnBadArgument;
        }

        IOLockLock(lock_);
        auto it = ranges_.find(handle);
        if (it == ranges_.end()) {
            IOLockUnlock(lock_);
            return kIOReturnNotFound;
        }
        if (it->second.owner != owner) {
            IOLockUnlock(lock_);
            return kIOReturnNotPermitted;
        }

        const auto& range = it->second;
        if (!WithinRange(range, offset, length)) {
            IOLockUnlock(lock_);
            return kIOReturnNoSpace;
        }

        outData->assign(range.buffer.begin() + static_cast<std::size_t>(offset),
                        range.buffer.begin() + static_cast<std::size_t>(offset + length));
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }

    kern_return_t WriteLocalData(void* owner,
                                 uint64_t handle,
                                 uint32_t offset,
                                 std::span<const uint8_t> data) {
        if (!lock_) {
            return kIOReturnBadArgument;
        }

        IOLockLock(lock_);
        auto it = ranges_.find(handle);
        if (it == ranges_.end()) {
            IOLockUnlock(lock_);
            return kIOReturnNotFound;
        }
        if (it->second.owner != owner) {
            IOLockUnlock(lock_);
            return kIOReturnNotPermitted;
        }

        if (!WithinRange(it->second, offset, static_cast<uint32_t>(data.size()))) {
            IOLockUnlock(lock_);
            return kIOReturnNoSpace;
        }

        WriteBytesLocked(it->second, offset, data);
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }

    Async::ResponseCode ApplyRemoteWrite(uint64_t address,
                                         std::span<const uint8_t> payload) {
        if (!lock_ || payload.empty()) {
            return Async::ResponseCode::AddressError;
        }

        RemoteWriteCallback callback;
        uint64_t handle = 0;
        uint32_t offset = 0;

        {
            IOLockLock(lock_);
            auto* range = FindRangeByAddressLocked(address, static_cast<uint32_t>(payload.size()));
            if (!range) {
                LogRangesLocked("write miss", address, static_cast<uint32_t>(payload.size()));
                IOLockUnlock(lock_);
                return Async::ResponseCode::AddressError;
            }

            offset = static_cast<uint32_t>(address - range->meta.address);
            ASFW_ADDRSPACE_LOG(
                "AddressSpaceManager[%p] remote write addr=0x%012llx len=%zu src=%p "
                "handle=0x%llx rangeAddr=0x%012llx off=%u buf=%p mapped=%p backing=%u",
                this,
                static_cast<unsigned long long>(address),
                payload.size(),
                payload.data(),
                static_cast<unsigned long long>(range->meta.handle),
                static_cast<unsigned long long>(range->meta.address),
                offset,
                range->buffer.data(),
                range->mappedBytes,
                range->hasBacking ? 1u : 0u);
            WriteBytesLocked(*range, offset, payload);
            callback = range->onRemoteWrite;
            handle = range->meta.handle;
            IOLockUnlock(lock_);
        }

        // Fire callback outside lock to avoid deadlock.
        if (callback) {
            callback(handle, offset, payload);
        }

        return Async::ResponseCode::Complete;
    }

    Async::ResponseCode ResolveReadSlice(uint64_t address,
                                         uint32_t length,
                                         ReadSlice* outSlice) {
        if (!lock_ || !outSlice || length == 0) {
            return Async::ResponseCode::AddressError;
        }

        IOLockLock(lock_);
        auto* range = FindRangeByAddressLocked(address, length);
        if (!range) {
            LogRangesLocked("resolve miss", address, length);
            IOLockUnlock(lock_);
            return Async::ResponseCode::AddressError;
        }

        if (!range->hasBacking || range->deviceAddress == 0) {
            IOLockUnlock(lock_);
            return Async::ResponseCode::DataError;
        }

        const uint64_t offset = address - range->meta.address;
        const uint64_t payloadAddress = range->deviceAddress + offset;
        if (payloadAddress > 0xFFFF'FFFFULL) {
            IOLockUnlock(lock_);
            return Async::ResponseCode::DataError;
        }

        outSlice->payloadDeviceAddress = payloadAddress;
        outSlice->payloadLength = length;

        IOLockUnlock(lock_);
        return Async::ResponseCode::Complete;
    }

    Async::ResponseCode ReadQuadlet(uint64_t address, uint32_t* outValue) {
        if (!lock_ || !outValue) {
            return Async::ResponseCode::AddressError;
        }

        IOLockLock(lock_);
        auto* range = FindRangeByAddressLocked(address, sizeof(uint32_t));
        if (!range) {
            IOLockUnlock(lock_);
            return Async::ResponseCode::AddressError;
        }

        const uint32_t offset = static_cast<uint32_t>(address - range->meta.address);
        std::memcpy(outValue,
                    range->buffer.data() + static_cast<std::size_t>(offset),
                    sizeof(uint32_t));

        IOLockUnlock(lock_);
        return Async::ResponseCode::Complete;
    }

    void ReleaseOwner(void* owner) {
        if (!lock_) {
            return;
        }

        IOLockLock(lock_);
        for (auto it = ranges_.begin(); it != ranges_.end();) {
            if (it->second.owner == owner) {
                ASFW_ADDRSPACE_LOG(
                    "AddressSpaceManager[%p] release owner=%p handle=0x%llx addr=0x%012llx len=%u",
                    this,
                    owner,
                    static_cast<unsigned long long>(it->second.meta.handle),
                    static_cast<unsigned long long>(it->second.meta.address),
                    it->second.meta.length);
                CleanupBacking(it->second);
                it = ranges_.erase(it);
            } else {
                ++it;
            }
        }
        IOLockUnlock(lock_);
    }

    // Register a callback to fire when a remote write arrives for the given handle.
    // Must be called after AllocateAddressRange. Replaces any previous callback.
    void SetRemoteWriteCallback(uint64_t handle, RemoteWriteCallback callback) {
        if (!lock_ || handle == 0) {
            return;
        }

        IOLockLock(lock_);
        auto it = ranges_.find(handle);
        if (it != ranges_.end()) {
            it->second.onRemoteWrite = std::move(callback);
        }
        IOLockUnlock(lock_);
    }

    void ClearAll() {
        if (!lock_) {
            return;
        }

        IOLockLock(lock_);
        for (auto& entry : ranges_) {
            CleanupBacking(entry.second);
        }
        ranges_.clear();
        IOLockUnlock(lock_);
    }

private:
    static constexpr uint16_t kAutoAddressHi = 0xFFFFu;
    static constexpr uint32_t kAutoAddressWindowStartLo = 0x0010'0000u;
    static constexpr uint32_t kAutoAddressWindowEndLo = 0x0FFF'FFFFu;
    static constexpr uint64_t kAutoAddressAlignment = 8ULL;

    struct AddressRange {
        AddressRangeMeta meta{};
        void* owner{nullptr};
        std::vector<uint8_t> buffer;
        RemoteWriteCallback onRemoteWrite;

        OSSharedPtr<IOBufferMemoryDescriptor> descriptor{};
        OSSharedPtr<IODMACommand> dmaCommand{};
        IOMemoryMap* mapping{nullptr};
        uint8_t* mappedBytes{nullptr};
        uint64_t deviceAddress{0};
        bool hasBacking{false};
    };

    static uint64_t ComposeAddress(uint16_t hi, uint32_t lo) {
        return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
    }

    static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
        const uint64_t mask = alignment - 1ULL;
        return (value + mask) & ~mask;
    }

    static bool CanFitRange(uint64_t start, uint32_t length, uint64_t limitExclusive) {
        const uint64_t end = start + static_cast<uint64_t>(length);
        return end >= start && end <= limitExclusive;
    }

    static bool RangesOverlap(uint64_t leftStart,
                              uint64_t leftLength,
                              uint64_t rightStart,
                              uint64_t rightLength) {
        const uint64_t leftEnd = leftStart + leftLength;
        const uint64_t rightEnd = rightStart + rightLength;
        return leftStart < rightEnd && rightStart < leftEnd;
    }

    static bool WithinRange(const AddressRange& range, uint32_t offset, uint32_t length) {
        const uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(length);
        return end <= static_cast<uint64_t>(range.meta.length);
    }

    AddressRange* FindRangeByAddressLocked(uint64_t address, uint32_t length) {
        const uint64_t end = address + static_cast<uint64_t>(length);
        if (end < address) {
            return nullptr;
        }

        for (auto& entry : ranges_) {
            auto& range = entry.second;
            const uint64_t rangeStart = range.meta.address;
            const uint64_t rangeEnd = rangeStart + static_cast<uint64_t>(range.meta.length);
            if (rangeEnd < rangeStart) {
                continue;
            }

            if (address >= rangeStart && end <= rangeEnd) {
                return &range;
            }
        }

        return nullptr;
    }

    void LogRangesLocked(const char* reason, uint64_t address, uint32_t length) {
        ASFW_ADDRSPACE_LOG("AddressSpaceManager[%p] %s addr=0x%012llx len=%u ranges=%lu",
                           this,
                           reason,
                           static_cast<unsigned long long>(address),
                           length,
                           static_cast<unsigned long>(ranges_.size()));
        for (const auto& entry : ranges_) {
            const auto& range = entry.second;
            ASFW_ADDRSPACE_LOG(
                "AddressSpaceManager[%p] range handle=0x%llx owner=%p addr=0x%012llx len=%u backing=%u dma=0x%08x",
                this,
                static_cast<unsigned long long>(range.meta.handle),
                range.owner,
                static_cast<unsigned long long>(range.meta.address),
                range.meta.length,
                range.hasBacking ? 1u : 0u,
                static_cast<unsigned>(range.deviceAddress));
        }
    }

    kern_return_t AllocateAddressRangeLocked(void* owner,
                                             uint16_t addressHi,
                                             uint32_t addressLo,
                                             uint32_t length,
                                             uint64_t* outHandle,
                                             AddressRangeMeta* outMeta) {
        const uint64_t start = ComposeAddress(addressHi, addressLo);
        const uint64_t end = start + static_cast<uint64_t>(length);
        if (end < start) {
            return kIOReturnBadArgument;
        }

        for (const auto& entry : ranges_) {
            if (RangesOverlap(start,
                              static_cast<uint64_t>(length),
                              entry.second.meta.address,
                              static_cast<uint64_t>(entry.second.meta.length))) {
                return kIOReturnNoSpace;
            }
        }

        AddressRange range{};
        range.owner = owner;
        range.meta.handle = nextHandle_++;
        range.meta.address = start;
        range.meta.addressHi = addressHi;
        range.meta.addressLo = addressLo;
        range.meta.length = length;
        range.buffer.resize(length, 0);

        const kern_return_t kr = AllocateBacking(range);
        if (kr != kIOReturnSuccess) {
            return kr;
        }

        const uint64_t handle = range.meta.handle;
        if (outMeta) {
            *outMeta = range.meta;
        }
        ranges_.emplace(handle, std::move(range));
        ASFW_ADDRSPACE_LOG("AddressSpaceManager[%p] alloc owner=%p handle=0x%llx addr=0x%012llx len=%u ranges=%lu",
                           this,
                           owner,
                           static_cast<unsigned long long>(handle),
                           static_cast<unsigned long long>(start),
                           length,
                           static_cast<unsigned long>(ranges_.size()));
        *outHandle = handle;
        return kIOReturnSuccess;
    }

    kern_return_t AllocateBacking(AddressRange& range) {
        const std::size_t size = static_cast<std::size_t>(range.meta.length);

        // IOBufferMemoryDescriptor::Create expects memory-direction options only.
        // Cache policy is set at CreateMapping time, not in allocation options.
        const uint64_t options = static_cast<uint64_t>(kIOMemoryDirectionInOut);

        std::optional<ASFW::Driver::HardwareInterface::DMABuffer> dma;
        if (hardware_) {
            dma = hardware_->AllocateDMA(size, options, 16);
        }

        if (!dma.has_value()) {
#ifdef ASFW_HOST_TEST
            range.hasBacking = false;
            return kIOReturnSuccess;
#else
            ASFW_LOG(UserClient,
                     "AddressSpaceManager: DMA allocation failed for len=%u",
                     range.meta.length);
            return kIOReturnNoMemory;
#endif
        }

        IOMemoryMap* mapping = nullptr;
        const kern_return_t kr = dma->descriptor->CreateMapping(
            kIOMemoryMapCacheModeInhibit,
            0,
            0,
            size,
            0,
            &mapping);
        if (kr != kIOReturnSuccess || !mapping) {
            if (dma->dmaCommand) {
                dma->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
                dma->dmaCommand.reset();
            }
            return kr != kIOReturnSuccess ? kr : kIOReturnError;
        }

        auto* mapped = reinterpret_cast<uint8_t*>(mapping->GetAddress());
        if (!mapped) {
            mapping->release();
            if (dma->dmaCommand) {
                dma->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
                dma->dmaCommand.reset();
            }
            return kIOReturnNoMemory;
        }

        std::memset(mapped, 0, size);
        OSSynchronizeIO();

        range.descriptor = std::move(dma->descriptor);
        range.dmaCommand = std::move(dma->dmaCommand);
        range.mapping = mapping;
        range.mappedBytes = mapped;
        range.deviceAddress = dma->deviceAddress;
        range.hasBacking = true;

        return kIOReturnSuccess;
    }

    static void CleanupBacking(AddressRange& range) {
        if (range.mapping) {
            range.mapping->release();
            range.mapping = nullptr;
        }

        if (range.dmaCommand) {
            range.dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            range.dmaCommand.reset();
        }

        range.descriptor.reset();
        range.mappedBytes = nullptr;
        range.deviceAddress = 0;
        range.hasBacking = false;
    }

    static void SyncRange(AddressRange& range, uint64_t offset, uint64_t length) {
        if (!range.hasBacking) {
            return;
        }

#if defined(IODMACommand_Synchronize_ID)
        if (range.dmaCommand) {
            const kern_return_t syncKr = range.dmaCommand->Synchronize(
                0,
                offset,
                length);
            if (syncKr != kIOReturnSuccess) {
                OSSynchronizeIO();
            }
            return;
        }
#endif
        OSSynchronizeIO();
    }

    static void CopyPayloadBytes(uint8_t* destination,
                                 std::span<const uint8_t> source) {
        if (!destination || source.empty()) {
            return;
        }

        const auto sourceAddr = reinterpret_cast<uintptr_t>(source.data());
        const auto destAddr = reinterpret_cast<uintptr_t>(destination);
        const bool quadletAligned = ((sourceAddr | destAddr) & 0x3u) == 0;

        std::size_t index = 0;
        if (quadletAligned) {
            for (; index + sizeof(uint32_t) <= source.size(); index += sizeof(uint32_t)) {
                uint32_t quadlet = 0;
                __builtin_memcpy(&quadlet, source.data() + index, sizeof(uint32_t));
                __builtin_memcpy(destination + index, &quadlet, sizeof(uint32_t));
            }
        }

        for (; index < source.size(); ++index) {
            destination[index] = source[index];
        }
    }

    static void WriteBytesLocked(AddressRange& range,
                                 uint32_t offset,
                                 std::span<const uint8_t> data) {
        ASFW_ADDRSPACE_LOG(
            "AddressSpaceManager write handle=0x%llx off=%u len=%zu src=%p buf=%p mapped=%p "
            "srcAlign=%zu bufAlign=%zu mappedAlign=%zu",
            static_cast<unsigned long long>(range.meta.handle),
            offset,
            data.size(),
            data.data(),
            range.buffer.data(),
            range.mappedBytes,
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(data.data()) & 0x7ULL),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(range.buffer.data()) & 0x7ULL),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(range.mappedBytes) & 0x7ULL));
        CopyPayloadBytes(range.buffer.data() + static_cast<std::size_t>(offset), data);

        if (range.hasBacking && range.mappedBytes) {
            CopyPayloadBytes(range.mappedBytes + static_cast<std::size_t>(offset), data);
            std::atomic_thread_fence(std::memory_order_release);
            SyncRange(range, offset, data.size());
        }
    }

    ASFW::Driver::HardwareInterface* hardware_{nullptr};
    IOLock* lock_{nullptr};
    uint64_t nextHandle_{1};
    std::unordered_map<uint64_t, AddressRange> ranges_;
};

#undef ASFW_ADDRSPACE_LOG

} // namespace ASFW::Protocols::SBP2
