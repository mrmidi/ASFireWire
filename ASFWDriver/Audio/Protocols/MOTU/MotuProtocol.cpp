// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuProtocol.cpp - Shared MOTU V1/V2/V3 register device protocol.
//
// Register semantics cross-validated with Linux sound/firewire/motu
// (motu-protocol-v2.c, motu-transaction.c) and confirmed against an 828mkII on
// 2026-07-26: clock status read back 0x00000008 (48 kHz, internal) matching the
// device's own front panel, and a rate write moved the hardware rate display.

#include "MotuProtocol.hpp"

#include "../Duplex/FamilyStageWait.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../DriverKit/Config/MOTU/MotuProfile.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"

namespace ASFW::Audio::Motu {

namespace {
constexpr uint16_t kAddrHi = static_cast<uint16_t>(kAddrBase >> 32);
constexpr uint32_t kAddrLo = static_cast<uint32_t>(kAddrBase);
} // namespace

Async::FWAddress MotuProtocol::AddressOf(Reg reg) noexcept {
    return Async::FWAddress(Async::FWAddress::AddressParts{
        .addressHi = kAddrHi,
        .addressLo = kAddrLo + static_cast<uint32_t>(reg)});
}

MotuProtocol::MotuProtocol(Protocols::Ports::FireWireBusOps& busOps,
                               Protocols::Ports::FireWireBusInfo& busInfo,
                               Discovery::DeviceRegistry& routeRegistry,
                               const Discovery::DeviceRouteToken& route,
                               uint32_t unitSwVersion,
                               ::ASFW::IRM::IRMClient* irmClient,
                               Scheduling::ITimerScheduler* timerScheduler)
    : io_(busOps, busInfo, routeRegistry, route)
    , busInfo_(busInfo)
    , irmClient_(irmClient)
    , unitSwVersion_(unitSwVersion), timerScheduler_(timerScheduler), route_(route) {
    if (IsV3() && Encoding::Motu::FireWireOnly(unitSwVersion_)) {
        notifications_ = std::make_shared<NotificationMailbox>(route);
        notificationAddress_ = Notifications::Register(notifications_);
    }
}
MotuProtocol::~MotuProtocol() {
    if (notifications_ && notifications_->Valid()) notifications_->Cancel();
}

bool MotuProtocol::IsV3() const noexcept {
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    return model && model->protocol == Encoding::Motu::ProtocolVersion::V3;
}

const char* MotuProtocol::GetName() const {
    const char* const model =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(unitSwVersion_);
    return model != nullptr ? model : "MOTU (FireWire)";
}

IOReturn MotuProtocol::Initialize() {
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    if (!model || !Encoding::Motu::FireWireOnly(unitSwVersion_)) return kIOReturnUnsupported;
    initialized_ = true;

    // Prime the cached clock state. Best-effort: a failure here leaves the cache at 0
    // ("not yet known") rather than failing device creation, since every consumer reads
    // the register on demand anyway.
    ReadClockStatus([](IOReturn status, ClockStatus clock) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(Audio, "MotuProtocol: initial clock read failed: 0x%x", status);
            return;
        }
        ASFW_LOG(Audio,
                 "MotuProtocol: clock status raw=0x%08x rate=%uHz source=%u",
                 clock.raw,
                 clock.sampleRateHz,
                 clock.source.has_value() ? static_cast<uint32_t>(*clock.source) : 0xFFFFFFFFu);
    });

    return kIOReturnSuccess;
}

