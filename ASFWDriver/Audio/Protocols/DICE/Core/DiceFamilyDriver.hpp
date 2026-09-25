// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFamilyDriver.hpp - Linear DICE duplex bring-up and teardown.
//
// Replaces the callback-chained DICEDuplexBringupController
// (documentation/AUDIO_SESSION_REDESIGN.md, stage S1). Every method runs to
// completion on the caller's thread and reads top to bottom: each register
// access goes through DiceDeviceIo, each wait through DiceWaitClock. The wire
// behaviour is the controller's, transaction for transaction; the golden
// traces in tests/golden/dice/ prove it.
//
// Threading: called by the duplex coordinator on the audio nub's queue or
// com.asfw.audio.dice, never on the driver's Default queue, where the bus
// completions it waits for are delivered.

#pragma once

#include "DICETypes.hpp"
#include "DiceDeviceIo.hpp"
#include "../../Duplex/DuplexControlTypes.hpp"
#include "../../../../Protocols/Ports/FireWireBusPort.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <cstdint>
#include <expected>

namespace ASFW::Audio::DICE {

class DiceFamilyDriver {
public:
    // The section layout is read at the start of every bring-up.
    DiceFamilyDriver(DiceDeviceIo& io,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     DICEBringupPolicy bringupPolicy) noexcept;

    DiceFamilyDriver(const DiceFamilyDriver&) = delete;
    DiceFamilyDriver& operator=(const DiceFamilyDriver&) = delete;

    // Claim the device, bring its clock to `clock`, and read its streams.
    // `refreshRuntimeCaps` re-reads the full stream configuration at the end
    // (the coordinator path); the reference-parity window does not.
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Prepare(
        const AudioDuplexChannels& channels,
        const DiceClockConfiguration& clock,
        bool refreshRuntimeCaps);
    // Arm every device-RX stream (host playback).
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ProgramRx();
    // Arm every device-TX stream (host capture), then the single GLOBAL_ENABLE.
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ProgramTxAndEnable();
    // Wait for source lock and confirm the device runs the channels we armed.
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm();
    // Change the clock while idle: claim, apply, release.
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClock(
        const DiceClockConfiguration& clock);
    // Total stop: disable, disarm every stream, release the owner.
    [[nodiscard]] IOReturn Stop();
    [[nodiscard]] IOReturn ReleaseOwner();

    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept { teardownCancel_ = cancel; }

    [[nodiscard]] bool IsPrepared() const noexcept { return session_.devicePrepared; }
    [[nodiscard]] bool IsArmed() const noexcept { return session_.deviceTxArmed; }
    [[nodiscard]] bool IsRunning() const noexcept { return session_.deviceRunning; }
    [[nodiscard]] bool IsOwnerClaimed() const noexcept { return session_.ownerClaimed; }

private:
    enum class FlowMode : uint8_t { kNone, kPrepareDuplex, kClockApply };

    // Bring-up chain. Each returns kIOReturnSuccess, or the error after rolling back.
    [[nodiscard]] IOReturn ClaimAndClock(const AudioDuplexChannels& channels);
    [[nodiscard]] IOReturn WriteClockSelect(const AudioDuplexChannels& channels);
    [[nodiscard]] IOReturn ActiveClockCheck(const AudioDuplexChannels& channels, uint32_t accumulatedNotify);
    [[nodiscard]] IOReturn WaitClockAccepted(const AudioDuplexChannels& channels);
    [[nodiscard]] IOReturn ReadGlobalAfterClockAccepted(const AudioDuplexChannels& channels,
                                                        uint32_t observedNotify,
                                                        IOReturn failureStatus);
    [[nodiscard]] IOReturn ClockConfirmed(const AudioDuplexChannels& channels);
    [[nodiscard]] IOReturn AwaitStreamingClockLock(const AudioDuplexChannels& channels);
    [[nodiscard]] IOReturn DiscoverStreams();
    [[nodiscard]] IOReturn FinishPrepare();
    [[nodiscard]] IOReturn CompleteClockApply();
    [[nodiscard]] IOReturn RefreshRuntimeCaps();
    [[nodiscard]] IOReturn ProgramRxStreams();
    [[nodiscard]] IOReturn ProgramTxStreamsAndEnable();
    [[nodiscard]] IOReturn CompleteConfirm(uint32_t notification, uint32_t status, uint32_t extStatus);
    [[nodiscard]] IOReturn Rollback(IOReturn error);

    // Stop sequence.
    [[nodiscard]] IOReturn StopSequence(bool releaseOwner);
    [[nodiscard]] IOReturn StopReleaseOwner();
    [[nodiscard]] bool AbortStopIfTeardown(const char* stage);

    [[nodiscard]] bool EnsureRouteCurrent() const noexcept;
    [[nodiscard]] bool TeardownRequested() const noexcept;
    [[nodiscard]] uint64_t OwnerValue() const noexcept;
    [[nodiscard]] uint32_t ResolvedTxSpeed() const noexcept;
    void RecordStopTeardownAbort(const char* stage) const noexcept;

    DiceDeviceIo& io_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    DICEBringupPolicy bringupPolicy_{};
    GeneralSections sections_{};

    DuplexRestartSession session_{};
    DiceClockConfiguration diceClock_{};
    FlowMode flowMode_{FlowMode::kNone};
    AudioStreamRuntimeCaps runtimeCaps_{};
    uint32_t confirmNotification_{0};
    uint32_t confirmStatus_{0};
    uint32_t confirmExtStatus_{0};
    IOReturn stopSequenceError_{kIOReturnSuccess};
    bool refreshRuntimeCapsOnPrepare_{true};
    // GLOBAL state read before the owner claim: CLOCK_SELECT is the rate that was
    // requested, STATUS/SAMPLE_RATE the rate the device reached. The redundant
    // CLOCK_SELECT write is skipped only when both are at the target.
    uint32_t preClaimClockSelect_{0};
    uint32_t preClaimStatus_{0};
    uint32_t preClaimSampleRate_{0};
    const std::atomic<bool>* teardownCancel_{nullptr};
};

} // namespace ASFW::Audio::DICE
