#include "VirtualAudioDeviceController.hpp"

namespace ASFW::Driver {

// HAL-facing orchestration (see ../README.md, Step 5). Initialize() makes
// the two previously-dead connections: engine→provider (BindSlotProvider)
// and IO-path→engine (BindDiceTxEngine). Profile selection falls back to the
// registry's generic catch-all, so the controller is always usable even for
// an unknown device identity.

using Protocols::Audio::AMDTP::AmdtpTimingState;
using Protocols::Audio::AMDTP::HostAudioBufferView;
using Protocols::Audio::DICE::DiceDeviceIdentity;
using Protocols::Audio::DICE::DiceStreamConfig;
using Protocols::Audio::DICE::IDiceDeviceProfile;

bool VirtualAudioDeviceController::Initialize() noexcept {
    if (!registry_.RegisterProfile(&focusriteProfile_)) {
        return false;
    }
    selectedProfile_ = registry_.GenericProfile();

    fakeSlotProvider_.Reset();
    txEngine_.BindSlotProvider(&fakeSlotProvider_);
    audioIOPath_.BindDiceTxEngine(&txEngine_);
    return selectedProfile_ != nullptr;
}

bool VirtualAudioDeviceController::SelectProfile(
    const DiceDeviceIdentity& identity) noexcept {
    const IDiceDeviceProfile* profile = registry_.FindProfile(identity);
    selectedProfile_ = (profile != nullptr) ? profile
                                            : registry_.GenericProfile();
    return selectedProfile_ != nullptr;
}

bool VirtualAudioDeviceController::ConfigureOutputStream(
    uint32_t sampleRate, uint32_t channels, uint32_t frameCapacity) noexcept {
    if (selectedProfile_ == nullptr || channels == 0 || channels > 64) {
        return false;
    }

    DiceStreamConfig txConfig{};
    if (!selectedProfile_->BuildDefaultTxStreamConfig(txConfig)) {
        return false;
    }
    txConfig.sampleRate = sampleRate;
    txConfig.pcmChannels = static_cast<uint8_t>(channels);
    txConfig.dbs = static_cast<uint8_t>(channels + txConfig.midiSlots);

    if (!txEngine_.Configure(*selectedProfile_, txConfig)) {
        return false;
    }
    txConfig_ = txConfig;

    audioIOPath_.SetOutputFormatFloat32(channels, frameCapacity);
    return true;
}

void VirtualAudioDeviceController::ResetTransportLab(
    uint8_t initialDbc, uint64_t initialAudioFrame) noexcept {
    txEngine_.ResetForStart(initialDbc, initialAudioFrame);
}

bool VirtualAudioDeviceController::PrepareLabPacket(uint32_t packetIndex,
                                                    uint16_t syt,
                                                    bool timingValid) noexcept {
    AmdtpTimingState timing{};
    timing.txClockValid = timingValid;
    timing.nextDataSyt = syt;
    return txEngine_.PrepareNextTransmitSlot(packetIndex, timing);
}

void VirtualAudioDeviceController::SubmitWriteEnd(
    const HostAudioBufferView& output) noexcept {
    audioIOPath_.HandleWriteEnd(output);
}

const Lab::FakeIsochTxSlotProvider&
VirtualAudioDeviceController::FakeSlotProvider() const noexcept {
    return fakeSlotProvider_;
}

Lab::FakeIsochTxSlotProvider&
VirtualAudioDeviceController::FakeSlotProvider() noexcept {
    return fakeSlotProvider_;
}

} // namespace ASFW::Driver