IOReturn MotuProtocol::Shutdown() {
    initialized_ = false;
    shuttingDown_.store(true, std::memory_order_release);
    if (notifications_ && notifications_->Valid()) notifications_->Cancel();
    cachedSampleRateHz_.store(0, std::memory_order_release);

    // Hand the device back to its front panel. Fire-and-forget: Shutdown is synchronous,
    // and a device that has already gone away cannot be released anyway.
    if (asyncAddressRegistered_.exchange(false, std::memory_order_acq_rel)) {
        // Shutdown does not wait on Default. Own a copy of the IO route for
        // these two completions; neither may retain this protocol's address.
        const auto releaseIo = std::make_shared<Protocols::Ports::ProtocolRegisterIO>(io_);
        const auto report = [](Async::AsyncStatus status) {
            const auto result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (result != kIOReturnSuccess)
                ASFW_LOG(Audio, "MotuProtocol: async address release failed: %{public}s (0x%x)",
                         Logging::IOReturnName(result), result);
        };
        (void)releaseIo->WriteQuadBE(AddressOf(Reg::AsyncAddrHi), 0,
            [releaseIo, report](Async::AsyncStatus status) {
                if (status != Async::AsyncStatus::kSuccess) { report(status); return; }
                (void)releaseIo->WriteQuadBE(AddressOf(Reg::AsyncAddrLo), 0,
                    [releaseIo, report](Async::AsyncStatus status) { report(status); });
            });
    }

    return kIOReturnSuccess;
}

void MotuProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                          std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    (void)avcUnit; // MOTU is register-based; no AV/C.
    if (route == route_) return;
    route_ = route;
    io_.UpdateRoute(route);
    if (notifications_ && notifications_->Valid()) notifications_->UpdateRoute(route);
    asyncAddressRegistered_.store(false, std::memory_order_release);
    opticalSnapshot_.store(0, std::memory_order_release);
    cachedSampleRateHz_.store(0, std::memory_order_release);
    preparedRateHz_.store(0, std::memory_order_release);
}

void MotuProtocol::ReadClockStatus(ClockStatusCallback callback) {
    (void)io_.ReadQuadBE(
        AddressOf(ClockRegister()),
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
            clock.sampleRateHz = (IsV1() ? DecodeRateV1(value, unitSwVersion_) : IsV3() ? DecodeRateV3(value) : DecodeRateV2(value)).value_or(0U);
            clock.source = IsV1() ? DecodeClockSourceV1(value, unitSwVersion_) : IsV3()
                ? DecodeClockSourceV3(value)
                : DecodeClockSourceV2(value);
            cachedSampleRateHz_.store(clock.sampleRateHz, std::memory_order_release);

            if (callback) {
                callback(kIOReturnSuccess, clock);
            }
        });
}

void MotuProtocol::SetSampleRate(uint32_t rateHz, CompletionCallback callback) {
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

        const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
        if (!model || !Encoding::Motu::SupportsRate(*model, rateHz) || !Encoding::Motu::FireWireOnly(unitSwVersion_)) {
            if (callback) callback(kIOReturnUnsupported);
            return;
        }
        if (IsV3()) { SetSampleRateV3(rateHz, clock, std::move(callback)); return; }
        const auto encoded = IsV1() ? EncodeRateV1(clock.raw, rateHz, unitSwVersion_) : EncodeRateV2(clock.raw, rateHz);
        if (!encoded.has_value()) {
            ASFW_LOG(Audio, "MotuProtocol: unsupported sample rate %u", rateHz);
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
            AddressOf(ClockRegister()),
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

void MotuProtocol::RebindNotifications(VoidCallback callback) {
    if (shuttingDown_.load(std::memory_order_acquire)) { if (callback) callback(kIOReturnAborted); return; }
    if (IsV3()) EnsureAsyncAddress(std::move(callback));
    else if (callback) callback(kIOReturnSuccess);
}

void MotuProtocol::EnsureAsyncAddress(CompletionCallback callback) {
    if (HasRegisteredAsyncAddress()) { if (callback) callback(kIOReturnSuccess); return; }
    const auto node = busInfo_.GetLocalNodeID();
    if (!notificationAddress_ || node == FW::kInvalidNodeId) {
        if (callback) callback(kIOReturnNotReady);
        return;
    }
    RegisterAsyncMessageAddress(static_cast<uint16_t>(0xffc0U | (node.value & 0x3fU)), notificationAddress_, std::move(callback));
}

void MotuProtocol::SetSampleRateV3(uint32_t rate, ClockStatus clock, CompletionCallback callback) {
    // Linux motu-protocol-v3.c:62-113: clear fetch, write clock, await
    // CLK_CHANGED for four seconds. ACK alone never establishes clock readiness.
    const uint32_t value = (clock.raw & ~0x0200ff00U) |
        (static_cast<uint32_t>(Encoding::Motu::RateToIndex(rate)) << 8);
    EnsureAsyncAddress([this, rate, clock, value, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) { if (callback) callback(status); return; }
        if (value == clock.raw) { if (callback) callback(kIOReturnSuccess); return; }
        if (!timerScheduler_) { if (callback) callback(kIOReturnNotReady); return; }
        auto wait = notifications_->Begin([this, rate, callback](IOReturn result) mutable {
            if (result != kIOReturnSuccess) { if (callback) callback(result); return; }
            ReadClockStatus([rate, callback = std::move(callback)](IOReturn result, ClockStatus observed) {
                if (callback) callback(result != kIOReturnSuccess ? result :
                    observed.sampleRateHz == rate ? kIOReturnSuccess : kIOReturnNotReady);
            });
        });
        if (!wait) { if (callback) callback(kIOReturnBusy); return; }
        (void)io_.WriteQuadBE(AddressOf(ClockRegister()), value,
            [this, wait](Async::AsyncStatus status) {
                const auto result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
                wait->Ack(result);
                if (result == kIOReturnSuccess && !wait->Done()) {
                    const std::weak_ptr<ClockChangeWait> weak = wait;
                    const auto token = timerScheduler_->ScheduleAfter(4'000'000'000ULL, [weak] {
                        if (const auto held = weak.lock()) held->Fail(kIOReturnTimeout);
                    });
                    if (token == Scheduling::kInvalidTimerToken) wait->Fail(kIOReturnNoResources);
                }
            });
    });
}

