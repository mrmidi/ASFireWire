#pragma once
#include <DriverKit/OSObject.h>
#include <cstdint>

// Real value-carrying stub. The no-op version returned 0 for every read, which
// made any round trip through a property dictionary untestable: the encode side
// could be deleted entirely and tests would still pass.
class OSNumber : public OSObject {
public:
    static OSNumber* withNumber(uint64_t value, uint32_t numberOfBits) {
        auto* number = new OSNumber();
        number->value_ = value;
        number->bits_ = numberOfBits;
        return number;
    }
    [[nodiscard]] uint64_t unsigned64BitValue() const { return value_; }
    [[nodiscard]] uint32_t unsigned32BitValue() const {
        return static_cast<uint32_t>(value_ & 0xFFFFFFFFULL);
    }
    [[nodiscard]] uint8_t unsigned8BitValue() const {
        return static_cast<uint8_t>(value_ & 0xFFULL);
    }
    [[nodiscard]] uint32_t numberOfBits() const { return bits_; }

private:
    uint64_t value_{0};
    uint32_t bits_{0};
};
