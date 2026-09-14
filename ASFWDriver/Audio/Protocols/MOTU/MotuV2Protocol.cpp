// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV2Protocol.cpp - MOTU protocol-v2 register device protocol.
//
// Register semantics cross-validated with Linux sound/firewire/motu
// (motu-protocol-v2.c, motu-transaction.c) and confirmed against an 828mkII on
// 2026-07-26: clock status read back 0x00000008 (48 kHz, internal) matching the
// device's own front panel, and a rate write moved the hardware rate display.

#include "MotuV2Protocol.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../DeviceProfiles/Audio/Vendors/MotuAudioProfiles.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"
#include "../../Wire/MOTU/MotuRegisterDsp.hpp"

namespace ASFW::Audio::Motu {

namespace {
constexpr uint16_t kAddrHi = static_cast<uint16_t>(kAddrBase >> 32);
constexpr uint32_t kAddrLo = static_cast<uint32_t>(kAddrBase);
} // namespace

Async::FWAddress MotuV2Protocol::AddressOf(Reg reg) noexcept {
    return Async::FWAddress(Async::FWAddress::AddressParts{
        .addressHi = kAddrHi,
        .addressLo = kAddrLo + static_cast<uint32_t>(reg)});
}

MotuV2Protocol::MotuV2Protocol(Protocols::Ports::FireWireBusOps& busOps,
                               Protocols::Ports::FireWireBusInfo& busInfo,
                               Discovery::DeviceRegistry& routeRegistry,
                               const Discovery::DeviceRouteToken& route,
                               uint32_t unitSwVersion,
                               ::ASFW::IRM::IRMClient* irmClient,
                               ::ASFW::Scheduling::ITimerScheduler* timerScheduler)
    : io_(busOps, busInfo, routeRegistry, route)
    , busInfo_(busInfo)
    , irmClient_(irmClient)
    , unitSwVersion_(unitSwVersion)
    , mainVolumeWriter_(
          [this](uint8_t value, MotuLevelWriter::DoneFn done) {
              ASFW_LOG(Audio, "MotuV2Protocol: main volume -> raw=0x%02x centiDb=%d", value,
                       static_cast<int>(Encoding::Motu::OutputVolumeToDb(value) * 100.0f));
              (void)io_.WriteQuadBE(
                  AddressOf(Reg::MainOutputVolume),
                  value,
                  [this, value, done = std::move(done)](Async::AsyncStatus status) {
                      const bool ok = status == Async::AsyncStatus::kSuccess;
                      if (ok) {
                          mainVolumeRaw_.store(value, std::memory_order_release);
                      } else {
                          ASFW_LOG(Audio,
                                   "MotuV2Protocol: main volume write 0x%02x failed status=%u",
                                   value, static_cast<unsigned>(status));
                      }
                      done(ok);
                  });
          },
          timerScheduler) {}

const char* MotuV2Protocol::GetName() const {
    const char* const model =
        DeviceProfiles::Audio::Motu::ModelNameForSwVersion(unitSwVersion_);
    return model != nullptr ? model : "MOTU (protocol v2)";
}

IOReturn MotuV2Protocol::Initialize() {
    initialized_ = true;

    // Prime the cached clock state. Best-effort: a failure here leaves the cache at 0
    // ("not yet known") rather than failing device creation, since every consumer reads
    // the register on demand anyway.
    ReadClockStatus([](IOReturn status, ClockStatus clock) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(Audio, "MotuV2Protocol: initial clock read failed: 0x%x", status);
            return;
        }
        ASFW_LOG(Audio,
                 "MotuV2Protocol: clock status raw=0x%08x rate=%uHz source=%u",
                 clock.raw,
                 clock.sampleRateHz,
                 clock.source.has_value() ? static_cast<uint32_t>(*clock.source) : 0xFFFFFFFFu);
    });

    // Seed the volume control's initial value. Best-effort like the clock read: without it
    // the control starts from a default until the capture stream reports the knob.
    if (HasMainOutputVolume()) {
        (void)io_.ReadQuadBE(
            AddressOf(Reg::MainOutputVolume),
            [this](Async::AsyncStatus status, uint32_t value) {
                if (status != Async::AsyncStatus::kSuccess) {
                    ASFW_LOG(Audio, "MotuV2Protocol: initial main volume read failed status=%u",
                             static_cast<unsigned>(status));
                    return;
                }
                const auto raw = static_cast<uint8_t>(value & 0xFFU);
                mainVolumeRaw_.store(raw > Encoding::Motu::kOutputVolumeMaxRaw
                                         ? Encoding::Motu::kOutputVolumeMaxRaw
                                         : raw,
                                     std::memory_order_release);
            });
    }

    return kIOReturnSuccess;
}