void MotuProtocol::WriteAsyncAddrPair(AsyncAddrValues values,
                                        bool registered,
                                        CompletionCallback callback) {
    (void)io_.WriteQuadBE(
        AddressOf(Reg::AsyncAddrHi),
        values.hi,
        [this, operationRoute = route_, values, registered, callback = std::move(callback)](
            Async::AsyncStatus hiStatus) mutable {
            const IOReturn hiResult = Protocols::Ports::MapAsyncStatusToIOReturn(hiStatus);
            if (shuttingDown_.load(std::memory_order_acquire)) { if (callback) callback(kIOReturnAborted); return; }
            if (operationRoute != route_) { if (callback) callback(kIOReturnOffline); return; }
            if (hiResult != kIOReturnSuccess) {
                if (callback) {
                    callback(hiResult);
                }
                return;
            }

            (void)io_.WriteQuadBE(
                AddressOf(Reg::AsyncAddrLo),
                values.lo,
                [this, operationRoute, registered, callback = std::move(callback)](
                    Async::AsyncStatus loStatus) mutable {
                    const IOReturn loResult = shuttingDown_.load(std::memory_order_acquire) ? kIOReturnAborted : operationRoute == route_ ?
                        Protocols::Ports::MapAsyncStatusToIOReturn(loStatus) : kIOReturnOffline;
                    if (loResult == kIOReturnSuccess) {
                        asyncAddressRegistered_.store(registered, std::memory_order_release);
                    }
                    if (callback) {
                        callback(loResult);
                    }
                });
        });
}

void MotuProtocol::RegisterAsyncMessageAddress(uint16_t hostNodeId,
                                                 uint64_t hostAddress,
                                                 CompletionCallback callback) {
    if (shuttingDown_.load(std::memory_order_acquire)) { if (callback) callback(kIOReturnAborted); return; }
    if (hostAddress < kAsyncMessageRegionStart || hostAddress > kAsyncMessageRegionEnd) {
        ASFW_LOG(Audio,
                 "MotuProtocol: async address 0x%012llx outside the device's accepted region",
                 hostAddress);
        if (callback) {
            callback(kIOReturnBadArgument);
        }
        return;
    }

    WriteAsyncAddrPair(EncodeAsyncAddr(hostNodeId, hostAddress), true, std::move(callback));
}

void MotuProtocol::ReleaseAsyncMessageAddress(CompletionCallback callback) {
    WriteAsyncAddrPair(AsyncAddrValues{.hi = 0U, .lo = 0U}, false, std::move(callback));
}

//==============================================================================
// Duplex bring-up
//==============================================================================

