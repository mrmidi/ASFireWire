#pragma once

#include <gmock/gmock.h>

// Mock for AsyncSubsystem to test AT context coordination
namespace ASFW::Driver::Tests {

class MockAsyncSubsystem {
public:
    // AT context management
    MOCK_METHOD(void, FlushATContexts, (), ());
    MOCK_METHOD(void, RearmATContexts, (), ());
    MOCK_METHOD(bool, AreATContextsActive, (), (const));
    
    // Virtual destructor for proper cleanup
    virtual ~MockAsyncSubsystem() = default;
};

} // namespace ASFW::Driver::Tests
