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

} // namespace ASFW::Audio::Motu