void MotuProtocol::ModifyRegister(Reg reg,
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

void MotuProtocol::EnsureRuntimeStreamGeometry(std::function<void(IOReturn)> callback) {
    opticalSnapshot_.store(0, std::memory_order_release);
    auto read = [this, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) { if (callback) callback(status); return; }
        ReadClockStatus([this, callback = std::move(callback)](IOReturn status, ClockStatus clock) mutable {
            const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
            if (status != kIOReturnSuccess || !model || !Encoding::Motu::SupportsRate(*model, clock.sampleRateHz)) {
                if (callback) callback(status != kIOReturnSuccess ? status : kIOReturnUnsupported);
                return;
            }
            ReadOpticalGeometry(std::move(callback));
        });
    };
    if (IsV3()) EnsureAsyncAddress(std::move(read));
    else read(kIOReturnSuccess);
}

void MotuProtocol::ReadOpticalGeometry(CompletionCallback callback) {
    // A stale answer must not outlive a failed read: until a read succeeds the caps
    // are unavailable.
    opticalSnapshot_.store(0U, std::memory_order_release);

    (void)io_.ReadQuadBE(
        AddressOf(OpticalRegister()),
        [this, callback = std::move(callback)](Async::AsyncStatus status,
                                               uint32_t optRaw) mutable {
            const IOReturn result = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (result != kIOReturnSuccess) {
                ASFW_LOG(Audio,
                         "[MotuGeometry] optical config read failed kr=0x%08x (%{public}s); "
                         "geometry not loaded; no rate formations available",
                         result, ASFW::Logging::IOReturnName(result));
                if (callback) {
                    callback(result);
                }
                return;
            }

            const auto index = Encoding::Motu::RateToIndex(CachedSampleRateHz());
            const auto mode = index >= 0 ? Encoding::Motu::IndexToMode(static_cast<uint32_t>(index)) : 0U;
            const V2PcmChunks chunks = ResolvePcmChunks(optRaw, mode, unitSwVersion_);
            if (!chunks.opticalDecoded || !chunks.tx || !chunks.rx) {
                if (callback) callback(kIOReturnUnsupported);
                return;
            }
            opticalSnapshot_.store((uint64_t{1} << 32) | optRaw, std::memory_order_release);
            // Optical modes: 0 off, 1 ADAT, 2 S/PDIF. Reserved encodings refuse publication.
            ASFW_LOG(Audio,
                     "[MotuGeometry] optical config opt=0x%08x in=%u out=%u decoded=%u -> "
                     "published txChunks=%u rxChunks=%u (rate mode %u)",
                     optRaw, static_cast<unsigned>(chunks.inputMode),
                     static_cast<unsigned>(chunks.outputMode), chunks.opticalDecoded ? 1U : 0U,
                     chunks.tx, chunks.rx, mode);
            if (callback) {
                callback(kIOReturnSuccess);
            }
        });
}

bool MotuProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = {};
    const auto snapshot = opticalSnapshot_.load(std::memory_order_acquire);
    if (!(snapshot >> 32)) return false;
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    if (!model) return false;
    outCaps.sampleRateHz = cachedSampleRateHz_.load(std::memory_order_acquire);
    if (!Encoding::Motu::SupportsRate(*model, outCaps.sampleRateHz)) return false;
    const auto mode = Encoding::Motu::IndexToMode(static_cast<uint32_t>(Encoding::Motu::RateToIndex(outCaps.sampleRateHz)));
    const auto chunks = ResolvePcmChunks(static_cast<uint32_t>(snapshot), mode, unitSwVersion_);
    if (!chunks.opticalDecoded || !chunks.tx || !chunks.rx) return false;
    outCaps.hostInputPcmChannels = outCaps.deviceToHostPcmChunks = chunks.tx;
    outCaps.hostOutputPcmChannels = outCaps.hostToDevicePcmChunks = chunks.rx;
    return true;
}

