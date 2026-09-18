// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FireworksProtocol.cpp — see FireworksProtocol.hpp.

#include "FireworksProtocol.hpp"

#include "../../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio::Fireworks {

namespace {

constexpr uint64_t kMillisecond = 1000ULL * 1000ULL;

[[nodiscard]] AudioStreamRuntimeCaps BuildCaps(const FireworksStaticGeometry& geometry) noexcept {
    // MBLA PCM slots plus the AM824 MIDI conformant slot(s): the data block is
    // pcm + ceil(midi ports / 8) per direction (Linux amdtp-am824), so the wire
    // slot count exceeds the CoreAudio channel count — same as the PHASE 88.
    AudioStreamRuntimeCaps caps{};
    caps.hostInputPcmChannels = geometry.captureChannels;
    caps.hostOutputPcmChannels = geometry.playbackChannels;
    caps.deviceToHostAm824Slots = geometry.CaptureAm824Slots();
    caps.hostToDeviceAm824Slots = geometry.PlaybackAm824Slots();
    caps.sampleRateHz = geometry.sampleRateHz;
    caps.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps.deviceToHostStreamCount = 1;
    caps.hostToDeviceStreamCount = 1;
    caps.deviceToHostStreams[0] = {.pcmChannels = geometry.captureChannels,
                                   .am824Slots = geometry.CaptureAm824Slots()};
    caps.hostToDeviceStreams[0] = {.pcmChannels = geometry.playbackChannels,
                                   .am824Slots = geometry.PlaybackAm824Slots()};
    return caps;
}

[[nodiscard]] constexpr uint32_t MidiSlotsForPorts(uint32_t ports) noexcept {
    return (ports + 7U) / 8U;  // Linux amdtp-am824: DIV_ROUND_UP(midi_ports, 8)
}

// Linux fireworks_stream.c:53-56 — firmware 4.6.0 (the last Onyx 1200F release)
// reports a wrong DBS at 88.2 kHz and above; irrelevant at 1x rates but must be
// honoured (trust the configured stride) before any 2x rate is offered.
constexpr uint32_t kWrongDbsAt2xArmVersion = 0x04060000;

} // namespace

FireworksProtocol::FireworksProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                     Protocols::Ports::FireWireBusInfo& busInfo,
                                     Discovery::DeviceRouteToken route,
                                     IRM::IRMClient* irmClient,
                                     CMP::CMPClient* cmpClient,
                                     Scheduling::ITimerScheduler* timerScheduler,
                                     const FireworksStaticGeometry& geometry) noexcept
    : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler),
      geometry_(geometry),
      efc_(EfcTransport::Create(busOps, busInfo, timerScheduler)),
      caps_(BuildCaps(geometry)) {
    efc_->SetRoute(route);
}

FireworksProtocol::~FireworksProtocol() {
    // Production teardown drops the shared_ptr without Shutdown(): fail the
    // clock apply (cancels its timer), abort EFC traffic (cancels the bus write
    // and response timeout), then expire the token the timer lambdas hold.
    CancelClockApply();
    efc_->CancelAll(kIOReturnAborted);
    alive_.reset();
}

IOReturn FireworksProtocol::Initialize() {
    const IOReturn status = BeBoBProtocol::Initialize();
    if (status != kIOReturnSuccess) {
        return status;
    }
    // Fire-and-forget capability probe so the device's real geometry lands in
    // the log at enumeration time, before anyone presses play.
    ProbeHardwareInfo([](IOReturn) {});
    return kIOReturnSuccess;
}

IOReturn FireworksProtocol::Shutdown() {
    ResetDeviceSession();
    return BeBoBProtocol::Shutdown();
}

void FireworksProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                             Protocols::AVC::FCPTransport* transport) {
    const bool routeChanged = (route_ != route);
    BeBoBProtocol::UpdateRuntimeContext(route, transport);
    efc_->SetRoute(route);
    if (routeChanged) {
        ResetDeviceSession();
    }
}

void FireworksProtocol::ResetDeviceSession() noexcept {
    efc_->CancelAll(kIOReturnAborted);
    transportModeSet_ = false;
}

bool FireworksProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = caps_;
    return true;
}

std::vector<uint32_t> FireworksProtocol::SupportedRates() const {
    // Single static rate, same policy as the Onyx-i: the ADK transport cannot
    // yet reconfigure AV/C static-profile devices on a host rate change, and a
    // failed change leaves a stale pending clock behind (field-verified on the
    // 820i, 2026-08-17). The device itself spans hwInfo_ min..max; EFC
    // SET_CLOCK is already wired for when the reconfig path lands.
    return {geometry_.sampleRateHz};
}

