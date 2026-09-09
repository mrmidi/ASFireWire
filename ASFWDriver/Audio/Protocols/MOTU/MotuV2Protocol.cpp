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
                               uint32_t unitSwVersion)
    : io_(busOps, busInfo, routeRegistry, route)
    , busInfo_(busInfo)
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

void MotuV2Protocol::PrepareDuplex48k(const AudioDuplexChannels& channels,
                                      VoidCallback callback) {
    deviceRxChannel_.store(channels.PlaybackChannel(0), std::memory_order_release);
    deviceTxChannel_.store(channels.CaptureChannel(0), std::memory_order_release);
    duplexPrepared_.store(true, std::memory_order_release);

    // The exclude-differed-chunks bits say "this direction carries only its fixed chunk
    // count". That is true exactly when the optical interface is not in ADAT mode: ADAT
    // adds chunks on top of the fixed baseline (motu-protocol-v2.c:253-269), so the
    // optical config has to be read before the packet format can be computed
    // (motu-stream.c:201-225). Capture (TX, device->host) follows the optical *input*;
    // playback (RX) follows the optical *output*.
    // FW::Speed's enumerators are the IEEE 1394-1995 wire codes (S100=0 .. S800=3), which
    // is exactly what this register field expects -- the same value Linux takes from
    // fw_parent_device(unit)->max_speed (motu-stream.c:218).
    const uint32_t speedCode = static_cast<uint32_t>(busInfo_.GetSpeed(io_.NodeId()));

    (void)io_.ReadQuadBE(
        AddressOf(Reg::InOutConfV2),
        [this, speedCode, callback = std::move(callback)](Async::AsyncStatus status,
                                                          uint32_t optRaw) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                ASFW_LOG(Audio, "MotuV2Protocol: optical config read failed: 0x%x", readResult);
                if (callback) {
                    callback(readResult);
                }
                return;
            }

            // A reserved encoding means we cannot prove the chunk counts. Treat that as
            // "differed chunks may be present" (clear both bits) rather than guessing the
            // device is in the fixed layout: claiming fixed when it is not truncates the
            // stream, while the conservative direction only costs the optimisation.
            const auto optical = DecodeOptIfaceConfig(optRaw);
            const bool txOnlyFixedChunks =
                optical.has_value() && optical->input != OptIfaceMode::Adat;
            const bool rxOnlyFixedChunks =
                optical.has_value() && optical->output != OptIfaceMode::Adat;

            ASFW_LOG(Audio,
                     "MotuV2Protocol: prepare duplex opt=0x%08x txFixed=%d rxFixed=%d speed=%u "
                     "rxCh=%u txCh=%u",
                     optRaw,
                     txOnlyFixedChunks ? 1 : 0,
                     rxOnlyFixedChunks ? 1 : 0,
                     speedCode,
                     deviceRxChannel_.load(std::memory_order_acquire),
                     deviceTxChannel_.load(std::memory_order_acquire));

            ModifyRegister(
                Reg::PacketFormat,
                [speedCode, txOnlyFixedChunks, rxOnlyFixedChunks](uint32_t current) {
                    return EncodePacketFormat(current, txOnlyFixedChunks, rxOnlyFixedChunks,
                                              speedCode);
                },
                std::move(callback));
        });
}

void MotuV2Protocol::ProgramRxForDuplex48k(VoidCallback callback) {
    // No device-side RX-only step exists on v2: both directions are activated together in
    // the single iso-comm write issued by ProgramTxAndEnableDuplex48k
    // (motu-stream.c:62-83, begin_session). The hook stays a success so the coordinator's
    // ordering (RX leg, then TX leg + enable) is preserved for protocols that need it.
    if (callback) {
        callback(kIOReturnSuccess);
    }
}

void MotuV2Protocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    if (!duplexPrepared_.load(std::memory_order_acquire)) {
        // Without PrepareDuplex48k the iso channels are unknown, and writing channel 0/0
        // would point the device at whatever is on those channels.
        ASFW_LOG(Audio, "MotuV2Protocol: enable requested before prepare");
        if (callback) {
            callback(kIOReturnNotReady);
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
        [this, callback = std::move(callback)](IOReturn status) mutable {
            if (status == kIOReturnSuccess) {
                duplexActive_.store(true, std::memory_order_release);
            }
            if (callback) {
                callback(status);
            }
        });
}

void MotuV2Protocol::ConfirmDuplex48kStart(VoidCallback callback) {
    // Read the iso-comm register back and require the device to report both directions
    // activated on the channels we asked for. The write completing only means the
    // transaction was accepted.
    const uint32_t expectedRx = deviceRxChannel_.load(std::memory_order_acquire);
    const uint32_t expectedTx = deviceTxChannel_.load(std::memory_order_acquire);

    (void)io_.ReadQuadBE(
        AddressOf(Reg::IsocCommControl),
        [expectedRx, expectedTx, callback = std::move(callback)](Async::AsyncStatus status,
                                                                 uint32_t value) mutable {
            const IOReturn readResult = Protocols::Ports::MapAsyncStatusToIOReturn(status);
            if (readResult != kIOReturnSuccess) {
                if (callback) {
                    callback(readResult);
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
            if (callback) {
                callback(ok ? kIOReturnSuccess : kIOReturnNotReady);
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
