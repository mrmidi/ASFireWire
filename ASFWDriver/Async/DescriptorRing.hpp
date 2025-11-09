#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::Async {

template <typename Traits>
class DescriptorRing {
public:
    DescriptorRing() = default;
    ~DescriptorRing() = default;

    bool Initialise(std::size_t /*descriptorCount*/) {
        // TODO: allocate descriptor ring storage and initialise hardware pointers.
        return false;
    }

    void Shutdown() {
        // TODO: release descriptor ring storage.
    }

    bool Append(void* /*firstDescriptor*/, void* /*lastDescriptor*/) {
        // TODO: append descriptor chain to ring and update hardware tail pointer.
        return false;
    }

    bool IsEmpty() const {
        // TODO: report whether hardware ring has pending descriptors.
        return true;
    }

    void* HardwareHead() const {
        // TODO: expose hardware head pointer for diagnostics.
        return nullptr;
    }

    void* HardwareTail() const {
        // TODO: expose hardware tail pointer for diagnostics.
        return nullptr;
    }

    void ServiceCompleted() {
        // TODO: update software state after servicing completed descriptors.
    }

private:
    std::size_t descriptorCount_{0};
};

} // namespace ASFW::Async