IOReturn MotuV2Protocol::Shutdown() {
    initialized_ = false;
    cachedSampleRateHz_.store(0, std::memory_order_release);

    // Hand the device back to its front panel. Fire-and-forget: Shutdown is synchronous,
    // and a device that has already gone away cannot be released anyway.
    if (asyncAddressRegistered_.load(std::memory_order_acquire)) {
        ReleaseAsyncMessageAddress([](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: async address release failed: 0x%x", status);
            }
        });
    }

    return kIOReturnSuccess;
}

void MotuV2Protocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                          Protocols::AVC::FCPTransport* transport) {
    (void)transport; // MOTU v2 is register-based; no AV/C transport.
    io_.UpdateRoute(route);
}

void MotuV2Protocol::ReadClockStatus(ClockStatusCallback callback) {
    (void)io_.ReadQuadBE(
        AddressOf(Reg::ClockStatusV2),
        [this, callback = std::move(callback)](Async::AsyncStatus status, uint32_t value) mutable {
            const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (result != kIOReturnSuccess) {
                if (callback) {
                    callback(result, ClockStatus{});
                }
                return;
            }

            ClockStatus clock{};
            clock.raw = value;
            clock.sampleRateHz = DecodeRateV2(value).value_or(0U);
            clock.source = DecodeClockSourceV2(value);
            cachedSampleRateHz_.store(clock.sampleRateHz, std::memory_order_release);

            if (callback) {
                callback(kIOReturnSuccess, clock);
            }
        });
}

void MotuV2Protocol::SetSampleRate(uint32_t rateHz, CompletionCallback callback) {
    // Read-modify-write: the rate lives in bits [5:3] and must not disturb the clock
    // source or any other field (motu-protocol-v2.c:59-86).
    ReadClockStatus([this, rateHz, callback = std::move(callback)](
                        IOReturn status, ClockStatus clock) mutable {
        if (status != kIOReturnSuccess) {
            if (callback) {
                callback(status);
            }
            return;
        }

        const auto encoded = EncodeRateV2(clock.raw, rateHz);
        if (!encoded.has_value()) {
            ASFW_LOG(Audio, "MotuV2Protocol: unsupported sample rate %u", rateHz);
            if (callback) {
                callback(kIOReturnUnsupported);
            }
            return;
        }

        if (*encoded == clock.raw) {
            if (callback) {
                callback(kIOReturnSuccess);
            }
            return;
        }

        (void)io_.WriteQuadBE(
            AddressOf(Reg::ClockStatusV2),
            *encoded,
            [this, rateHz, callback = std::move(callback)](Async::AsyncStatus writeStatus) mutable {
                const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(writeStatus);
                if (result == kIOReturnSuccess) {
                    cachedSampleRateHz_.store(rateHz, std::memory_order_release);
                }
                if (callback) {
                    callback(result);
                }
            });
    });
}

void MotuV2Protocol::WriteAsyncAddrPair(AsyncAddrValues values,
                                        bool registered,
                                        CompletionCallback callback) {
    (void)io_.WriteQuadBE(
        AddressOf(Reg::AsyncAddrHi),
        values.hi,
        [this, values, registered, callback = std::move(callback)](
            Async::AsyncStatus hiStatus) mutable {
            const IOReturn hiResult = Protocols::Ports::MapAsyncStatusToIOReturn(hiStatus);
            if (hiResult != kIOReturnSuccess) {
                if (callback) {
                    callback(hiResult);
                }
                return;
            }

            (void)io_.WriteQuadBE(
                AddressOf(Reg::AsyncAddrLo),
                values.lo,
                [this, registered, callback = std::move(callback)](
                    Async::AsyncStatus loStatus) mutable {
                    const IOReturn loResult =
                        Protocols::Ports::MapAsyncStatusToIOReturn(loStatus);
                    if (loResult == kIOReturnSuccess) {
                        asyncAddressRegistered_.store(registered, std::memory_order_release);
                    }
                    if (callback) {
                        callback(loResult);
                    }
                });
        });
}

