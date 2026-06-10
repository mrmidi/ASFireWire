#pragma once

#include "../AMDTP/AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxQuirks final {
    AMDTP::PcmSlotEncoding hostToDevicePcmEncoding{
        AMDTP::PcmSlotEncoding::Am824MBLA
    };

    AMDTP::DbsPolicy dbsPolicy{
        AMDTP::DbsPolicy::Constant
    };

    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool initializeNonAudioSlots{true};
};

struct DiceRxQuirks final {
    AMDTP::PcmSlotEncoding deviceToHostPcmEncoding{
        AMDTP::PcmSlotEncoding::Am824MBLA
    };

    AMDTP::DbsPolicy dbsPolicy{
        AMDTP::DbsPolicy::Constant
    };
};

struct DiceDeviceQuirks final {
    DiceTxQuirks tx{};
    DiceRxQuirks rx{};
};

} // namespace ASFW::Protocols::Audio::DICE