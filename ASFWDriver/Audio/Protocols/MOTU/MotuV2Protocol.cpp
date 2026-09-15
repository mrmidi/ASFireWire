// SPDX-License-Identifier: Apache-2.0
// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
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
                               ::ASFW::IRM::IRMClient* irmClient,
                               uint32_t unitSwVersion)
    : io_(busOps, busInfo, routeRegistry, route)
    , irmClient_(irmClient)
    , route_(route)
    , unitSwVersion_(unitSwVersion) {}

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

    return kIOReturnSuccess;
}

IOReturn MotuV2Protocol::Shutdown() {
    (void)StopDuplex();
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
    route_ = route;
}

bool MotuV2Protocol::GetRuntimeAudioStreamCaps(
    AudioStreamRuntimeCaps& outCaps) const {
    outCaps = {};
    outCaps.hostInputPcmChannels = 14;
    outCaps.hostOutputPcmChannels = 14;
    // This field is the generic wire data-block width. MOTU carries 13
    // quadlets per block while exposing 14 packed PCM channels.
    outCaps.deviceToHostAm824Slots = Encoding::Motu::DataBlockQuadlets(14);
    outCaps.hostToDeviceAm824Slots = Encoding::Motu::DataBlockQuadlets(14);
    outCaps.sampleRateHz =
        cachedSampleRateHz_.load(std::memory_order_acquire);
    if (outCaps.sampleRateHz == 0) {
        outCaps.sampleRateHz = 48000;
    }
    outCaps.deviceToHostStreamCount = 1;
    outCaps.hostToDeviceStreamCount = 1;
    outCaps.deviceToHostStreams[0] = {
        .isoChannel = AudioStreamWireInfo::kInvalidIsoChannel,
        .pcmChannels = 14,
        .am824Slots =
            static_cast<uint16_t>(Encoding::Motu::DataBlockQuadlets(14)),
        .midiPorts = 1,
    };
    outCaps.hostToDeviceStreams[0] =
        outCaps.deviceToHostStreams[0];
    return true;
}

void MotuV2Protocol::SetAssignedChannels(
    const AudioDuplexChannels& channels) noexcept {
    assignedChannels_ = channels;
}

void MotuV2Protocol::PrepareDuplex(
    const AudioDuplexChannels& channels,
    const AudioClockConfig& desiredClock,
    PrepareCallback callback) {
    if (!initialized_ || irmClient_ == nullptr) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (desiredClock.sampleRateHz != 0 &&
        desiredClock.sampleRateHz != 48000) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    assignedChannels_ = channels;
    SetSampleRate(48000,
        [this, callback = std::move(callback)](IOReturn rateStatus) mutable {
            if (rateStatus != kIOReturnSuccess) {
                callback(rateStatus, {});
                return;
            }
            (void)io_.ReadQuadBE(
                AddressOf(Reg::InOutConfV2),
                [this, callback = std::move(callback)](
                    Async::AsyncStatus ioStatus, uint32_t value) mutable {
                    const IOReturn status =
                        Protocols::Ports::MapAsyncStatusToIOReturn(ioStatus);
                    if (status != kIOReturnSuccess) {
                        callback(status, {});
                        return;
                    }
                    if (const auto optical =
                            DecodeOptIfaceConfig(value);
                        optical.has_value()) {
                        txOnlyFixedChunks_ =
                            optical->input != OptIfaceMode::Adat;
                        rxOnlyFixedChunks_ =
                            optical->output != OptIfaceMode::Adat;
                    }
                    AudioStreamRuntimeCaps caps{};
                    (void)GetRuntimeAudioStreamCaps(caps);
                    callback(kIOReturnSuccess,
                             DuplexPrepareResult{
                                 .generation = route_.generation,
                                 .channels = assignedChannels_,
                                 .appliedClock =
                                     AudioClockConfig{.sampleRateHz = 48000},
                                 .runtimeCaps = caps,
                             });
                });
        });
}

void MotuV2Protocol::ProgramRx(StageCallback callback) {
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback(kIOReturnSuccess,
             DuplexStageResult{
                 .generation = route_.generation,
                 .channels = assignedChannels_,
                 .runtimeCaps = caps,
             });
}