void MotuV2Protocol::RegisterAsyncMessageAddress(uint16_t hostNodeId,
                                                 uint64_t hostAddress,
                                                 CompletionCallback callback) {
    if (hostAddress < kAsyncMessageRegionStart || hostAddress > kAsyncMessageRegionEnd) {
        ASFW_LOG(Audio,
                 "MotuV2Protocol: async address 0x%012llx outside the device's accepted region",
                 hostAddress);
        if (callback) {
            callback(kIOReturnBadArgument);
        }
        return;
    }

    WriteAsyncAddrPair(EncodeAsyncAddr(hostNodeId, hostAddress), true, std::move(callback));
}

void MotuV2Protocol::ReleaseAsyncMessageAddress(CompletionCallback callback) {
    WriteAsyncAddrPair(AsyncAddrValues{.hi = 0U, .lo = 0U}, false, std::move(callback));
}

//==============================================================================
// Duplex bring-up
//==============================================================================

void MotuV2Protocol::ModifyRegister(Reg reg,
                                    std::function<uint32_t(uint32_t)> transform,
                                    CompletionCallback callback) {
    (void)io_.ReadQuadBE(
        AddressOf(reg),
        [this, reg, transform = std::move(transform), callback = std::move(callback)](
            Async::AsyncStatus status, uint32_t current) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                if (callback) {
                    callback(readResult);
                }
                return;
            }
            (void)io_.WriteQuadBE(
                AddressOf(reg),
                transform(current),
                [callback = std::move(callback)](Async::AsyncStatus writeStatus) mutable {
                    if (callback) {
                        callback(Protocols::Ports::MapAsyncStatusToIOReturn(writeStatus));
                    }
                });
        });
}

bool MotuV2Protocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = MakeRuntimeCaps();
    if (outCaps.sampleRateHz == 0) {
        // Not prepared yet: answer from the model's fixed layout so the nub can be
        // published. The v2 fixed-chunk models carry 14 PCM chunks per direction at
        // 44.1/48 kHz (motu-protocol-v2.c:274-282). Prefer the rate the device last
        // reported over assuming 48k.
        const uint32_t cachedRate = cachedSampleRateHz_.load(std::memory_order_acquire);
        outCaps.hostInputPcmChannels = Encoding::Motu::k828mk2FixedPcmChunks[0];
        outCaps.hostOutputPcmChannels = Encoding::Motu::k828mk2FixedPcmChunks[0];
        outCaps.deviceToHostPcmChunks = outCaps.hostInputPcmChannels;
        outCaps.hostToDevicePcmChunks = outCaps.hostOutputPcmChannels;
        outCaps.sampleRateHz = cachedRate != 0 ? cachedRate : 48000U;
    }
    return true;
}

bool MotuV2Protocol::GetChannelLabels(std::vector<std::string>& inNames,
                                      std::vector<std::string>& outNames) const {
    const Encoding::Motu::MotuPortMap capture =
        Encoding::Motu::CapturePortsForSwVersion(unitSwVersion_);
    const Encoding::Motu::MotuPortMap playback =
        Encoding::Motu::PlaybackPortsForSwVersion(unitSwVersion_);
    if (capture.empty() && playback.empty()) {
        return false;
    }
    inNames.clear();
    outNames.clear();
    for (const Encoding::Motu::MotuPort& port : capture) {
        inNames.emplace_back(port.name);
    }
    for (const Encoding::Motu::MotuPort& port : playback) {
        outNames.emplace_back(port.name);
    }
    return true;
}

bool MotuV2Protocol::HasMainOutputVolume() const noexcept {
    // Register-DSP v2 models with a 0x0c0c main volume (FFADO MixerCtrls_828Mk2 and
    // MixerCtrls_Ultralite, motu_mixerdefs.cpp:121-127, :230-236). Others stay unmapped until
    // their layouts are confirmed.
    return unitSwVersion_ == DeviceProfiles::Audio::kMotu828mk2SwVersion ||
           unitSwVersion_ == DeviceProfiles::Audio::kMotuUltraliteSwVersion;
}

