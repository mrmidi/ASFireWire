// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// HostDriverKitStubs.hpp — Minimal stubs for DriverKit types to allow unit testing on host.

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

class OSObject {
private:
    mutable std::atomic<uint32_t> refCount_{1};
public:
    OSObject() = default;
    virtual ~OSObject() = default;
    OSObject(const OSObject&) = delete;
    OSObject& operator=(const OSObject&) = delete;
    OSObject(OSObject&&) = delete;
    OSObject& operator=(OSObject&&) = delete;

    virtual bool init() { return true; }
    virtual void free() { delete this; }
    void retain() const {
        refCount_.fetch_add(1, std::memory_order_relaxed);
    }
    void release() const {
        if (refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            const_cast<OSObject*>(this)->free();
        }
    }
    uint32_t getRetainCount() const {
        return refCount_.load(std::memory_order_relaxed);
    }
};

class OSAction : public OSObject {};

class IOService : public OSObject {
public:
    virtual kern_return_t Start(IOService*) { return kIOReturnSuccess; }
    virtual void Stop(IOService*) {}
};

using IODispatchQueueName = const char*;

#define OSDynamicCast(T, obj) dynamic_cast<T*>(obj)

#define kPCIBARTypeM32 1
#define kPCIBARTypeM32PF 2
#define kPCIBARTypeM64 3
#define kPCIBARTypeM64PF 4

struct IODMACommandSpecification {
    uint32_t type;
    uint32_t options;
    uint32_t maxAddressBits;
};

namespace ASFW::Testing {

inline uint64_t DefaultHostMonotonicNow() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::function<uint64_t()>& HostMonotonicClockOverride() {
    static std::function<uint64_t()> clockOverride;
    return clockOverride;
}

inline uint64_t HostMonotonicNow() noexcept {
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

    bool manualDispatchForTesting_{false};
    mutable std::mutex pendingLock_;
    std::deque<PendingWorkItem> pending_;
};

class IOInterruptDispatchSource : public OSObject {
public:
    static kern_return_t Create(IOService*, uint32_t, IODispatchQueue*, IOInterruptDispatchSource**) {
        return kIOReturnUnsupported;
    }

    kern_return_t SetHandler(OSAction*) { return kIOReturnUnsupported; }
    kern_return_t SetEnableWithCompletion(bool, void*) { return kIOReturnUnsupported; }
    kern_return_t Cancel(void (^handler)(void)) {
        if (handler) {
            handler();
        }
        return kIOReturnSuccess;
    }
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

class IOPCIDevice : public IOService {
public:
    virtual kern_return_t Open(IOService*) { return kIOReturnUnsupported; }
    virtual void Close(IOService*) {}
    virtual kern_return_t GetBARInfo(uint8_t, uint8_t*, uint64_t*, uint8_t*) { return kIOReturnUnsupported; }
    virtual void MemoryRead32(uint8_t, uint64_t, uint32_t*) {}
    virtual void MemoryWrite32(uint8_t, uint64_t, uint32_t) {}
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

class IOMemoryDescriptor : public OSObject {
public:
    virtual kern_return_t GetAddressRange(IOAddressSegment* range) = 0;
    // Matches the real SDK signature (kern_return_t, not void) so handler code
    // can check the return value under host test.
    virtual kern_return_t GetLength(uint64_t* length) = 0;
    virtual kern_return_t CreateMapping(uint64_t options,
                                        uint64_t address,
                                        uint64_t offset,
                                        uint64_t length,
                                        uint64_t alignment,
                                        IOMemoryMap** map) = 0;
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
    void* buffer_{nullptr};
    uint64_t length_{0};
public:
    virtual ~IOBufferMemoryDescriptor() override {
        if (buffer_) {
            ::free(buffer_);
            buffer_ = nullptr;
        }
    }

    virtual void free() override {
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
        std::memset(ptr, 0, length);
        desc->buffer_ = ptr;
        desc->length_ = length;
        *descriptor = desc;
        return kIOReturnSuccess;
    }

    virtual kern_return_t GetAddressRange(IOAddressSegment* range) override {
        if (!range) return kIOReturnBadArgument;
        range->address = reinterpret_cast<uint64_t>(buffer_);
        range->length = length_;
        return kIOReturnSuccess;
    }

    kern_return_t GetLength(uint64_t* length) override {
        if (!length) {
            return kIOReturnBadArgument;
        }
        *length = length_;
        return kIOReturnSuccess;
    }
    
    kern_return_t SetLength(uint64_t len) {
        if (len > length_) return kIOReturnNoSpace;
        // Don't actually realloc, just track 'length' if needed, but for now we trust the alloc size
        return kIOReturnSuccess;
    }

