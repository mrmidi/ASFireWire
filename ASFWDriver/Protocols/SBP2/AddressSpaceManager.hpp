#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#endif
#include <DriverKit/IOLib.h>

#include "../../Async/ResponseCode.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Protocols::SBP2 {

class AddressSpaceManager {
public:
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

        const uint64_t start = ComposeAddress(addressHi, addressLo);
        const uint64_t end = start + static_cast<uint64_t>(length);
        if (end < start) {
            return kIOReturnBadArgument;
        }

        IOLockLock(lock_);

        for (const auto& entry : ranges_) {
            if (RangesOverlap(start,
                              static_cast<uint64_t>(length),
                              entry.second.meta.address,
                              static_cast<uint64_t>(entry.second.meta.length))) {
                IOLockUnlock(lock_);
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
            IOLockUnlock(lock_);
            return kr;
        }

        const uint64_t handle = range.meta.handle;
        if (outMeta) {
            *outMeta = range.meta;
        }
        ranges_.emplace(handle, std::move(range));
        *outHandle = handle;

        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }

    kern_return_t DeallocateAddressRange(void* owner, uint64_t handle) {
        if (!lock_ || handle == 0) {
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

        IOLockLock(lock_);
        auto* range = FindRangeByAddressLocked(address, static_cast<uint32_t>(payload.size()));
        if (!range) {
            IOLockUnlock(lock_);
            return Async::ResponseCode::AddressError;
        }

        const uint32_t offset = static_cast<uint32_t>(address - range->meta.address);
        WriteBytesLocked(*range, offset, payload);
        IOLockUnlock(lock_);
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
                CleanupBacking(it->second);
                it = ranges_.erase(it);
            } else {
                ++it;
            }
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
    struct AddressRange {
        AddressRangeMeta meta{};
        void* owner{nullptr};
        std::vector<uint8_t> buffer;

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

    kern_return_t AllocateBacking(AddressRange& range) {
        const std::size_t size = static_cast<std::size_t>(range.meta.length);

        const uint64_t options =
            static_cast<uint64_t>(kIOMemoryDirectionOut) |
            static_cast<uint64_t>(kIOMemoryDirectionIn) |
            static_cast<uint64_t>(kIOMemoryMapCacheModeInhibit);

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
            0,
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

    static void WriteBytesLocked(AddressRange& range,
                                 uint32_t offset,
                                 std::span<const uint8_t> data) {
        std::memcpy(range.buffer.data() + static_cast<std::size_t>(offset),
                    data.data(),
                    data.size());

        if (range.hasBacking && range.mappedBytes) {
            std::memcpy(range.mappedBytes + static_cast<std::size_t>(offset),
                        data.data(),
                        data.size());
            std::atomic_thread_fence(std::memory_order_release);
            SyncRange(range, offset, data.size());
        }
    }

    ASFW::Driver::HardwareInterface* hardware_{nullptr};
    IOLock* lock_{nullptr};
    uint64_t nextHandle_{1};
    std::unordered_map<uint64_t, AddressRange> ranges_;
};

} // namespace ASFW::Protocols::SBP2