bool MotuV2Protocol::DescribeControl(const ControlKey& key, ControlInfo& outInfo) const {
    if (key != kMasterOutputVolumeKey || !HasMainOutputVolume()) {
        return false;
    }
    outInfo = ControlInfo{.isSettable = true,
                          .minDecibels = Encoding::Motu::kOutputVolumeMinDb,
                          .maxDecibels = Encoding::Motu::kOutputVolumeMaxDb};
    return true;
}

IOReturn MotuV2Protocol::ReadControl(const ControlKey& key, ControlValue& outValue) {
    if (key != kMasterOutputVolumeKey || !HasMainOutputVolume()) {
        return kIOReturnUnsupported;
    }
    const int32_t raw = mainVolumeRaw_.load(std::memory_order_acquire);
    if (raw < 0) {
        return kIOReturnNotReady;
    }
    outValue = ControlValue{.kind = ControlKind::kLevel,
                            .decibels = Encoding::Motu::OutputVolumeToDb(static_cast<uint8_t>(raw))};
    return kIOReturnSuccess;
}

IOReturn MotuV2Protocol::WriteControl(const ControlKey& key, const ControlValue& value) {
    if (key != kMasterOutputVolumeKey || !HasMainOutputVolume()) {
        return kIOReturnUnsupported;
    }
    if (value.kind != ControlKind::kLevel) {
        return kIOReturnBadArgument;
    }
    mainVolumeWriter_.Set(Encoding::Motu::OutputVolumeFromDb(value.decibels));
    return kIOReturnSuccess;
}

AudioStreamRuntimeCaps MotuV2Protocol::MakeRuntimeCaps() const noexcept {
    AudioStreamRuntimeCaps caps{};
    caps.hostInputPcmChannels = txPcmChunks_.load(std::memory_order_acquire);
    caps.hostOutputPcmChannels = rxPcmChunks_.load(std::memory_order_acquire);
    // MOTU does not carry AM824 slots: its data blocks are 3-byte chunks past an SPH
    // quadlet. The slot fields stay zero so nothing mistakes this for an AM824 stream.
    caps.deviceToHostAm824Slots = 0;
    caps.hostToDeviceAm824Slots = 0;
    caps.deviceToHostPcmChunks = caps.hostInputPcmChannels;
    caps.hostToDevicePcmChunks = caps.hostOutputPcmChannels;
    caps.sampleRateHz = preparedRateHz_.load(std::memory_order_acquire);
    return caps;
}

void MotuV2Protocol::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    // IRM allocation can replace the provisional numbers after PrepareDuplex, and the
    // device must be told the committed values before ProgramTxAndEnableDuplex writes
    // them into the iso-comm register.
    deviceRxChannel_.store(channels.PlaybackChannel(0), std::memory_order_release);
    deviceTxChannel_.store(channels.CaptureChannel(0), std::memory_order_release);
}