std::shared_ptr<const std::vector<Runtime::RateFormation>> MotuProtocol::RateFormations() const {
    const auto snapshot = opticalSnapshot_.load(std::memory_order_acquire);
    if (!(snapshot >> 32)) return {};
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    if (!model) return {};
    auto formations = std::make_shared<std::vector<Runtime::RateFormation>>();
    const auto optical = static_cast<uint32_t>(snapshot);
    for (const auto rate : Encoding::Motu::kClockRates) {
        if (!Encoding::Motu::SupportsRate(*model, rate)) continue;
        const auto chunks = ResolvePcmChunks(optical,
            Encoding::Motu::IndexToMode(static_cast<uint32_t>(Encoding::Motu::RateToIndex(rate))), unitSwVersion_);
        // A reserved optical code cannot establish any usable formation.
        if (!chunks.opticalDecoded) continue;
        Runtime::RateFormation formation{};
        formation.sampleRateHz = rate;
        formation.protocolSupported = true;
        formation.packedPcm = true;
        formation.packedCaptureMessageChunks = Encoding::Motu::MessageChunks(unitSwVersion_, true);
        formation.packedPlaybackMessageChunks = Encoding::Motu::MessageChunks(unitSwVersion_, false);
        formation.capture.push_back({chunks.tx, Encoding::Motu::DataBlockQuadlets(chunks.tx, formation.packedCaptureMessageChunks)});
        formation.playback.push_back({chunks.rx, Encoding::Motu::DataBlockQuadlets(chunks.rx, formation.packedPlaybackMessageChunks)});
        formations->push_back(std::move(formation));
    }
    return formations;
}

void MotuProtocol::ReadRateObservation(std::function<void(IOReturn, RateHardwareObservation)> callback) {
    ReadClockStatus([this, callback = std::move(callback)](IOReturn status, ClockStatus clock) mutable {
        RateHardwareObservation observation{};
        const auto index = Encoding::Motu::RateToIndex(clock.sampleRateHz);
        if (index >= 0) {
            const auto chunks = ResolvePcmChunks(static_cast<uint32_t>(opticalSnapshot_.load(std::memory_order_acquire)),
                Encoding::Motu::IndexToMode(static_cast<uint32_t>(index)), unitSwVersion_);
            observation.caps.hostInputPcmChannels = observation.caps.deviceToHostPcmChunks = chunks.tx;
            observation.caps.hostOutputPcmChannels = observation.caps.hostToDevicePcmChunks = chunks.rx;
        }
        observation.caps.sampleRateHz = clock.sampleRateHz;
        observation.clockConfirmed = status == kIOReturnSuccess && clock.sampleRateHz != 0;
        if (callback) callback(status, observation);
    });
}

