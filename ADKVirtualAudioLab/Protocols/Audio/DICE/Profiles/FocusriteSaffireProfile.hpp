#pragma once

#include "../DiceDeviceProfile.hpp"

namespace ASFW::Protocols::Audio::DICE::Profiles {

class FocusriteSaffireProfile final : public IDiceDeviceProfile {
public:
    const char* Name() const noexcept override;

    bool Matches(const DiceDeviceIdentity& identity) const noexcept override;

    DiceDeviceQuirks Quirks() const noexcept override;

    bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
};

} // namespace ASFW::Protocols::Audio::DICE::Profiles