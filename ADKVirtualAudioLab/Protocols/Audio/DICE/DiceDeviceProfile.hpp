#pragma once

#include "DiceQuirks.hpp"
#include "DiceStreamConfig.hpp"
#include "../AMDTP/AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceDeviceIdentity final {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
};

class IDiceDeviceProfile {
public:
    virtual ~IDiceDeviceProfile() = default;

    virtual const char* Name() const noexcept = 0;

    virtual bool Matches(const DiceDeviceIdentity& identity) const noexcept = 0;

    virtual DiceDeviceQuirks Quirks() const noexcept = 0;

    virtual bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept = 0;
    virtual bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept = 0;
};

} // namespace ASFW::Protocols::Audio::DICE