bool MotuProtocol::GetChannelLabels(std::vector<std::string>& inNames,
                                      std::vector<std::string>& outNames) const {
    const Encoding::Motu::MotuPortMap capture =
        Isoch::Audio::MOTU::Profiles::CapturePortsForSwVersion(unitSwVersion_);
    const Encoding::Motu::MotuPortMap playback =
        Isoch::Audio::MOTU::Profiles::PlaybackPortsForSwVersion(unitSwVersion_);
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

AudioStreamRuntimeCaps MotuProtocol::MakeRuntimeCaps() const noexcept {
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

void MotuProtocol::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    // IRM allocation can replace the provisional numbers after PrepareDuplex, and the
    // device must be told the committed values before ProgramTxAndEnableDuplex writes
    // them into the iso-comm register.
    deviceRxChannel_.store(channels.PlaybackChannel(0), std::memory_order_release);
    deviceTxChannel_.store(channels.CaptureChannel(0), std::memory_order_release);
}

void MotuProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                   const AudioClockConfig& desiredClock,
                                   PrepareCallback callback) {
    SetAssignedChannels(channels);

    const uint32_t rateHz = desiredClock.sampleRateHz != 0U ? desiredClock.sampleRateHz : 48000U;
    const int32_t rateIndex = Encoding::Motu::RateToIndex(rateHz);
    if (rateIndex < 0) {
        ASFW_LOG(Audio, "MotuProtocol: unsupported duplex rate %u", rateHz);
        if (callback) {
            callback(kIOReturnUnsupported, DuplexPrepareResult{});
        }
        return;
    }
    const uint32_t mode = Encoding::Motu::IndexToMode(static_cast<uint32_t>(rateIndex));
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    const uint32_t fixedChunks = model && Encoding::Motu::SupportsRate(*model, rateHz) ? model->captureChunks[mode] : 0U;
    if (fixedChunks == 0U) {
        // Mode 2 (176.4/192k) is not implemented by the v2 fixed-chunk models.
        ASFW_LOG(Audio, "MotuProtocol: rate %u has no chunk layout for this model", rateHz);
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
    // from the idle clock apply, which the start path does not call. The
    // UltraLite therefore stayed on whatever rate it powered up with (44.1 kHz, clock
    // status reading 0x00000000) while the host had negotiated 48 kHz. WaitForStableGlobalClock
    // requires nominalRateHz == desiredClock.sampleRateHz, so it could never succeed: it
    // spun for its full 1000 ms budget on every attempt. That wait runs inside a
    // DispatchSync chain rooted in coreaudiod's StartDevice external method, so overrunning
    // it makes the HAL abandon the call with kIOReturnTimeout -- surfacing to CoreAudio as
    // _TellHardwareToStart returning 0x77686174 ('what') with no failure logged by us.
    SetSampleRate(
        rateHz,
        [this, speedCode, rateHz, mode, callback = std::move(callback)](
            IOReturn rateStatus) mutable {
            if (rateStatus != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuProtocol: set clock rate %u failed: 0x%x", rateHz,
                         rateStatus);
                if (callback) {
                    callback(rateStatus, DuplexPrepareResult{});
                }
                return;
            }
            ASFW_LOG(Audio, "MotuProtocol: clock rate set to %u before prepare", rateHz);

    (void)io_.ReadQuadBE(
        AddressOf(OpticalRegister()),
        [this, speedCode, rateHz, mode, callback = std::move(callback)](
            Async::AsyncStatus status, uint32_t optRaw) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuProtocol: optical config read failed: 0x%x", readResult);
                if (callback) {
                    callback(readResult, DuplexPrepareResult{});
                }
                return;
            }

            // The same arithmetic the published description uses. A reserved encoding
            // means we cannot prove the chunk counts; ResolveV2PcmChunks then clears both
            // exclude bits ("differed chunks may be present") rather than guessing the
            // device is in the fixed layout.
            const V2PcmChunks chunks = ResolvePcmChunks(optRaw, mode, unitSwVersion_);
            if (!chunks.opticalDecoded) {
                if (callback) callback(kIOReturnUnsupported, {});
                return;
            }
            // Packet format flags compare the 1x geometry, independently of the
            // requested mode (Linux motu-stream.c:201-225).
            const auto baseline = ResolvePcmChunks(optRaw, 0, unitSwVersion_);
            const bool txOnlyFixedChunks = baseline.txOnlyFixedChunks;
            const bool rxOnlyFixedChunks = baseline.rxOnlyFixedChunks;
            opticalSnapshot_.store((uint64_t{1} << 32) | optRaw, std::memory_order_release);
            txPcmChunks_.store(chunks.tx, std::memory_order_release);
            rxPcmChunks_.store(chunks.rx, std::memory_order_release);
            preparedRateHz_.store(rateHz, std::memory_order_release);

            ASFW_LOG(Audio,
                     "MotuProtocol: prepare duplex rate=%u opt=0x%08x txChunks=%u rxChunks=%u "
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

void MotuProtocol::ApplyFetchingModeIfNeeded(bool enable,
                                               CompletionCallback callback) {
    // 828mk2 and 896HD need no fetching-mode write; the UltraLite and 8pre implement a
    // Xilinx Spartan XC3S200 and do (motu-protocol-v2.c:190-225).
    // Completion is awaited by the session; failed writes abort bring-up.
    if (IsV1()) {
        ModifyRegister(ClockRegister(), [enable, version = unitSwVersion_](uint32_t current) {
            return EncodeFetchingV1(current, enable, version);
        }, std::move(callback));
        return;
    }
    if (!IsV3() && !NeedsFetchingModeWrite(unitSwVersion_)) {
        if (callback) {
            callback(kIOReturnSuccess);
        }
        return;
    }
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    const bool spartan = model && model->fetch == Encoding::Motu::FetchRule::Spartan;
    const bool traveler = model && model->fetch == Encoding::Motu::FetchRule::Traveler;
    ModifyRegister(
        Reg::ClockStatusV2,
        [enable, spartan, traveler, v3 = IsV3()](uint32_t current) {
            return v3 ? ((current & ~kClockFetchEnable) | (enable ? kClockFetchEnable : 0U)) :
                EncodeFetchingMode(current, enable, spartan, traveler);
        },
        [callback = std::move(callback)](IOReturn status) mutable {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuProtocol: fetching-mode write failed: 0x%x", status);
            }
            if (callback) {
                callback(status);
            }
        });
}

void MotuProtocol::ProgramRx(StageCallback callback) {
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

void MotuProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!duplexPrepared_.load(std::memory_order_acquire)) {
        // Without PrepareDuplex the iso channels are unknown, and writing channel 0/0
        // would point the device at whatever is on those channels.
        ASFW_LOG(Audio, "MotuProtocol: enable requested before prepare");
        if (callback) {
            callback(kIOReturnNotReady, DuplexStageResult{});
        }
        return;
    }

    const uint32_t rxChannel = deviceRxChannel_.load(std::memory_order_acquire);
    const uint32_t txChannel = deviceTxChannel_.load(std::memory_order_acquire);

    ModifyRegister(
        Reg::IsocCommControl,
        [rxChannel, txChannel, clearLow = Encoding::Motu::FindModel(unitSwVersion_)->runtime.clearIsoCommLowBits](uint32_t current) {
            // Zeroing is observed on the 828mk3 in PR #172. Other models
            // preserve unrelated low bits like Linux motu-stream.c:62-84.
            return EncodeIsoCommStart(clearLow ? 0U : current, rxChannel, txChannel);
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
                if (Encoding::Motu::FindModel(unitSwVersion_)->runtime.write48kStreamConfig && preparedRateHz_.load(std::memory_order_acquire) == 48000) {
                    (void)io_.WriteQuadBE(AddressOf(Reg::StreamConfigV3), 0x00120000,
                        [result, callback = std::move(callback)](Async::AsyncStatus status) mutable {
                            callback(Protocols::Ports::MapAsyncStatusToIOReturn(status), result);
                        });
                } else callback(kIOReturnSuccess, result);
            }
        });
}

void MotuProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    // Session has started both contexts and passed the model readiness gate.
    // Linux motu-stream.c:289-307 waits for the domain before enabling fetch.
    ApplyFetchingModeIfNeeded(true, [this, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            if (callback) callback(status, {});
            return;
        }
        ReadDuplexConfirmation(std::move(callback));
    });
}