void MotuV2Protocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    (void)io_.ReadQuadBE(
        AddressOf(Reg::PacketFormat),
        [this, callback = std::move(callback)](
            Async::AsyncStatus readStatus, uint32_t packetFormat) mutable {
            IOReturn status =
                Protocols::Ports::MapAsyncStatusToIOReturn(readStatus);
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            const uint32_t encoded = EncodePacketFormat(
                packetFormat, txOnlyFixedChunks_, rxOnlyFixedChunks_,
                /*S400=*/2);
            (void)io_.WriteQuadBE(
                AddressOf(Reg::PacketFormat), encoded,
                [this, callback = std::move(callback)](
                    Async::AsyncStatus writeStatus) mutable {
                    const IOReturn status =
                        Protocols::Ports::MapAsyncStatusToIOReturn(writeStatus);
                    if (status != kIOReturnSuccess) {
                        callback(status, {});
                        return;
                    }
                    (void)io_.ReadQuadBE(
                        AddressOf(Reg::IsocCommControl),
                        [this, callback = std::move(callback)](
                            Async::AsyncStatus readIsoStatus,
                            uint32_t isoControl) mutable {
                            const IOReturn readResult =
                                Protocols::Ports::MapAsyncStatusToIOReturn(
                                    readIsoStatus);
                            if (readResult != kIOReturnSuccess) {
                                callback(readResult, {});
                                return;
                            }
                            const uint32_t start = EncodeIsoCommStart(
                                isoControl,
                                assignedChannels_.hostToDeviceIsoChannel,
                                assignedChannels_.deviceToHostIsoChannel);
                            (void)io_.WriteQuadBE(
                                AddressOf(Reg::IsocCommControl), start,
                                [this, callback = std::move(callback)](
                                    Async::AsyncStatus startStatus) mutable {
                                    const IOReturn result =
                                        Protocols::Ports::
                                            MapAsyncStatusToIOReturn(
                                                startStatus);
                                    AudioStreamRuntimeCaps caps{};
                                    (void)GetRuntimeAudioStreamCaps(caps);
                                    callback(
                                        result,
                                        DuplexStageResult{
                                            .generation = route_.generation,
                                            .channels = assignedChannels_,
                                            .runtimeCaps = caps,
                                        });
                                });
                        });
                });
        });
}

void MotuV2Protocol::ConfirmDuplexStart(ConfirmCallback callback) {
    (void)io_.ReadQuadBE(
        AddressOf(Reg::ClockStatusV2),
        [this, callback = std::move(callback)](
            Async::AsyncStatus readStatus, uint32_t clock) mutable {
            const IOReturn status =
                Protocols::Ports::MapAsyncStatusToIOReturn(readStatus);
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            (void)io_.WriteQuadBE(
                AddressOf(Reg::ClockStatusV2),
                EncodeFetchingModeV2(clock, true),
                [this, callback = std::move(callback)](
                    Async::AsyncStatus writeStatus) mutable {
                    const IOReturn result =
                        Protocols::Ports::MapAsyncStatusToIOReturn(
                            writeStatus);
                    AudioStreamRuntimeCaps caps{};
                    (void)GetRuntimeAudioStreamCaps(caps);
                    callback(
                        result,
                        DuplexConfirmResult{
                            .generation = route_.generation,
                            .channels = assignedChannels_,
                            .appliedClock =
                                AudioClockConfig{.sampleRateHz = 48000},
                            .runtimeCaps = caps,
                        });
                });
        });
}

void MotuV2Protocol::ApplyClockConfig(
    const AudioClockConfig& desiredClock,
    ClockApplyCallback callback) {
    if (desiredClock.sampleRateHz != 48000) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    SetSampleRate(48000,
        [this, callback = std::move(callback)](IOReturn status) mutable {
            AudioStreamRuntimeCaps caps{};
            (void)GetRuntimeAudioStreamCaps(caps);
            callback(status,
                     ClockApplyResult{
                         .generation = route_.generation,
                         .appliedClock =
                             AudioClockConfig{.sampleRateHz = 48000},
                         .runtimeCaps = caps,
                     });
        });
}

void MotuV2Protocol::ReadDuplexHealth(HealthCallback callback) {
    ReadClockStatus(
        [this, callback = std::move(callback)](
            IOReturn status, ClockStatus clock) mutable {
            AudioStreamRuntimeCaps caps{};
            (void)GetRuntimeAudioStreamCaps(caps);
            callback(status,
                     DuplexHealthResult{
                         .generation = route_.generation,
                         .appliedClock =
                             AudioClockConfig{
                                 .sampleRateHz = clock.sampleRateHz},
                         .runtimeCaps = caps,
                         .sourceLocked = status == kIOReturnSuccess,
                         .clockReferenceHealthy =
                             status == kIOReturnSuccess,
                         .nominalRateHz = clock.sampleRateHz,
                     });
        });
}

IOReturn MotuV2Protocol::StopDuplex() {
    if (!initialized_) {
        return kIOReturnSuccess;
    }
    (void)io_.ReadQuadBE(
        AddressOf(Reg::ClockStatusV2),
        [this](Async::AsyncStatus status, uint32_t clock) {
            if (status == Async::AsyncStatus::kSuccess) {
                (void)io_.WriteQuadBE(
                    AddressOf(Reg::ClockStatusV2),
                    EncodeFetchingModeV2(clock, false),
                    [](Async::AsyncStatus) {});
            }
        });
    (void)io_.ReadQuadBE(
        AddressOf(Reg::IsocCommControl),
        [this](Async::AsyncStatus status, uint32_t control) {
            if (status == Async::AsyncStatus::kSuccess) {
                (void)io_.WriteQuadBE(
                    AddressOf(Reg::IsocCommControl),
                    EncodeIsoCommStop(control),
                    [](Async::AsyncStatus) {});
            }
        });
    return kIOReturnSuccess;
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

} // namespace ASFW::Audio::Motu