void MotuV2Protocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                   const AudioClockConfig& desiredClock,
                                   PrepareCallback callback) {
    SetAssignedChannels(channels);

    const uint32_t rateHz = desiredClock.sampleRateHz != 0U ? desiredClock.sampleRateHz : 48000U;
    const int32_t rateIndex = Encoding::Motu::RateToIndex(rateHz);
    if (rateIndex < 0) {
        ASFW_LOG(Audio, "MotuV2Protocol: unsupported duplex rate %u", rateHz);
        if (callback) {
            callback(kIOReturnUnsupported, DuplexPrepareResult{});
        }
        return;
    }
    const uint32_t mode = Encoding::Motu::IndexToMode(static_cast<uint32_t>(rateIndex));
    const uint32_t fixedChunks = Encoding::Motu::k828mk2FixedPcmChunks[mode];
    if (fixedChunks == 0U) {
        // Mode 2 (176.4/192k) is not implemented by the v2 fixed-chunk models.
        ASFW_LOG(Audio, "MotuV2Protocol: rate %u has no chunk layout for this model", rateHz);
        if (callback) {
            callback(kIOReturnUnsupported, DuplexPrepareResult{});
        }
        return;
    }

    // The exclude-differed-chunks bit per direction is only correct when that direction
    // carries just its fixed chunk count, and ADAT adds chunks on top of the baseline
    // (motu-protocol-v2.c:253-269) -- so the optical config has to be read before the
    // packet format can be computed (motu-stream.c:201-225). Capture (TX, device->host)
    // follows the optical input; playback (RX) follows the optical output.
    //
    // FW::Speed's enumerators are the IEEE 1394-1995 wire codes, which is what this
    // register field expects -- the same value Linux takes from
    // fw_parent_device(unit)->max_speed (motu-stream.c:218).
    const uint32_t speedCode = static_cast<uint32_t>(busInfo_.GetSpeed(io_.NodeId()));

    // Set the device's clock rate before anything else in the prepare stage. Linux does
    // exactly this in snd_motu_stream_reserve_duplex (motu-stream.c:143-164): read the
    // current rate, and write the requested one before caching packet formats and keeping
    // iso resources.
    //
    // ASFW's start path never applied the clock at all -- ApplyClockConfig is only reached
    // from DuplexStartTransaction::ApplyIdleClock, which RunDuplexStart does not call. The
    // UltraLite therefore stayed on whatever rate it powered up with (44.1 kHz, clock
    // status reading 0x00000000) while the host had negotiated 48 kHz. WaitForStableGlobalClock
    // requires nominalRateHz == desiredClock.sampleRateHz, so it could never succeed: it
    // spun for its full 1000 ms budget on every attempt. That wait runs inside a
    // DispatchSync chain rooted in coreaudiod's StartDevice external method, so overrunning
    // it makes the HAL abandon the call with kIOReturnTimeout -- surfacing to CoreAudio as
    // _TellHardwareToStart returning 0x77686174 ('what') with no failure logged by us.
    SetSampleRate(
        rateHz,
        [this, speedCode, rateHz, mode, fixedChunks, callback = std::move(callback)](
            IOReturn rateStatus) mutable {
            if (rateStatus != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: set clock rate %u failed: 0x%x", rateHz,
                         rateStatus);
                if (callback) {
                    callback(rateStatus, DuplexPrepareResult{});
                }
                return;
            }
            ASFW_LOG(Audio, "MotuV2Protocol: clock rate set to %u before prepare", rateHz);

    (void)io_.ReadQuadBE(
        AddressOf(Reg::InOutConfV2),
        [this, speedCode, rateHz, mode, fixedChunks, callback = std::move(callback)](
            Async::AsyncStatus status, uint32_t optRaw) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: optical config read failed: 0x%x", readResult);
                if (callback) {
                    callback(readResult, DuplexPrepareResult{});
                }
                return;
            }

            // A reserved encoding means we cannot prove the chunk counts. Treat that as
            // "differed chunks may be present" (clear both bits) rather than guessing the
            // device is in the fixed layout: claiming fixed when it is not truncates the
            // stream, while the conservative direction only costs the optimisation.
            const auto optical = DecodeOptIfaceConfig(optRaw);
            const bool inputIsAdat = optical.has_value() && optical->input == OptIfaceMode::Adat;
            const bool outputIsAdat = optical.has_value() && optical->output == OptIfaceMode::Adat;
            const bool txOnlyFixedChunks = optical.has_value() && !inputIsAdat;
            const bool rxOnlyFixedChunks = optical.has_value() && !outputIsAdat;

            const uint32_t adatExtra = Encoding::Motu::AdatExtraChunks(mode);
            txPcmChunks_.store(fixedChunks + (inputIsAdat ? adatExtra : 0U),
                               std::memory_order_release);
            rxPcmChunks_.store(fixedChunks + (outputIsAdat ? adatExtra : 0U),
                               std::memory_order_release);
            preparedRateHz_.store(rateHz, std::memory_order_release);

            ASFW_LOG(Audio,
                     "MotuV2Protocol: prepare duplex rate=%u opt=0x%08x txChunks=%u rxChunks=%u "
                     "speed=%u rxCh=%u txCh=%u",
                     rateHz,
                     optRaw,
                     txPcmChunks_.load(std::memory_order_acquire),
                     rxPcmChunks_.load(std::memory_order_acquire),
                     speedCode,
                     deviceRxChannel_.load(std::memory_order_acquire),
                     deviceTxChannel_.load(std::memory_order_acquire));

            ModifyRegister(
                Reg::PacketFormat,
                [speedCode, txOnlyFixedChunks, rxOnlyFixedChunks](uint32_t current) {
                    return EncodePacketFormat(current, txOnlyFixedChunks, rxOnlyFixedChunks,
                                              speedCode);
                },
                [this, rateHz, callback = std::move(callback)](IOReturn status) mutable {
                    if (status != kIOReturnSuccess) {
                        if (callback) {
                            callback(status, DuplexPrepareResult{});
                        }
                        return;
                    }
                    duplexPrepared_.store(true, std::memory_order_release);
                    ApplyFetchingModeIfNeeded(true, [](IOReturn) {});

                    DuplexPrepareResult result{};
                    result.generation = busInfo_.GetGeneration();
                    result.channels.deviceToHostIsoChannel =
                        deviceTxChannel_.load(std::memory_order_acquire);
                    result.channels.hostToDeviceIsoChannel =
                        deviceRxChannel_.load(std::memory_order_acquire);
                    result.appliedClock = AudioClockConfig{.sampleRateHz = rateHz};
                    result.runtimeCaps = MakeRuntimeCaps();
                    if (callback) {
                        callback(kIOReturnSuccess, result);
                    }
                });
        });
        });
}

