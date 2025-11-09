#pragma once

#include <gmock/gmock.h>
#include <cstdint>

// Mock for ConfigROMStager to test Config ROM restoration
namespace ASFW::Driver::Tests {

class MockConfigROMStager {
public:
    // Config ROM restoration
    MOCK_METHOD(void, RestoreHeaderAfterBusReset, (), ());
    MOCK_METHOD(uint32_t, ExpectedBusOptions, (), (const));
    MOCK_METHOD(uint32_t, ExpectedHeader, (), (const));
    
    // Virtual destructor for proper cleanup
    virtual ~MockConfigROMStager() = default;
};

} // namespace ASFW::Driver::Tests
