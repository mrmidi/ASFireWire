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
//  - **Only five AV/C frame shapes may ever be sent to it.** The permitted-frame
//    table in Protocols/AVC/AVCCommandFilter.hpp refuses everything else at
//    submit, including anything this class might send by mistake.
//
// The inverted start choreography (H8) is deliberately NOT here yet: the base
// programs signal format before CMP, and this device needs it re-sent after DMA
// start. That needs a new hook on BeBoBProtocol and lands separately.

#pragma once

#include "BeBoBProtocol.hpp"
#include "MAudioSpecialFormation.hpp"

#include <cstdint>
#include <vector>

namespace ASFW::Audio::BeBoB {

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
    /// This is the analogue of Linux's discover-time
    /// avc_maudio_set_special_clk(bebob, 0x03, 0, 0, 0), and it lives here
    /// rather than in ApplyClockConfig because the settle is 2500 ms — long
    /// enough that paying it inside the stream-start path would blow the 4 s
    /// initial-ZTS budget on its own. See kMAudioClockSettleMs.
    void InitializeClock(std::function<void(IOReturn)> completion);

protected:
    const char* DeviceName() const override;
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override;
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    void ReadClockHealth(HealthCallback callback) override;

    /// This firmware fails the INPUT signal-format command when it lands
    /// immediately after the OUTPUT one. Linux waits 100 ms; the vendor kext
    /// waits 300 around its own rate set, and the vendor driver is authoritative
    /// for what the hardware needs. See H8.
    [[nodiscard]] uint32_t SignalFormatInterlockMs() const override { return 300; }

    /// The start choreography is inverted here: signal format must be re-sent
    /// *after* host DMA is running, not only before CMP. See the definition.
    void ConfirmDuplexStart(ConfirmCallback callback) override;

private:
    [[nodiscard]] AudioStreamRuntimeCaps CapsForCurrentFormation() const noexcept;

    const MAudioSpecialModel model_;

    // Mirrors Linux's `struct special_params`. These are driver-side beliefs
    // about device state, not readbacks — the device is never asked to confirm
    // them, so they must only ever change alongside a vendor clock command that
    // actually succeeded.
    //
    // Both default to S/PDIF because that is what Linux's own initialisation
    // sets: avc_maudio_set_special_clk(bebob, 0x03, 0x00, 0x00, 0x00) at
    // bebob_maudio.c:276 — clk_src 0x03, dig_in_fmt 0, dig_out_fmt 0.
    MAudioDigitalFormat captureFormat_{MAudioDigitalFormat::SPDIF};
    MAudioDigitalFormat playbackFormat_{MAudioDigitalFormat::SPDIF};

    // The rate the geometry is currently shaped for. Seeded from the device via
    // the signal-format probe rather than assumed, because this firmware keeps
    // its rate across a host restart and guessing wrong mis-shapes the stream.
    uint32_t currentRateHz_{48000};
};

} // namespace ASFW::Audio::BeBoB
