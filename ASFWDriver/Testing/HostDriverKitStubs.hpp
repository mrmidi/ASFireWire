#pragma once

#ifdef ASFW_HOST_TEST

#include <mach/kern_return.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#ifndef OSSwapBigToHostInt32
#define OSSwapBigToHostInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
#endif
#ifndef OSSwapHostToBigInt32
#define OSSwapHostToBigInt32(x) __builtin_bswap32(static_cast<uint32_t>(x))
#endif
#else
#ifndef OSSwapBigToHostInt32
#define OSSwapBigToHostInt32(x) static_cast<uint32_t>(x)
#endif
#ifndef OSSwapHostToBigInt32
#define OSSwapHostToBigInt32(x) static_cast<uint32_t>(x)
#endif
#endif

struct IOAddressSegment {
    uint64_t address{0};
    uint64_t length{0};
};

// Forward declare for Create
class IOBufferMemoryDescriptor;

class IOService {};

namespace ASFW::Testing {

inline uint64_t DefaultHostMonotonicNow() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::function<uint64_t()>& HostMonotonicClockOverride() {
    static std::function<uint64_t()> clockOverride;
    return clockOverride;
}

inline uint64_t HostMonotonicNow() {
    auto& clockOverride = HostMonotonicClockOverride();
    if (clockOverride) {
        return clockOverride();
    }
    return DefaultHostMonotonicNow();
}

inline void SetHostMonotonicClockForTesting(std::function<uint64_t()> provider) {
    HostMonotonicClockOverride() = std::move(provider);
}

inline void ResetHostMonotonicClockForTesting() {
    HostMonotonicClockOverride() = {};
}

} // namespace ASFW::Testing

class OSObject {
public:
    virtual ~OSObject() = default;
    virtual bool init() { return true; }
    virtual void free() { delete this; }
    void retain() {}
    void release() {}
};

class OSAction : public OSObject {};

class IODispatchQueue : public OSObject {
public:
    struct PendingWorkItem {
        uint64_t dueNs{0};
        std::function<void()> work;
    };

    static kern_return_t Create(const char*, uint64_t, uint64_t, IODispatchQueue**) {
        return kIOReturnUnsupported;
    }

    void DispatchAsync(const std::function<void()>& work) {
        if (!work) {
            return;
        }

        if (manualDispatchForTesting_) {
            EnqueueForTesting(ASFW::Testing::HostMonotonicNow(), work);
            return;
        }

        work();
    }

    void DispatchAsyncAfter(uint64_t delayNs, const std::function<void()>& work) {
        if (!work) {
            return;
        }

        if (manualDispatchForTesting_) {
            EnqueueForTesting(ASFW::Testing::HostMonotonicNow() + delayNs, work);
            return;
        }

        if (delayNs > 0U) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(delayNs));
        }
        work();
    }

    void DispatchSync(const std::function<void()>& work) {
        if (work) {
            work();
        }
    }

    void SetManualDispatchForTesting(bool manual) {
        manualDispatchForTesting_ = manual;
    }

    [[nodiscard]] bool UsesManualDispatchForTesting() const {
        return manualDispatchForTesting_;
    }

    [[nodiscard]] size_t PendingTaskCountForTesting() const {
        std::scoped_lock lock(pendingLock_);
        return pending_.size();
    }

    size_t DrainReadyForTesting() {
        if (!manualDispatchForTesting_) {
            return 0;
        }

        size_t drained = 0;
        while (true) {
            std::function<void()> work;
            {
                std::scoped_lock lock(pendingLock_);
                const uint64_t nowNs = ASFW::Testing::HostMonotonicNow();
                const auto it = std::find_if(
                    pending_.begin(), pending_.end(),
                    [nowNs](const PendingWorkItem& item) { return item.dueNs <= nowNs; });
                if (it == pending_.end()) {
                    break;
                }
                work = std::move(it->work);
                pending_.erase(it);
            }

            if (work) {
                work();
                ++drained;
            }
        }

        return drained;
    }

    size_t DrainAllForTesting() {
        if (!manualDispatchForTesting_) {
            return 0;
        }

        size_t drained = 0;
        while (true) {
            std::function<void()> work;
            {
                std::scoped_lock lock(pendingLock_);
                if (pending_.empty()) {
                    break;
                }
                work = std::move(pending_.front().work);
                pending_.pop_front();
            }

            if (work) {
                work();
                ++drained;
            }
        }

        return drained;
    }

