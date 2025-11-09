#pragma once

#include <gmock/gmock.h>
#include <optional>
#include "../../ASFWDriver/Core/SelfIDCapture.hpp"

// Mock for SelfIDCapture to test bus reset coordination
namespace ASFW::Driver::Tests {

class MockSelfIDCapture {
public:
    using Result = ASFW::Driver::SelfIDCapture::Result;
    
    // Buffer management
    MOCK_METHOD(void, Arm, (), ());
    MOCK_METHOD(void, Disarm, (), ());
    
    // Decoding with double-read validation
    MOCK_METHOD(std::optional<Result>, Decode, (uint32_t selfIDCountReg), (const));
    
    // Virtual destructor for proper cleanup
    virtual ~MockSelfIDCapture() = default;
};

} // namespace ASFW::Driver::Tests
