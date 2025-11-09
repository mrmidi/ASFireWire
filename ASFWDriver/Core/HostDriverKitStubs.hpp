#pragma once

#ifdef ASFW_HOST_TEST

#include <mach/kern_return.h>
#include <cstdint>
#include <functional>
#include <DriverKit/IOReturn.h>

struct IOAddressSegment {
    uint64_t address{0};
    uint64_t length{0};
};

class IOService {};

class OSObject {
public:
    virtual ~OSObject() = default;
    void retain() {}
    void release() {}
};

class OSAction : public OSObject {};

class IODispatchQueue : public OSObject {
public:
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

class IOPCIDevice : public OSObject {
public:
    kern_return_t Open(IOService*) { return kIOReturnUnsupported; }
    void Close(IOService*) {}
    kern_return_t GetBARInfo(uint8_t, uint8_t*, uint64_t*, uint8_t*) { return kIOReturnUnsupported; }
    void MemoryRead32(uint8_t, uint64_t, uint32_t*) {}
    void MemoryWrite32(uint8_t, uint64_t, uint32_t) {}
};

class IOBufferMemoryDescriptor : public OSObject {
public:
    static kern_return_t Create(uint64_t, uint64_t, uint64_t, IOBufferMemoryDescriptor**) {
        return kIOReturnUnsupported;
    }
    kern_return_t GetAddressRange(IOAddressSegment*) { return kIOReturnUnsupported; }
    kern_return_t SetLength(uint64_t) { return kIOReturnUnsupported; }
};

class IOMemoryMap : public OSObject {
public:
    uint64_t GetAddress() const { return 0; }
    uint64_t GetLength() const { return 0; }
};

class IODMACommand : public OSObject {
public:
    static kern_return_t Create(IOService*, uint64_t, void*, IODMACommand**) {
        return kIOReturnUnsupported;
    }
    void FullBarrier() {}
};

struct OSNoRetainTag {};
struct OSRetainTag {};
static constexpr OSNoRetainTag OSNoRetain{};
static constexpr OSRetainTag OSRetain{};

#ifndef kIOReturnUnsupported
static constexpr kern_return_t kIOReturnUnsupported = static_cast<kern_return_t>(0xE00002C7);
#endif

static constexpr uint64_t kIOMemoryDirectionInOut = 0;
static constexpr uint64_t kIODMACommandCreateNoOptions = 0;

template <typename T>
class OSSharedPtr {
public:
    OSSharedPtr() = default;
    OSSharedPtr(T* ptr, OSNoRetainTag) : ptr_(ptr) {}
    OSSharedPtr(T* ptr, OSRetainTag) : ptr_(ptr) {}

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() { ptr_ = nullptr; }

private:
    T* ptr_{nullptr};
};

#endif // ASFW_HOST_TEST