private:
    void EnqueueForTesting(uint64_t dueNs, const std::function<void()>& work) {
        std::scoped_lock lock(pendingLock_);
        pending_.push_back(PendingWorkItem{dueNs, work});
    }

    mutable std::mutex pendingLock_;
    std::deque<PendingWorkItem> pending_;
    bool manualDispatchForTesting_{false};
};

using IODispatchQueueName = const char*;

class IOInterruptDispatchSource : public OSObject {
public:
    static kern_return_t Create(IOService*, uint32_t, IODispatchQueue*, IOInterruptDispatchSource**) {
        return kIOReturnUnsupported;
    }

    kern_return_t SetHandler(OSAction*) { return kIOReturnUnsupported; }
    kern_return_t SetEnableWithCompletion(bool, void*) { return kIOReturnUnsupported; }
};

class IOTimerDispatchSource : public OSObject {
public:
    static kern_return_t Create(IOService*, uint64_t, IOTimerDispatchSource**) {
        return kIOReturnUnsupported;
    }

    kern_return_t SetTimeout(uint64_t, uint64_t, void*) { return kIOReturnUnsupported; }
    kern_return_t Cancel(void*) { return kIOReturnUnsupported; }
};

class IODataQueueDispatchSource : public OSObject {
public:
    static kern_return_t Create(uint64_t, IODispatchQueue*, IODataQueueDispatchSource**) {
        return kIOReturnUnsupported;
    }

    kern_return_t Enqueue(unsigned int, void (^)(void*, size_t)) {
        return kIOReturnUnsupported;
    }
    
    kern_return_t SetEnable(bool) { return kIOReturnUnsupported; }
    kern_return_t Cancel(void*) { return kIOReturnUnsupported; }
};

class IOPCIDevice : public OSObject {
public:
    kern_return_t Open(IOService*) { return kIOReturnUnsupported; }
    void Close(IOService*) {}
    kern_return_t GetBARInfo(uint8_t, uint8_t*, uint64_t*, uint8_t*) { return kIOReturnUnsupported; }
    void MemoryRead32(uint8_t, uint64_t, uint32_t*) {}
    void MemoryWrite32(uint8_t, uint64_t, uint32_t) {}
};

class IOMemoryMap : public OSObject {
    uint64_t address_{0};
    uint64_t length_{0};
public:
    // Helper to set backing store
    void SetMockData(uint64_t addr, uint64_t len) { address_ = addr; length_ = len; }

    uint64_t GetAddress() const { return address_; }
    uint64_t GetLength() const { return length_; }
};

class IOBufferMemoryDescriptor : public OSObject {
    void* buffer_{nullptr};
    uint64_t length_{0};
public:
    virtual void free() override {
        if (buffer_) ::free(buffer_);
        OSObject::free();
    }