void MotuProtocol::ReadDuplexConfirmation(ConfirmCallback callback) {
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
                         "MotuProtocol: duplex not confirmed raw=0x%08x rx=%u/%u tx=%u/%u",
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

void MotuProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                      ClockApplyCallback callback) {
    const uint32_t rateHz = desiredClock.sampleRateHz;
    SetSampleRate(rateHz, [this, rateHz, callback = std::move(callback)](IOReturn status) mutable {
        if (status == kIOReturnSuccess) {
            preparedRateHz_.store(rateHz, std::memory_order_release);
        }
        DuplexClockApplyResult result{};
        result.generation = busInfo_.GetGeneration();
        result.appliedClock = AudioClockConfig{.sampleRateHz = CachedSampleRateHz()};
        result.runtimeCaps = MakeRuntimeCaps();
        if (callback) {
            callback(status, result);
        }
    });
}

void MotuProtocol::ReadDuplexHealth(HealthCallback callback) {
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

IOReturn MotuProtocol::StopDuplex() {
    if (!duplexActive_.load(std::memory_order_acquire) &&
        !duplexPrepared_.load(std::memory_order_acquire)) return kIOReturnSuccess;
    // Never report a successful stop while device writes are still in flight.
    // Mute fetching before deactivation; both completions belong to this stage.
    const IOReturn result = AwaitStageStatus([this](auto callback) {
        ApplyFetchingModeIfNeeded(false, [this, callback = std::move(callback)](IOReturn fetchStatus) mutable {
            ModifyRegister(Reg::IsocCommControl,
                [](uint32_t value) { return EncodeIsoCommStop(value); },
                [fetchStatus, callback = std::move(callback)](IOReturn stopStatus) mutable {
                    callback(fetchStatus != kIOReturnSuccess ? fetchStatus : stopStatus);
                });
        });
    }, teardownCancel_);
    if (result == kIOReturnSuccess) {
        duplexActive_.store(false, std::memory_order_release);
        duplexPrepared_.store(false, std::memory_order_release);
        preparedRateHz_.store(0, std::memory_order_release);
    }
    return result;
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

void MotuProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
}

IOReturn MotuProtocol::LoadGeometry() {
    return AwaitStageStatus([this](auto callback) {
        ReadClockStatus([this, callback = std::move(callback)](IOReturn status, ClockStatus) mutable {
            if (status != kIOReturnSuccess) { callback(status); return; }
            EnsureRuntimeStreamGeometry(std::move(callback));
        });
    }, teardownCancel_);
}

std::optional<AudioStreamRuntimeCaps> MotuProtocol::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> MotuProtocol::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    return AwaitStage<DuplexPrepareResult>(
        [&](auto callback) { PrepareDuplex(channels, clock, std::move(callback)); },
        teardownCancel_);
}