void FireworksProtocol::EvaluateGeometry(const Efc::HwInfo& info) {
    // Data-block layout per direction, as Linux keep_resources() derives it:
    // capture (device -> host) = tx PCM + midi_out_ports slots,
    // playback (host -> device) = rx PCM + midi_in_ports slots.
    const uint32_t deviceCapture = info.txPcmChannels[0];
    const uint32_t devicePlayback = info.rxPcmChannels[0];
    const uint32_t deviceCaptureMidi = MidiSlotsForPorts(info.midiOutPorts);
    const uint32_t devicePlaybackMidi = MidiSlotsForPorts(info.midiInPorts);
    const bool matches = deviceCapture == geometry_.captureChannels &&
                         devicePlayback == geometry_.playbackChannels &&
                         deviceCaptureMidi == geometry_.captureMidiSlots &&
                         devicePlaybackMidi == geometry_.playbackMidiSlots;
    geometryCheck_ = matches ? GeometryCheck::kMatched : GeometryCheck::kMismatch;
    if (!matches) {
        ASFW_LOG_ERROR(Audio,
                       "[Fireworks] %{public}s geometry mismatch: device reports capture=%u+%u midi "
                       "playback=%u+%u midi (1x, pcm+slots) but the static profile publishes "
                       "capture=%u+%u playback=%u+%u — streaming disabled; update "
                       "kOnyx400FGeometry / MackieOnyx400FProfile / the nub publisher from this capture",
                       geometry_.name, deviceCapture, deviceCaptureMidi, devicePlayback,
                       devicePlaybackMidi,
                       static_cast<unsigned>(geometry_.captureChannels),
                       static_cast<unsigned>(geometry_.captureMidiSlots),
                       static_cast<unsigned>(geometry_.playbackChannels),
                       static_cast<unsigned>(geometry_.playbackMidiSlots));
    }
    if (info.armVersion == kWrongDbsAt2xArmVersion) {
        ASFW_LOG(Audio, "[Fireworks] firmware 4.6.0: DBS untrustworthy at >= 88.2 kHz "
                        "(Linux CIP_WRONG_DBS) — keep 1x rates until the RX stride is pinned");
    }
}

void FireworksProtocol::ProbeHardwareInfo(SimpleCallback callback) {
    if (hwInfo_) {
        callback(kIOReturnSuccess);
        return;
    }
    efc_->Submit(Efc::Category::kHwInfo,
                static_cast<uint32_t>(Efc::HwInfoCommand::kGetCaps), {},
                [this, callback = std::move(callback)](IOReturn status,
                                                       const Efc::Response& response) mutable {
        if (status != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "[Fireworks] HWINFO GET_CAPS failed status=0x%x", status);
            callback(status);
            return;
        }
        auto info = Efc::ParseHwInfo(response.paramBytes);
        if (!info) {
            ASFW_LOG_ERROR(Audio, "[Fireworks] HWINFO GET_CAPS short response: %zu quadlets",
                           response.ParamQuadletCount());
            callback(kIOReturnIOError);
            return;
        }
        ASFW_LOG(Audio,
                 "[Fireworks] HWINFO vendor=\"%{public}s\" model=\"%{public}s\" type=0x%06x guid=0x%016llx "
                 "flags=0x%08x arm=0x%08x dsp=0x%08x fpga=0x%08x clocks=0x%08x rate=%u..%u",
                 info->vendorName.data(), info->modelName.data(), info->type,
                 static_cast<unsigned long long>(info->guid), info->flags, info->armVersion,
                 info->dspVersion, info->fpgaVersion, info->supportedClocks,
                 info->minSampleRate, info->maxSampleRate);
        ASFW_LOG(Audio,
                 "[Fireworks] HWINFO amdtp capture(tx)=%u/%u/%u playback(rx)=%u/%u/%u (1x/2x/4x%{public}s) "
                 "phys in=%u out=%u midi in=%u out=%u mixer cap=%u pb=%u",
                 info->txPcmChannels[0], info->txPcmChannels[1], info->txPcmChannels[2],
                 info->rxPcmChannels[0], info->rxPcmChannels[1], info->rxPcmChannels[2],
                 info->hasMultiplierCounts ? "" : ", 2x/4x not reported",
                 info->physIn, info->physOut, info->midiInPorts, info->midiOutPorts,
                 info->mixerCaptureChannels, info->mixerPlaybackChannels);
        for (uint32_t i = 0; i < info->physInGroupCount; ++i) {
            ASFW_LOG(Audio, "[Fireworks] HWINFO phys-in group %u: type=%u count=%u", i,
                     info->physInGroups[i].type, info->physInGroups[i].count);
        }
        for (uint32_t i = 0; i < info->physOutGroupCount; ++i) {
            ASFW_LOG(Audio, "[Fireworks] HWINFO phys-out group %u: type=%u count=%u", i,
                     info->physOutGroups[i].type, info->physOutGroups[i].count);
        }
        hwInfo_ = std::move(info);
        EvaluateGeometry(*hwInfo_);
        callback(kIOReturnSuccess);
    });
}

