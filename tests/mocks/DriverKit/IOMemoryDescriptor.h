#pragma once
#include "../../ASFWDriver/Testing/HostDriverKitStubs.hpp"

#include <cstdint>
#include <vector>

// IOMemoryDescriptor itself lives in HostDriverKitStubs.hpp (abstract, mirroring
// the SDK surface the handlers use). This header adds a concrete test double that
// can be handed to handlers via IOUserClientMethodArguments::structureInputDescriptor
// / structureOutputDescriptor.
class HostTestMemoryDescriptor : public IOMemoryDescriptor {
    std::vector<uint8_t> bytes_;
public:
    void SetMockBytes(const void* data, size_t length) {
        bytes_.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + length);
    }

    // Output-descriptor use: size the backing buffer, let the handler fill it
    // through CreateMapping, then inspect the result here.
    void ResizeMockBytes(size_t length) { bytes_.assign(length, 0); }
    const std::vector<uint8_t>& MockBytes() const { return bytes_; }

    kern_return_t GetAddressRange(IOAddressSegment* range) override {
        if (!range) {
            return kIOReturnBadArgument;
        }
        range->address = reinterpret_cast<uint64_t>(bytes_.data());
        range->length = bytes_.size();
        return kIOReturnSuccess;
    }

    kern_return_t GetLength(uint64_t* returnLength) override {
        if (!returnLength) {
            return kIOReturnBadArgument;
        }
        *returnLength = bytes_.size();
        return kIOReturnSuccess;
    }

    kern_return_t CreateMapping(uint64_t /*options*/, uint64_t /*address*/,
                                uint64_t offset, uint64_t length,
                                uint64_t /*alignment*/, IOMemoryMap** map) override {
        if (!map || offset > bytes_.size()) {
            return kIOReturnBadArgument;
        }
        auto* m = new IOMemoryMap();
        m->SetMockData(reinterpret_cast<uint64_t>(bytes_.data()) + offset,
                       length > 0 ? length : bytes_.size() - offset);
        *map = m;
        return kIOReturnSuccess;
    }
};