    kern_return_t CreateMapping(uint64_t options,
                                uint64_t address,
                                uint64_t offset,
                                uint64_t length,
                                uint64_t alignment,
                                IOMemoryMap** map) override {
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
    kern_return_t PrepareForDMA(uint64_t options,
                                IOMemoryDescriptor* buffer,
                                uint64_t offset,
                                uint64_t length,
                                uint64_t* flags,
                                uint32_t* segments,
                                IOAddressSegment* segmentOut) {
        if (!segmentOut || !segments || *segments == 0) {
            return kIOReturnBadArgument;
        }
        if (*segments > 32) {
            return kIOReturnOverrun;
        }
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
static constexpr uint64_t kIOMemoryDirectionOutIn = 3;
static constexpr uint64_t kIODMACommandCreateNoOptions = 0;
static constexpr uint64_t kIODMACommandPrepareForDMANoOptions = 0;
static constexpr uint64_t kIODMACommandCompleteDMANoOptions = 0;
static constexpr uint64_t kIODMACommandSpecificationNoOptions = 0;
static constexpr uint64_t kIOMemoryMapCacheModeDefault = 0;
static constexpr uint64_t kIOMemoryMapCacheModeInhibit = 0;

template <typename T>
class OSSharedPtr {
public:
    OSSharedPtr() noexcept : ptr_(nullptr) {}
    OSSharedPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}

    OSSharedPtr(T* ptr, OSNoRetainTag) noexcept : ptr_(ptr) {}
    OSSharedPtr(T* ptr, OSRetainTag) noexcept : ptr_(ptr) {
        if (ptr_) ptr_->retain();
    }

    OSSharedPtr(const OSSharedPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) ptr_->retain();
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    OSSharedPtr(const OSSharedPtr<U>& other) noexcept : ptr_(other.get()) {
        if (ptr_) ptr_->retain();
    }

    OSSharedPtr(OSSharedPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    OSSharedPtr(OSSharedPtr<U>&& other) noexcept : ptr_(other.detach()) {}

    ~OSSharedPtr() {
        if (ptr_) {
            ptr_->release();
            ptr_ = nullptr;
        }
    }

    OSSharedPtr& operator=(const OSSharedPtr& other) noexcept {
        if (this != &other) {
            T* old = ptr_;
            ptr_ = other.ptr_;
            if (ptr_) ptr_->retain();
            if (old) old->release();
        }
        return *this;
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    OSSharedPtr& operator=(const OSSharedPtr<U>& other) noexcept {
        if (ptr_ != other.get()) {
            T* old = ptr_;
            ptr_ = other.get();
            if (ptr_) ptr_->retain();
            if (old) old->release();
        }
        return *this;
    }

    OSSharedPtr& operator=(OSSharedPtr&& other) noexcept {
        if (this != &other) {
            T* old = ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
            if (old) old->release();
        }
        return *this;
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    OSSharedPtr& operator=(OSSharedPtr<U>&& other) noexcept {
        if (ptr_ != other.get()) {
            T* old = ptr_;
            ptr_ = other.detach();
            if (old) old->release();
        }
        return *this;
    }

    OSSharedPtr& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    void reset() noexcept {
        if (ptr_) {
            T* old = ptr_;
            ptr_ = nullptr;
            old->release();
        }
    }

    void reset(T* ptr, OSNoRetainTag) noexcept {
        if (ptr_ != ptr) {
            T* old = ptr_;
            ptr_ = ptr;
            if (old) old->release();
        }
    }

    void reset(T* ptr, OSRetainTag) noexcept {
        if (ptr) ptr->retain();
        if (ptr_ != ptr) {
            T* old = ptr_;
            ptr_ = ptr;
            if (old) old->release();
        } else if (ptr) {
            ptr->release();
        }
    }

    T* detach() noexcept {
        T* raw = ptr_;
        ptr_ = nullptr;
        return raw;
    }

    template <typename U>
    bool operator==(const OSSharedPtr<U>& other) const noexcept { return ptr_ == other.get(); }
    template <typename U>
    bool operator!=(const OSSharedPtr<U>& other) const noexcept { return ptr_ != other.get(); }
    bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_{nullptr};
};

template <typename T>
inline bool operator==(std::nullptr_t, const OSSharedPtr<T>& p) noexcept { return p.get() == nullptr; }
template <typename T>
inline bool operator!=(std::nullptr_t, const OSSharedPtr<T>& p) noexcept { return p.get() != nullptr; }

template <typename T>
OSSharedPtr(T*, OSNoRetainTag) -> OSSharedPtr<T>;
template <typename T>
OSSharedPtr(T*, OSRetainTag) -> OSSharedPtr<T>;

#endif // ASFW_HOST_TEST
