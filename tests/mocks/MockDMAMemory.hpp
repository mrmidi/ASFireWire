#pragma once

#include <gmock/gmock.h>

#include "ASFWDriver/Shared/Memory/IDMAMemory.hpp"

namespace ASFW::Testing {

class MockDMAMemory : public Shared::IDMAMemory {
public:
    using Shared::IDMAMemory::FetchFromDevice;
    using Shared::IDMAMemory::PublishToDevice;
    using Shared::IDMAMemory::VirtToIOVA;

    MOCK_METHOD(std::optional<Shared::DMARegion>, AllocateRegion, (size_t size, size_t alignment), (override));
    MOCK_METHOD(uint64_t, VirtToIOVA, (const std::byte* virt), (const, noexcept, override));
    MOCK_METHOD(std::byte*, IOVAToVirt, (uint64_t iova), (const, noexcept, override));
    MOCK_METHOD(void, PublishToDevice, (const std::byte* address, size_t length), (const, noexcept, override));
    MOCK_METHOD(void, FetchFromDevice, (const std::byte* address, size_t length), (const, noexcept, override));
    MOCK_METHOD(size_t, TotalSize, (), (const, noexcept, override));
    MOCK_METHOD(size_t, AvailableSize, (), (const, noexcept, override));
};

} // namespace ASFW::Testing