void FireworksProtocol::EnsureTransportMode(SimpleCallback callback) {
    if (transportModeSet_) {
        callback(kIOReturnSuccess);
        return;
    }
    const uint32_t params[] = {static_cast<uint32_t>(Efc::TransportMode::kIec61883)};
    efc_->Submit(Efc::Category::kTransport,
                static_cast<uint32_t>(Efc::TransportCommand::kSetTxMode), params,
                [this, callback = std::move(callback)](IOReturn status, const Efc::Response&) mutable {
        if (status == kIOReturnSuccess) {
            transportModeSet_ = true;
        } else {
            ASFW_LOG_ERROR(Audio, "[Fireworks] TRANSPORT SET_TX_MODE(IEC61883) failed status=0x%x",
                           status);
        }
        callback(status);
    });
}

void FireworksProtocol::ReadClock(ClockCallback callback) {
    efc_->Submit(Efc::Category::kHwCtl,
                static_cast<uint32_t>(Efc::HwCtlCommand::kGetClock), {},
                [this, callback = std::move(callback)](IOReturn status,
                                                       const Efc::Response& response) mutable {
        if (status != kIOReturnSuccess) {
            callback(status, Efc::Clock{});
            return;
        }
        auto clock = Efc::ParseClock(response);
        if (!clock) {
            ASFW_LOG_ERROR(Audio, "[Fireworks] HWCTL GET_CLOCK short response: %zu quadlets",
                           response.ParamQuadletCount());
            callback(kIOReturnIOError, Efc::Clock{});
            return;
        }
        lastClock_ = *clock;
        callback(kIOReturnSuccess, *clock);
    });
}

void FireworksProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                      const AudioClockConfig& desiredClock,
                                      PrepareCallback callback) {
    ProbeHardwareInfo([this, channels, desiredClock, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        if (geometryCheck_ != GeometryCheck::kMatched) {
            ASFW_LOG_ERROR(Audio, "[Fireworks] refusing to prepare duplex: geometry not verified");
            callback(kIOReturnUnsupported, {});
            return;
        }
        BeBoBProtocol::PrepareDuplex(channels, desiredClock, std::move(callback));
    });
}

void FireworksProtocol::CompleteClockApply(const std::shared_ptr<ClockApplyEpoch>& epoch,
                                           IOReturn status) {
    if (activeClockApply_ != epoch.get()) {
        return;  // cancelled (Shutdown / route change) while EFC was in flight
    }
    if (epoch->settleTimer != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(epoch->settleTimer);
        epoch->settleTimer = Scheduling::kInvalidTimerToken;
    }
    if (status == kIOReturnSuccess) {
        appliedClock_ = epoch->appliedClock;
    }
    FinishClockApply(epoch.get(), status);
}

void FireworksProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                         ClockApplyCallback callback) {
    if (!IsRateSupported(desiredClock.sampleRateHz)) {
        ASFW_LOG_ERROR(Audio, "[Fireworks] refusing clock apply: rate %u Hz not in the offered set (%u Hz)",
                       desiredClock.sampleRateHz, geometry_.sampleRateHz);
        callback(kIOReturnUnsupported, {});
        return;
    }
    if (geometryCheck_ == GeometryCheck::kMismatch) {
        ASFW_LOG_ERROR(Audio, "[Fireworks] refusing clock apply: geometry mismatch");
        callback(kIOReturnUnsupported, {});
        return;
    }
    if (timerScheduler_ == nullptr) {
        callback(kIOReturnNotReady, {});
        return;
    }
    CancelClockApply();

    auto epoch = std::make_shared<ClockApplyEpoch>();
    epoch->generation = busInfo_.GetGeneration();
    epoch->completion = std::move(callback);
    epoch->appliedClock = desiredClock;
    activeClockApply_ = epoch.get();

    // Watchdog for the whole EFC sequence; also guarantees the base's
    // CancelClockApply() always has a live timer token to cancel.
    std::weak_ptr<LifetimeToken> alive = alive_;
    epoch->settleTimer = timerScheduler_->ScheduleAfter(
        static_cast<uint64_t>(kClockApplyWatchdogMs) * kMillisecond, [this, alive, epoch]() {
            if (alive.expired()) return;  // protocol destroyed before the timer fired
            if (activeClockApply_ != epoch.get()) return;
            ASFW_LOG_ERROR(Audio, "[Fireworks] clock apply watchdog fired (rate=%u)",
                           epoch->appliedClock.sampleRateHz);
            epoch->settleTimer = Scheduling::kInvalidTimerToken;
            FinishClockApply(epoch.get(), kIOReturnTimeout);
        });
    if (epoch->settleTimer == Scheduling::kInvalidTimerToken) {
        ASFW_LOG_ERROR(Audio, "[Fireworks] could not arm the clock-apply watchdog");
        FinishClockApply(epoch.get(), kIOReturnNotReady);
        return;
    }

    // Linux snd-fireworks order: transport mode once per session, then read the
    // clock and only write it when the rate actually differs (command_set_clock).
    EnsureTransportMode([this, epoch, desiredClock](IOReturn modeStatus) {
        if (activeClockApply_ != epoch.get()) return;
        if (modeStatus != kIOReturnSuccess) {
            CompleteClockApply(epoch, modeStatus);
            return;
        }
        ReadClock([this, epoch, desiredClock](IOReturn readStatus, Efc::Clock current) {
            if (activeClockApply_ != epoch.get()) return;
            if (readStatus != kIOReturnSuccess) {
                CompleteClockApply(epoch, readStatus);
                return;
            }
            if (current.sampleRateHz == desiredClock.sampleRateHz) {
                ASFW_LOG(Audio, "[Fireworks] clock already at %u Hz (source=%u)",
                         current.sampleRateHz, current.source);
                CompleteClockApply(epoch, kIOReturnSuccess);
                return;
            }
            const Efc::Clock wanted{.source = current.source,
                                    .sampleRateHz = desiredClock.sampleRateHz,
                                    .index = 0};
            const auto params = Efc::EncodeClock(wanted);
            ASFW_LOG(Audio, "[Fireworks] HWCTL SET_CLOCK source=%u rate=%u -> %u",
                     current.source, current.sampleRateHz, desiredClock.sampleRateHz);
            efc_->Submit(Efc::Category::kHwCtl,
                        static_cast<uint32_t>(Efc::HwCtlCommand::kSetClock), params,
                        [this, epoch, wanted](IOReturn setStatus, const Efc::Response&) {
                if (activeClockApply_ != epoch.get()) return;
                if (setStatus != kIOReturnSuccess) {
                    CompleteClockApply(epoch, setStatus);
                    return;
                }
                // The firmware reports the old rate for ~100 ms after SET_CLOCK
                // (Linux command_set_clock: 150 ms). Settle before CMP.
                timerScheduler_->Cancel(epoch->settleTimer);
                std::weak_ptr<LifetimeToken> settleAlive = alive_;
                epoch->settleTimer = timerScheduler_->ScheduleAfter(
                    static_cast<uint64_t>(Efc::kClockSettleMs) * kMillisecond,
                    [this, settleAlive, epoch, wanted]() {
                        if (settleAlive.expired()) return;
                        if (activeClockApply_ != epoch.get()) return;
                        epoch->settleTimer = Scheduling::kInvalidTimerToken;
                        lastClock_ = wanted;
                        CompleteClockApply(epoch, kIOReturnSuccess);
                    });
                if (epoch->settleTimer == Scheduling::kInvalidTimerToken) {
                    CompleteClockApply(epoch, kIOReturnNotReady);
                }
            });
        });
    });
}

void FireworksProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    // No PCR read-back. Linux snd-fireworks never re-reads the plug registers
    // after amdtp_domain_start (fireworks_stream.c), and the compare-swap at
    // connect time already proved both plug states. Field-found 2026-09-13: the
    // base class's oPCR read, issued in the same millisecond the host IT
    // context starts, is never answered by the 400F, and the confirm stage then
    // waits out the 12 s sync bridge while the stream is already running.
    if (!cmpClient_ || !inputConnected_ || !outputConnected_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    callback(kIOReturnSuccess,
             DuplexConfirmResult{.generation = busInfo_.GetGeneration(),
                                 .channels = duplexChannels_,
                                 .appliedClock = appliedClock_,
                                 .runtimeCaps = caps_});
}

void FireworksProtocol::ReadClockHealth(HealthCallback callback) {
    ReadClock([this, callback = std::move(callback)](IOReturn status, Efc::Clock clock) mutable {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        const bool rateMatches = clock.sampleRateHz == caps_.sampleRateHz;
        callback(kIOReturnSuccess,
                 DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                    .appliedClock = appliedClock_,
                                    .runtimeCaps = caps_,
                                    .sourceLocked = rateMatches && inputConnected_ && outputConnected_,
                                    .clockReferenceHealthy = rateMatches,
                                    .nominalRateHz = clock.sampleRateHz});
    });
}

} // namespace ASFW::Audio::Fireworks
