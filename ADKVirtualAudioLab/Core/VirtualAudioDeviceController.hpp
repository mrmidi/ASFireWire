#pragma once

#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Protocols/Audio/DICE/DiceProfileRegistry.hpp"
#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"
#include "../Protocols/Audio/DICE/Profiles/FocusriteSaffireProfile.hpp"
#include "AudioIOPath.hpp"

#include <cstdint>

namespace ASFW::Driver {

class VirtualAudioDeviceController final {
public:
    VirtualAudioDeviceController() noexcept = default;

    bool Initialize() noexcept;

    bool SelectProfile(const Protocols::Audio::DICE::DiceDeviceIdentity& identity) noexcept;

    bool ConfigureOutputStream(uint32_t sampleRate,
                               uint32_t channels,
                               uint32_t frameCapacity) noexcept;

    void ResetTransportLab(uint8_t initialDbc,
                           uint64_t initialAudioFrame) noexcept;

    bool PrepareLabPacket(uint32_t packetIndex,
                          uint16_t syt,
                          bool timingValid) noexcept;

    void SubmitWriteEnd(const Protocols::Audio::AMDTP::HostAudioBufferView& output) noexcept;

    const Lab::FakeIsochTxSlotProvider& FakeSlotProvider() const noexcept;
    Lab::FakeIsochTxSlotProvider& FakeSlotProvider() noexcept;

private:
    Protocols::Audio::DICE::Profiles::FocusriteSaffireProfile focusriteProfile_{};
    Protocols::Audio::DICE::DiceProfileRegistry registry_{};
    const Protocols::Audio::DICE::IDiceDeviceProfile* selectedProfile_{nullptr};

    Protocols::Audio::DICE::DiceStreamConfig txConfig_{};
    Protocols::Audio::DICE::DiceTxStreamEngine txEngine_{};

    Lab::FakeIsochTxSlotProvider fakeSlotProvider_{};

    AudioIOPath audioIOPath_{};
};

} // namespace ASFW::Driver