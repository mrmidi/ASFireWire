#pragma once

#include <gmock/gmock.h>
#include <cstdint>

// Mock for HardwareInterface to enable unit testing without real hardware
namespace ASFW::Driver::Tests {

class MockHardwareInterface {
public:
    // Register read/write operations
    MOCK_METHOD(uint32_t, Read, (uint32_t offset), (const));
    MOCK_METHOD(void, Write, (uint32_t offset, uint32_t value), ());
    MOCK_METHOD(void, WriteAndFlush, (uint32_t offset, uint32_t value), ());
    
    // Bit manipulation helpers
    MOCK_METHOD(void, SetBits, (uint32_t offset, uint32_t bits), ());
    MOCK_METHOD(void, ClearBits, (uint32_t offset, uint32_t bits), ());
    
    // Memory barriers
    MOCK_METHOD(void, FullBarrier, (), ());
    
    // Virtual destructor for proper cleanup
    virtual ~MockHardwareInterface() = default;
};

} // namespace ASFW::Driver::Tests
