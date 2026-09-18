// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FireworksProtocol.hpp — Runtime duplex control for Echo Fireworks devices
// (Mackie Onyx 400F first).
//
// Streaming rides on the shared AV/C+CMP base (plug-0 CMP connections, AM824,
// blocking mode, SYT-unaware — Linux snd-fireworks: CIP_BLOCKING |
// CIP_UNAWARE_SYT). What differs from BeBoB/Oxford is control: sample rate and
// clock source are set through EFC (HWCTL SET_CLOCK), the transport mode must
// be switched to IEC 61883 before streaming (TRANSPORT SET_TX_MODE), and the
// channel geometry is self-described by HWINFO GET_CAPS.
//
// Geometry policy: the ADK profile and the published nub carry a static
// geometry (captured from the spec sheet, not yet from hardware). The first
// HWINFO answer is logged in full and compared against it; on mismatch the
// protocol refuses to stream rather than lock the device to a wrong layout.
// Cross-validated with references/linux-sound-firewire-stack/firewire/fireworks/.

#pragma once

#include "../BeBoB/BeBoBProtocol.hpp"
#include "EfcProtocol.hpp"
#include "EfcTransport.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ASFW::Audio::Fireworks {

struct FireworksStaticGeometry {
    const char* name{"Fireworks"};
    uint16_t captureChannels{0};    // device -> host AMDTP PCM channels (1x)
    uint16_t playbackChannels{0};   // host -> device AMDTP PCM channels (1x)
    // AM824 MIDI conformant slots per direction: DIV_ROUND_UP(midi ports, 8),
    // Linux fireworks_stream.c keep_resources — capture uses midi_out_ports,
    // playback uses midi_in_ports. Part of DBS, invisible to CoreAudio.
    uint8_t captureMidiSlots{0};
    uint8_t playbackMidiSlots{0};
    uint32_t sampleRateHz{44100};

    // uint16_t: AudioStreamWireInfo::am824Slots is 16-bit; DBS is 8-bit on the wire.
    [[nodiscard]] constexpr uint16_t CaptureAm824Slots() const noexcept {
        return static_cast<uint16_t>(captureChannels + captureMidiSlots);
    }
    [[nodiscard]] constexpr uint16_t PlaybackAm824Slots() const noexcept {
        return static_cast<uint16_t>(playbackChannels + playbackMidiSlots);
    }
};

// Mackie Onyx 400F: 8 analog + S/PDIF stereo per direction = 10 x 10 PCM at 1x,
// plus one MIDI in and one MIDI out port => one AM824 MIDI slot per direction
// (DBS 11). Product-spec values, pending hardware capture: HWINFO
// amdtp_tx/rx_pcm_channels and midi_in/out_ports must agree or streaming stays
// off (see GeometryCheck).
inline constexpr FireworksStaticGeometry kOnyx400FGeometry{
    .name = "Mackie Onyx 400F",
    .captureChannels = 10,
    .playbackChannels = 10,
    .captureMidiSlots = 1,
    .playbackMidiSlots = 1,
    .sampleRateHz = 44100,
};

class FireworksProtocol final : public BeBoB::BeBoBProtocol {
public:
    enum class GeometryCheck : uint8_t { kPending, kMatched, kMismatch };

    FireworksProtocol(Protocols::Ports::FireWireBusOps& busOps,
                      Protocols::Ports::FireWireBusInfo& busInfo,
                      Discovery::DeviceRouteToken route,
                      IRM::IRMClient* irmClient,
                      CMP::CMPClient* cmpClient,
                      Scheduling::ITimerScheduler* timerScheduler,
                      const FireworksStaticGeometry& geometry) noexcept;
    ~FireworksProtocol() override;

    const char* GetName() const override { return geometry_.name; }

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;

    [[nodiscard]] const std::optional<Efc::HwInfo>& HardwareInfo() const noexcept { return hwInfo_; }
    [[nodiscard]] GeometryCheck GeometryStatus() const noexcept { return geometryCheck_; }
    [[nodiscard]] const std::optional<Efc::Clock>& LastClock() const noexcept { return lastClock_; }
    [[nodiscard]] bool TransportModeSet() const noexcept { return transportModeSet_; }
    [[nodiscard]] EfcTransport& Transport() noexcept { return efc_; }

protected:
    const char* DeviceName() const override { return geometry_.name; }
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    void ReadClockHealth(HealthCallback callback) override;

private:
    using SimpleCallback = std::function<void(IOReturn)>;
    using ClockCallback = std::function<void(IOReturn, Efc::Clock)>;

    void ProbeHardwareInfo(SimpleCallback callback);
    void EnsureTransportMode(SimpleCallback callback);
    void ReadClock(ClockCallback callback);
    void EvaluateGeometry(const Efc::HwInfo& info);
    void CompleteClockApply(const std::shared_ptr<ClockApplyEpoch>& epoch, IOReturn status);
    void ResetDeviceSession() noexcept;

    static constexpr uint32_t kClockApplyWatchdogMs = 2000;

    struct LifetimeToken {};

    FireworksStaticGeometry geometry_{};
    EfcTransport efc_;
    AudioStreamRuntimeCaps caps_{};
    std::optional<Efc::HwInfo> hwInfo_{};
    std::optional<Efc::Clock> lastClock_{};
    GeometryCheck geometryCheck_{GeometryCheck::kPending};
    bool transportModeSet_{false};
    // Weakly captured by the clock-apply timers: production teardown drops the
    // protocol's shared_ptr without calling Shutdown(), so a timer that fires
    // after destruction must find an expired token, not a dangling `this`.
    std::shared_ptr<LifetimeToken> alive_{std::make_shared<LifetimeToken>()};
};

} // namespace ASFW::Audio::Fireworks
