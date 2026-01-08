#pragma once
#include <DriverKit/OSObject.h>
class OSNumber : public OSObject {
public:
    static OSNumber* withNumber(uint64_t value, uint32_t numberOfBits) { return new OSNumber(); }
    uint64_t unsigned64BitValue() const { return 0; }
};
