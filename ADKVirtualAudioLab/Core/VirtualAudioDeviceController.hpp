#pragma once

#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Lab/SimulatedCycleTimeline.hpp"
#include "../Protocols/Audio/DICE/DiceProfileRegistry.hpp"
#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"
#include "../Protocols/Audio/DICE/Profiles/FocusriteSaffireProfile.hpp"
#include "AudioIOPath.hpp"
#include "TxTimingModel.hpp"

#include <cstdint>

namespace ASFW::Driver {

// Lead-health bookkeeping for the timed prepare path (work-queue confined,
// like the rest of the controller's lab state).
struct TxTimingLabCounters final {
    uint64_t dataPackets{0};
    uint64_t seeds{0};
    uint64_t tightWarn{0};
    uint64_t late{0};
    uint64_t gate{0};
    uint64_t escalate{0};
    int64_t lastLeadTicks{0};
};

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

    // Milestone 2 — SYT realism. EnableLabTiming binds a TxTimingModel and a
    // SimulatedCycleTimeline (advanced by the caller from the frame cursor);
    // PrepareLabPacketTimed then stamps real SYTs: peek the model, let the
    // packetizer's cadence decide the wire, commit only if a data packet was
    // actually published (the model tracks, it never decides). Lead health is
    // recorded as telemetry per data packet.
    void EnableLabTiming(const TxTimingModel::Config& config,
                         int64_t timelineEpochTicks = 0) noexcept;
    void DisableLabTiming() noexcept;
    [[nodiscard]] bool LabTimingEnabled() const noexcept;
    void AdvanceLabTimelineToFrame(uint64_t absoluteFrame) noexcept;
    bool PrepareLabPacketTimed(uint32_t packetIndex) noexcept;
    [[nodiscard]] const TxTimingLabCounters& TimingCounters() const noexcept;

    void SubmitWriteEnd(const Protocols::Audio::AMDTP::HostAudioBufferView& output) noexcept;

    // Step 6 seam: interpose a decorator (Verifying(Fake)) between the engine
    // and the fake ring. Call after Initialize(), which binds the bare fake;
    // passing nullptr restores the fake. The caller owns the provider and
    // typically wraps FakeSlotProvider() itself.
    void BindLabSlotProvider(
        Protocols::Audio::AMDTP::IAmdtpTxSlotProvider* provider) noexcept;

    const Protocols::Audio::AMDTP::AmdtpPayloadWriterCounters&
    PayloadCounters() const noexcept;

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

    bool labTimingEnabled_{false};
    int64_t timelineEpochTicks_{0};
    TxTimingModel timingModel_{};
    Lab::SimulatedCycleTimeline simulatedTimeline_{};
    TxTimingLabCounters timingCounters_{};
};

} // namespace ASFW::Driver