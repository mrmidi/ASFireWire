// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioSpecialProtocol.hpp — FireWire 1814 and ProjectMix I/O.
//
// One class for both, because they are one device family running one firmware:
// Linux gives them the same quirk, the same forced reset, the same SpecialModel
// and the same control surface, and differs only in how many rates it offers
// (bebob.c:171-174, :285-293; maudio/special.rs:73 vs :89).
//
// What makes this device unlike every other BeBoB adapter here:
//
//  - **Its geometry is not discovered, it is asserted.** MAudioSpecialFormation
//    holds the table; nothing may ask the device to confirm it (H5).
//  - **Capture and playback shapes are chosen independently**, by dig_in_fmt and
//    dig_out_fmt in the vendor clock command, so DeviceCaps() depends on runtime
//    state rather than being a constant like Phase88Caps().
//  - **Only six AV/C frame shapes may ever be sent to it.** The permitted-frame
//    table in Protocols/AVC/AVCCommandFilter.hpp refuses everything else at
//    submit, including anything this class might send by mistake.
//
// The inverted start choreography (H8) is implemented by ConfirmDuplexStart:
// after host DMA is armed, this device receives the signal-format pair again.

#pragma once

#include "BeBoBProtocol.hpp"
#include "MAudioSpecialFormation.hpp"

#include <cstdint>
#include <vector>

namespace ASFW::Audio::BeBoB {

/// M-Audio special-firmware parameter window, 0xffc700700000. Write-only: the
/// firmware answers no read here, so the host must assert every register.
/// Register map in FFADO bebob/maudio/special_avdevice.h:47-134.
inline constexpr uint16_t kMAudioParamAddressHi = 0xFFC7;
inline constexpr uint32_t kMAudioParamAddressLo = 0x0070'0000;

/// Which of the two personas this instance is driving. They share geometry and
/// control; only the offered rate list differs.
enum class MAudioSpecialModel : uint8_t {
    FireWire1814,
    ProjectMix,
};

class MAudioSpecialProtocol final : public BeBoBProtocol {
public:
    MAudioSpecialProtocol(Protocols::Ports::FireWireBusOps& busOps,
                          Protocols::Ports::FireWireBusInfo& busInfo,
                          Discovery::DeviceRouteToken route,
                          IRM::IRMClient* irmClient,
                          CMP::CMPClient* cmpClient,
                          Scheduling::ITimerScheduler* timerScheduler,
                          MAudioSpecialModel model) noexcept;

    const char* GetName() const override { return DeviceName(); }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;

    /// Tell the device which clock to run on and which digital formats are
    /// selected, then wait out its settle. Must complete before streaming.
    ///
    /// This reproduces the vendor driver's SetBlankSlateClockSource: the
    /// M-Audio clock/formation frame, 300 ms, Audio selector FB 4, 300 ms, then
    /// the outer 2500 ms blank-slate settle. It lives here rather than in
    /// ApplyClockConfig because that sequence must finish before publication.
    void InitializeClock(std::function<void(IOReturn)> completion);

protected:
    const char* DeviceName() const override;
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override;
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    void ReadClockHealth(HealthCallback callback) override;

    /// This firmware fails the INPUT signal-format command when it lands
    /// immediately after the OUTPUT one. Linux's special_set_rate() waits
    /// exactly 100 ms. The vendor kext's 300 ms waits surround a different
    /// operation (its clock/selector pair), so they do not apply here.
    [[nodiscard]] uint32_t SignalFormatInterlockMs() const override { return 100; }

    /// The start choreography is inverted here: signal format must be re-sent
    /// *after* host DMA is running, not only before CMP. See the definition.
    void ConfirmDuplexStart(ConfirmCallback callback) override;

private:
    /// Assert the whole 0x00-0x9c parameter window in one block write.
    ///
    /// These registers are write-only: the firmware answers no read for them,
    /// so their power-on state is indeterminate and every quadlet must be
    /// stated. FFADO does exactly this at startup (Mixer::initialize,
    /// bebob/maudio/special_mixer.cpp:74-106) and drives this device; the
    /// vendor kext reaches the same end state through 42 individual per-control
    /// writes, which is its control API, not a wire requirement.
    ///
    /// Runs as the last step of InitializeClock, mirroring the vendor's
    /// non-blank-slate pass, where the equivalent register push happens
    /// (FWSettingsLevels::SendToDevice).
    void SendParameterBlock(std::function<void(IOReturn)> completion);

    [[nodiscard]] AudioStreamRuntimeCaps CapsForCurrentFormation() const noexcept;

    const MAudioSpecialModel model_;

    // Mirrors Linux's `struct special_params`. These are driver-side beliefs
    // about device state, not readbacks — the device is never asked to confirm
    // them, so they must only ever change alongside a vendor clock command that
    // actually succeeded.
    //
    // Both default to S/PDIF because the vendor blank-slate frame and Linux's
    // discover-time frame agree on dig_in_fmt=0 and dig_out_fmt=0, despite
    // differing on clk_src (vendor 0, Linux 3).
    MAudioDigitalFormat captureFormat_{MAudioDigitalFormat::SPDIF};
    MAudioDigitalFormat playbackFormat_{MAudioDigitalFormat::SPDIF};

    // The rate the geometry is currently shaped for. Seeded from the device via
    // the signal-format probe rather than assumed, because this firmware keeps
    // its rate across a host restart and guessing wrong mis-shapes the stream.
    uint32_t currentRateHz_{48000};
};

} // namespace ASFW::Audio::BeBoB