std::expected<AudioDuplexChannels, IOReturn> MotuProtocol::AssignChannels(const AudioDuplexChannels& channels) {
    SetAssignedChannels(channels);
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> MotuProtocol::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_, timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> MotuProtocol::ArmDeviceRx() {
    if (const auto* model = Encoding::Motu::FindModel(unitSwVersion_); model && model->runtime.deactivateBeforeStart) {
        const auto status = AwaitStageStatus([this](auto callback) {
            ModifyRegister(Reg::IsocCommControl, [](uint32_t) { return kChangeRxState | kChangeTxState; }, std::move(callback));
        }, teardownCancel_);
        if (status != kIOReturnSuccess) return std::unexpected(status);
        IOSleep(20); // PR #172 MOTU828Mk3Protocol.cpp:270-299, off Default.
        if (teardownCancel_ && teardownCancel_->load(std::memory_order_acquire)) return std::unexpected(kIOReturnAborted);
    }
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramRx(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexStageResult, IOReturn> MotuProtocol::ArmDeviceTxAndEnable() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramTxAndEnableDuplex(std::move(callback)); }, teardownCancel_);
}

FamilyDriver::StartReadinessPolicy MotuProtocol::GetStartReadinessPolicy() const noexcept {
    const auto* model = Encoding::Motu::FindModel(unitSwVersion_);
    return model ? StartReadinessPolicy{.requireTransmitTiming = model->runtime.requireTimingBeforeFetch,
        .minimumHostRunMs = model->runtime.minimumHostRunMs} : StartReadinessPolicy{};
}

std::expected<DuplexConfirmResult, IOReturn> MotuProtocol::Confirm() {
    return AwaitStage<DuplexConfirmResult>(
        [&](auto callback) { ConfirmDuplexStart(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexClockApplyResult, IOReturn> MotuProtocol::ApplyClockIdle(
    const AudioClockConfig& clock) {
    return AwaitStage<DuplexClockApplyResult>(
        [&](auto callback) { ApplyClockConfig(clock, std::move(callback)); }, teardownCancel_);
}

IOReturn MotuProtocol::DisconnectPlayback() {
    // MOTU has no per-direction connection: no CMP plug, and the streams are
    // switched together through the IsocCommControl register. The staged-stop
    // recipe that calls this is not MOTU's.
    return kIOReturnUnsupported;
}

IOReturn MotuProtocol::DisconnectCapture() {
    // As DisconnectPlayback.
    return kIOReturnUnsupported;
}

IOReturn MotuProtocol::BreakConnections() {
    // No CMP connections to break; StopDuplex switches both streams off.
    return kIOReturnUnsupported;
}

IOReturn MotuProtocol::Stop() {
    return StopDuplex();
}

} // namespace ASFW::Audio::Motu
