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

bool VirtualAudioDeviceController::GetOutputDeviceCaps(
    OutputDeviceCaps& outCaps) const noexcept {
    if (selectedProfile_ == nullptr) {
        return false;
    }
    DiceStreamConfig txConfig{};
    if (!selectedProfile_->BuildDefaultTxStreamConfig(txConfig)) {
        return false;
    }
    outCaps.sampleRate = txConfig.sampleRate;
    outCaps.pcmChannels = txConfig.pcmChannels;
    return true;
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
    if (labTimingEnabled_) {
        timingModel_.Reset();
        simulatedTimeline_.Reset(timelineEpochTicks_);
        timingCounters_ = TxTimingLabCounters{};
    }
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

void VirtualAudioDeviceController::EnableLabTiming(
    const TxTimingModel::Config& config, int64_t timelineEpochTicks) noexcept {
    timingModel_.Configure(config);
    timelineEpochTicks_ = timelineEpochTicks;
    simulatedTimeline_.Reset(timelineEpochTicks);
    timingCounters_ = TxTimingLabCounters{};
    labTimingEnabled_ = true;
}

void VirtualAudioDeviceController::DisableLabTiming() noexcept {
    labTimingEnabled_ = false;
}

bool VirtualAudioDeviceController::LabTimingEnabled() const noexcept {
    return labTimingEnabled_;
}

void VirtualAudioDeviceController::AdvanceLabTimelineToFrame(
    uint64_t absoluteFrame) noexcept {
    simulatedTimeline_.AdvanceToFrame(absoluteFrame);
}

bool VirtualAudioDeviceController::PrepareLabPacketTimed(
    uint32_t packetIndex) noexcept {
    if (!labTimingEnabled_) {
        return PrepareLabPacket(packetIndex, 0xFFFF, false);
    }

    const auto decision = timingModel_.PeekNextDataSyt(simulatedTimeline_);
    if (decision.seededThisCall) {
        ++timingCounters_.seeds; // the seeding peek may land on a no-data cycle
    }
    if (!PrepareLabPacket(packetIndex, decision.syt, true)) {
        return false;
    }

    // Cadence decided the wire; the model tracks only what was emitted.
    const auto* published = fakeSlotProvider_.PublishedPacket(packetIndex);
    if (published != nullptr && published->packetIndex == packetIndex &&
        published->isData) {
        timingModel_.CommitDataPacket();
        ++timingCounters_.dataPackets;
        timingCounters_.lastLeadTicks = decision.leadTicks;
        switch (decision.health) {
            case TxTimingModel::LeadHealth::kTightWarn:
                ++timingCounters_.tightWarn;
                break;
            case TxTimingModel::LeadHealth::kLate:
                ++timingCounters_.late;
                break;
            case TxTimingModel::LeadHealth::kGate:
                ++timingCounters_.gate;
                break;
            case TxTimingModel::LeadHealth::kEscalate:
                ++timingCounters_.escalate;
                break;
            default:
                break;
        }
    }
    return true;
}

const TxTimingLabCounters&
VirtualAudioDeviceController::TimingCounters() const noexcept {
    return timingCounters_;
}

void VirtualAudioDeviceController::BindLabSlotProvider(
    Protocols::Audio::AMDTP::IAmdtpTxSlotProvider* provider) noexcept {
    txEngine_.BindSlotProvider(
        provider != nullptr
            ? provider
            : static_cast<Protocols::Audio::AMDTP::IAmdtpTxSlotProvider*>(
                  &fakeSlotProvider_));
}

const Protocols::Audio::AMDTP::AmdtpPayloadWriterCounters&
VirtualAudioDeviceController::PayloadCounters() const noexcept {
    return txEngine_.PayloadWriterCounters();
}

const Lab::FakeIsochTxSlotProvider&
VirtualAudioDeviceController::FakeSlotProvider() const noexcept {
    return fakeSlotProvider_;
}

Lab::FakeIsochTxSlotProvider&
VirtualAudioDeviceController::FakeSlotProvider() noexcept {
    return fakeSlotProvider_;
}

const Protocols::Audio::AMDTP::AmdtpPacketTimeline&
VirtualAudioDeviceController::Timeline() const noexcept {
    return txEngine_.Timeline();
}

} // namespace ASFW::Driver
