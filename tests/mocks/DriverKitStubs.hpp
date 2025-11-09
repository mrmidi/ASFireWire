#pragma once

// DriverKit stubs for host-side testing
// These provide minimal interface definitions to allow compilation without DriverKit SDK

#ifdef ASFW_HOST_TEST

#include <memory>
#include <cstdint>

// OSSharedPtr stub - mimics DriverKit smart pointer
template<typename T>
class OSSharedPtr {
public:
    OSSharedPtr() : ptr_(nullptr) {}
    explicit OSSharedPtr(T* p) : ptr_(p) {}
    
    T* get() const { return ptr_.get(); }
    T* operator->() const { return ptr_.get(); }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    
    void reset(T* p = nullptr) { ptr_.reset(p); }
    
private:
    std::shared_ptr<T> ptr_;
};

// IOMemoryMap stub
class IOMemoryMap {
public:
    virtual ~IOMemoryMap() = default;
    virtual uint64_t GetAddress() const { return 0; }
    virtual uint64_t GetLength() const { return 0; }
};

// IOBufferMemoryDescriptor stub
class IOBufferMemoryDescriptor {
public:
    virtual ~IOBufferMemoryDescriptor() = default;
};

// IODMACommand stub
class IODMACommand {
public:
    virtual ~IODMACommand() = default;
    
    enum IODMACommandMemoryOptions {
        kIOMD_Read = 1,
        kIOMD_Write = 2,
        kIOMD_ReadWrite = 3
    };
    
    virtual void FullBarrier() {}
};

// IODispatchQueue stub
class IODispatchQueue {
public:
    virtual ~IODispatchQueue() = default;
};

// kern_return_t stub
using kern_return_t = int;
constexpr kern_return_t kIOReturnSuccess = 0;
constexpr kern_return_t kIOReturnError = -1;

#endif // ASFW_HOST_TEST
