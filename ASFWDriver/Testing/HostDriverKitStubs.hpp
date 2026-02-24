#pragma once

#ifdef ASFW_HOST_TEST

#include <mach/kern_return.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <DriverKit/IOReturn.h>

struct IOAddressSegment {
    uint64_t address{0};
    uint64_t length{0};
};

// Forward declare for Create
class IOBufferMemoryDescriptor;

class IOService {};

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
    static kern_return_t Create(const char*, uint64_t, uint64_t, IODispatchQueue**) {
        return kIOReturnUnsupported;
    }

    void DispatchAsync(const std::function<void()>& work) {
        if (work) {
            work();
        }
    }

    void DispatchSync(const std::function<void()>& work) {
        if (work) {
            work();
        }
    }
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
        segmentOut[0] = seg;
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