    static kern_return_t Create(uint64_t options, uint64_t length, uint64_t alignment, IOBufferMemoryDescriptor** descriptor) {
        if (!descriptor) return kIOReturnBadArgument;
        auto* desc = new IOBufferMemoryDescriptor();
        // Allocate with alignment if possible, or just malloc
        // For host tests, posix_memalign is good
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment > 0 ? alignment : 16, length) != 0) {
            delete desc;
            return kIOReturnNoMemory;
        }
        desc->buffer_ = ptr;
        desc->length_ = length;
        *descriptor = desc;
        return kIOReturnSuccess;
    }

    kern_return_t GetAddressRange(IOAddressSegment* range) {
        if (!range) return kIOReturnBadArgument;
        range->address = reinterpret_cast<uint64_t>(buffer_);
        range->length = length_;
        return kIOReturnSuccess;
    }
    
    kern_return_t SetLength(uint64_t len) {
        if (len > length_) return kIOReturnNoSpace;
        // Don't actually realloc, just track 'length' if needed, but for now we trust the alloc size
        return kIOReturnSuccess;
    }

    kern_return_t CreateMapping(uint64_t options, uint64_t address, uint64_t offset, uint64_t length, uint64_t alignment, IOMemoryMap** map) {
        if (!map) return kIOReturnBadArgument;
        auto* m = new IOMemoryMap();
        // In stub, buffer_ is the pointer. 'address' arg to CreateMapping is usually 0 (offset in descriptor).
        // The mapping should reflect descriptor's buffer + offset.
        // We can just reuse buffer_ pointer as the "virtual address".
        uint64_t base = reinterpret_cast<uint64_t>(buffer_) + offset;
        m->SetMockData(base, length);
        *map = m;
        return kIOReturnSuccess; 
    }
};

class IODMACommand : public OSObject {
public:
    static kern_return_t Create(IOService*, uint64_t, void*, IODMACommand**) {
        return kIOReturnUnsupported;
    }
    void FullBarrier() {}
    kern_return_t CompleteDMA(uint64_t options) { return kIOReturnSuccess; }
    kern_return_t PrepareForDMA(uint64_t options, IOBufferMemoryDescriptor* buffer, uint64_t offset, uint64_t length, uint64_t* flags, uint32_t* segments, IOAddressSegment* segmentOut) {
        if (!segmentOut) return kIOReturnBadArgument;
        IOAddressSegment seg;
        buffer->GetAddressRange(&seg);
        static std::atomic<uint32_t> sMockIOVA{0x10000000u};
        const uint32_t size = static_cast<uint32_t>(length > 0 ? length : seg.length);
        const uint32_t next = sMockIOVA.fetch_add(size + 0x1000u, std::memory_order_relaxed);
        segmentOut[0].address = next;
        segmentOut[0].length = seg.length;
        *segments = 1;
        return kIOReturnSuccess;
    }
};

struct OSNoRetainTag {};
struct OSRetainTag {};
static constexpr OSNoRetainTag OSNoRetain{};
static constexpr OSRetainTag OSRetain{};

#ifndef kIOReturnUnsupported
static constexpr kern_return_t kIOReturnUnsupported = static_cast<kern_return_t>(0xE00002C7);
#endif

static constexpr uint64_t kIOMemoryDirectionInOut = 0;
static constexpr uint64_t kIOMemoryDirectionIn = 1;
static constexpr uint64_t kIOMemoryDirectionOut = 2;
static constexpr uint64_t kIODMACommandCreateNoOptions = 0;
static constexpr uint64_t kIODMACommandPrepareForDMANoOptions = 0;
static constexpr uint64_t kIODMACommandCompleteDMANoOptions = 0;
static constexpr uint64_t kIODMACommandSpecificationNoOptions = 0;
static constexpr uint64_t kIOMemoryMapCacheModeDefault = 0;
static constexpr uint64_t kIOMemoryMapCacheModeInhibit = 0;

template <typename T>
class OSSharedPtr {
public:
    OSSharedPtr() = default;
    OSSharedPtr(T* ptr, OSNoRetainTag) : ptr_(ptr) {}
    OSSharedPtr(T* ptr, OSRetainTag) : ptr_(ptr) {}
    OSSharedPtr(std::nullptr_t) : ptr_(nullptr) {}

    T* get() const { return ptr_.get(); }
    T* operator->() const { return ptr_.get(); }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() { ptr_.reset(); }
    void reset(T* ptr, OSNoRetainTag) { ptr_.reset(ptr); }
    void reset(T* ptr, OSRetainTag) { ptr_.reset(ptr); }

private:
    std::shared_ptr<T> ptr_;
};

#endif // ASFW_HOST_TEST