void MotuV2Protocol::ApplyFetchingModeIfNeeded(bool enable,
                                               CompletionCallback callback) {
    // 828mk2 and 896HD need no fetching-mode write; the UltraLite and 8pre implement a
    // Xilinx Spartan XC3S200 and do (motu-protocol-v2.c:190-225). Fire-and-forget by
    // design: this rides alongside bring-up, and a device that has gone away cannot be
    // configured anyway.
    if (!NeedsFetchingModeWrite(unitSwVersion_)) {
        if (callback) {
            callback(kIOReturnSuccess);
        }
        return;
    }
    const bool spartan = true; // every model reaching here is Spartan-based
    ModifyRegister(
        Reg::ClockStatusV2,
        [enable, spartan](uint32_t current) {
            return EncodeFetchingMode(current, enable, spartan);
        },
        [callback = std::move(callback)](IOReturn status) mutable {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: fetching-mode write failed: 0x%x", status);
            }
            if (callback) {
                callback(status);
            }
        });
}

void MotuV2Protocol::ProgramRx(StageCallback callback) {
    // No device-side RX-only step exists on v2: both directions are activated together
    // in the single iso-comm write issued by ProgramTxAndEnableDuplex
    // (motu-stream.c:62-83, begin_session). The stage stays a success so the
    // coordinator's RX-then-TX ordering is preserved for protocols that need it.
    DuplexStageResult result{};
    result.generation = busInfo_.GetGeneration();
    result.channels.deviceToHostIsoChannel = deviceTxChannel_.load(std::memory_order_acquire);
    result.channels.hostToDeviceIsoChannel = deviceRxChannel_.load(std::memory_order_acquire);
    result.runtimeCaps = MakeRuntimeCaps();
    if (callback) {
        callback(kIOReturnSuccess, result);
    }
}

void MotuV2Protocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!duplexPrepared_.load(std::memory_order_acquire)) {
        // Without PrepareDuplex the iso channels are unknown, and writing channel 0/0
        // would point the device at whatever is on those channels.
        ASFW_LOG(Audio, "MotuV2Protocol: enable requested before prepare");
        if (callback) {
            callback(kIOReturnNotReady, DuplexStageResult{});
        }
        return;
    }

    const uint32_t rxChannel = deviceRxChannel_.load(std::memory_order_acquire);
    const uint32_t txChannel = deviceTxChannel_.load(std::memory_order_acquire);

    ModifyRegister(
        Reg::IsocCommControl,
        [rxChannel, txChannel](uint32_t current) {
            return EncodeIsoCommStart(current, rxChannel, txChannel);
        },
        [this, rxChannel, txChannel, callback = std::move(callback)](IOReturn status) mutable {
            if (status != kIOReturnSuccess) {
                if (callback) {
                    callback(status, DuplexStageResult{});
                }
                return;
            }
            duplexActive_.store(true, std::memory_order_release);

            DuplexStageResult result{};
            result.generation = busInfo_.GetGeneration();
            result.channels.hostToDeviceIsoChannel = static_cast<uint8_t>(rxChannel);
            result.channels.deviceToHostIsoChannel = static_cast<uint8_t>(txChannel);
            result.runtimeCaps = MakeRuntimeCaps();
            if (callback) {
                callback(kIOReturnSuccess, result);
            }
        });
}

void MotuV2Protocol::ConfirmDuplexStart(ConfirmCallback callback) {
    // Read the iso-comm register back and require the device to report both directions
    // activated on the channels we asked for. The write completing only means the
    // transaction was accepted.
    const uint32_t expectedRx = deviceRxChannel_.load(std::memory_order_acquire);
    const uint32_t expectedTx = deviceTxChannel_.load(std::memory_order_acquire);

    (void)io_.ReadQuadBE(
        AddressOf(Reg::IsocCommControl),
        [this, expectedRx, expectedTx, callback = std::move(callback)](Async::AsyncStatus status,
                                                                       uint32_t value) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                if (callback) {
                    callback(readResult, DuplexConfirmResult{});
                }
                return;
            }

            const IsoCommState state = DecodeIsoCommState(value);
            const bool ok = state.rxActivated && state.txActivated &&
                            state.rxChannel == expectedRx && state.txChannel == expectedTx;
            if (!ok) {
                ASFW_LOG(Audio,
                         "MotuV2Protocol: duplex not confirmed raw=0x%08x rx=%u/%u tx=%u/%u",
                         value,
                         state.rxActivated ? 1U : 0U,
                         expectedRx,
                         state.txActivated ? 1U : 0U,
                         expectedTx);
            }

            DuplexConfirmResult result{};
            result.generation = busInfo_.GetGeneration();
            result.channels.hostToDeviceIsoChannel = static_cast<uint8_t>(expectedRx);
            result.channels.deviceToHostIsoChannel = static_cast<uint8_t>(expectedTx);
            result.appliedClock =
                AudioClockConfig{.sampleRateHz = preparedRateHz_.load(std::memory_order_acquire)};
            result.runtimeCaps = MakeRuntimeCaps();
            result.status = value;
            if (callback) {
                callback(ok ? kIOReturnSuccess : kIOReturnNotReady, result);
            }
        });
}

void MotuV2Protocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                      ClockApplyCallback callback) {
    const uint32_t rateHz = desiredClock.sampleRateHz;
    SetSampleRate(rateHz, [this, rateHz, callback = std::move(callback)](IOReturn status) mutable {
        if (status == kIOReturnSuccess) {
            preparedRateHz_.store(rateHz, std::memory_order_release);
        }
        ClockApplyResult result{};
        result.generation = busInfo_.GetGeneration();
        result.appliedClock = AudioClockConfig{.sampleRateHz = CachedSampleRateHz()};
        result.runtimeCaps = MakeRuntimeCaps();
        if (callback) {
            callback(status, result);
        }
    });
}

void MotuV2Protocol::ReadDuplexHealth(HealthCallback callback) {
    // v2 has no notification mailbox or lock-status register: the clock status word is
    // the only health evidence the device publishes. A decodable rate and source is
    // reported as locked; anything unreadable stays unlocked so a needed recovery is
    // never suppressed on missing evidence.
    ReadClockStatus([this, callback = std::move(callback)](IOReturn status,
                                                           ClockStatus clock) mutable {
        DuplexHealthResult result{};
        result.generation = busInfo_.GetGeneration();
        result.runtimeCaps = MakeRuntimeCaps();

        if (status != kIOReturnSuccess) {
            result.sourceLocked = false;
            result.clockReferenceHealthy = false;
            if (callback) {
                callback(status, result);
            }
            return;
        }

        const bool decoded = clock.sampleRateHz != 0U && clock.source.has_value();
        result.sourceLocked = decoded;
        result.clockReferenceHealthy = decoded;
        result.nominalRateHz = clock.sampleRateHz;
        result.appliedClock = AudioClockConfig{.sampleRateHz = clock.sampleRateHz};
        result.status = clock.raw;
        if (callback) {
            callback(kIOReturnSuccess, result);
        }
    });
}

IOReturn MotuV2Protocol::StopDuplex() {
    if (!duplexActive_.exchange(false, std::memory_order_acq_rel)) {
        return kIOReturnSuccess;
    }
    duplexPrepared_.store(false, std::memory_order_release);

    // Synchronous hook over an async transport: issue the deactivate and report that it
    // was dispatched. A device that has already gone away cannot be deactivated anyway,
    // which is the same fire-and-forget contract Shutdown() uses for the async address.
    ModifyRegister(
        Reg::IsocCommControl,
        [](uint32_t current) { return EncodeIsoCommStop(current); },
        [](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: iso-comm stop failed: 0x%x", status);
            }
        });

    return kIOReturnSuccess;
}

} // namespace ASFW::Audio::Motu
