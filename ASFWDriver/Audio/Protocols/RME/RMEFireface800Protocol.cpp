// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RMEFireface800Protocol.cpp - Stage 5H bounded silent playback integration.
//
// This stage preserves the verified device start/stop handshake and the Stage
// 5G D-D-D-S packet shape, then runs the immutable 48-cycle silence program as
// a circular ring for exactly 100 ms. OHCI is stopped before the device and IRM
// cleanup. Core Audio StartIO remains rejected; refill and interrupts stay off.

#include "RMEFireface800Protocol.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Bus/IRM/IRMClient.hpp"
#include "../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "../../../Shared/Isoch/IsochAudioTransport.hpp"
#include "../../../Hardware/OHCIDescriptors.hpp"
#include "../Backends/SyncAsyncBridge.hpp"

#include <span>
#include <array>
#include <memory>
#include <new>
#include <DriverKit/IOLib.h>

namespace ASFW::Audio::RME {
namespace {

constexpr uint64_t kStfAddress = 0x0000fc88f000ULL;
constexpr uint64_t kSyncStatusAddress = 0x0000801c0000ULL;
constexpr uint64_t kClockConfigAddress = 0x0000801c0004ULL;
constexpr uint64_t kTxIsochChannelAddress = 0x0000801c0008ULL;
constexpr uint64_t kRxPacketFormatAddress = 0x0000fc88f004ULL;
constexpr uint64_t kAllocTxStreamAddress = 0x0000fc88f008ULL;
constexpr uint64_t kIsochCommStartAddress = 0x0000fc88f00cULL;
constexpr uint64_t kIsochCommStopAddress = 0x0000fc88f010ULL;

Async::FWAddress MakeAddress(uint64_t value) noexcept {
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((value >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(value & 0xFFFFFFFFU),
    }};
}

} // namespace

RMEFireface800Protocol::RMEFireface800Protocol(
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    uint16_t nodeId,
    uint64_t deviceGuid,
    ::ASFW::IRM::IRMClient* irmClient) noexcept
    : busOps_(busOps),
      busInfo_(busInfo),
      nodeId_(nodeId),
      deviceGuid_(deviceGuid),
      irmClient_(irmClient) {}

RMEFireface800Protocol::~RMEFireface800Protocol() {
    BestEffortStopDeviceCommunication();
    active_.store(false, std::memory_order_release);
    ReleaseReservedResources();
    if (probeHandle_.IsValid()) {
        (void)busOps_.Cancel(probeHandle_);
    }
}

IOReturn RMEFireface800Protocol::Initialize() {
    playbackPreflightRoute_.store(0U, std::memory_order_release);
    stage5hInFlight_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    ASFW_LOG(Audio,
             "[RME] Stage 5F no-CIP wire-format preflight starting GUID=0x%016llx node=%u gen=%u",
             deviceGuid_,
             static_cast<unsigned>(nodeId_ & 0x3FU),
             busInfo_.GetGeneration().value);
    ProbeClockConfig();
    return kIOReturnSuccess;
}

IOReturn RMEFireface800Protocol::Shutdown() {
    BestEffortStopDeviceCommunication();
    active_.store(false, std::memory_order_release);
    stage5hInFlight_.store(false, std::memory_order_release);
    ReleaseReservedResources();
    if (probeHandle_.IsValid()) {
        (void)busOps_.Cancel(probeHandle_);
    }
    ASFW_LOG(Audio, "[RME] Stage 5F preflight stopped GUID=0x%016llx", deviceGuid_);
    return kIOReturnSuccess;
}

void RMEFireface800Protocol::UpdateRuntimeContext(
    uint16_t nodeId,
    Protocols::AVC::FCPTransport* transport) {
    (void)transport;
    nodeId_ = nodeId;
}

bool RMEFireface800Protocol::GetPlaybackPreflightRoute(
    PlaybackPreflightRoute& outRoute) const {
    if (!active_.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t snapshot =
        playbackPreflightRoute_.load(std::memory_order_acquire);
    if (snapshot == 0U) {
        return false;
    }

    outRoute.channel = static_cast<uint8_t>(snapshot & 0xFFU);
    outRoute.bandwidthUnits =
        static_cast<uint32_t>((snapshot >> 8U) & 0x00FFFFFFU);
    outRoute.sampleRateHz = static_cast<uint32_t>(snapshot >> 32U);
    outRoute.deviceCommunicationStopped = true;
    return outRoute.channel <= 63U && outRoute.bandwidthUnits != 0U &&
           outRoute.sampleRateHz != 0U;
}

IOReturn RMEFireface800Protocol::RunBoundedPlaybackIntegrationPreflight(
    const PlaybackPreflightRoute& route,
    BoundedPlaybackStep prepareHost,
    BoundedPlaybackStep runHostBurst,
    BoundedPlaybackStep cleanupHost) {
    if (!active_.load(std::memory_order_acquire) || !prepareHost ||
        !runHostBurst || !cleanupHost ||
        route.channel > 63U || route.bandwidthUnits != 1286U ||
        route.sampleRateHz != 192000U ||
        !route.deviceCommunicationStopped) {
        return kIOReturnBadArgument;
    }

    PlaybackPreflightRoute heldRoute{};
    if (!GetPlaybackPreflightRoute(heldRoute) ||
        heldRoute.channel != route.channel ||
        heldRoute.bandwidthUnits != route.bandwidthUnits ||
        heldRoute.sampleRateHz != route.sampleRateHz ||
        !heldRoute.deviceCommunicationStopped) {
        return kIOReturnNotReady;
    }

    bool expected = false;
    if (!stage5hInFlight_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        ASFW_LOG_WARNING(Audio,
                         "[RME] Stage 5H bounded integration already in flight GUID=0x%016llx",
                         deviceGuid_);
        return kIOReturnBusy;
    }

    // Consume the held route so repeated StartIO retries cannot enter while the
    // synchronous bounded test owns the FF800 engine and its IRM reservation.
    playbackPreflightRoute_.store(0U, std::memory_order_release);

    IOReturn finalStatus = prepareHost();
    bool hostPrepared = finalStatus == kIOReturnSuccess;
    bool startAccepted = false;
    bool stopAccepted = false;
    bool releaseAccepted = false;

    if (!hostPrepared) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H host cadence preparation failed kr=0x%x; proceeding with bounded cleanup",
                       finalStatus);
    }

    const uint32_t dataBlockQuadlets = DecodeChannelCount(route.sampleRateHz);
    if (hostPrepared && dataBlockQuadlets == 0U) {
        finalStatus = kIOReturnBadArgument;
    } else if (hostPrepared) {
        // Exact Stage 4D/5F FF800 begin-session value: communication enable,
        // S800 flag, and the 192 kHz data-block-quadlet count.
        const uint32_t startValue =
            0x80000000U | 0x00000800U | dataBlockQuadlets;
        const std::array<uint8_t, 4> startLE = {
            static_cast<uint8_t>(startValue & 0xFFU),
            static_cast<uint8_t>((startValue >> 8U) & 0xFFU),
            static_cast<uint8_t>((startValue >> 16U) & 0xFFU),
            static_cast<uint8_t>((startValue >> 24U) & 0xFFU),
        };
        const auto generation = busInfo_.GetGeneration();
        const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
        const auto startResult = ASFW::Audio::WaitForAsyncResult<bool>(
            [this, generation, node, startLE](auto done) {
                probeHandle_ = busOps_.WriteBlock(
                    generation,
                    node,
                    MakeAddress(kIsochCommStartAddress),
                    std::span<const uint8_t>{startLE},
                    FW::FwSpeed::S400,
                    [done](Async::AsyncStatus status,
                           std::span<const uint8_t> payload) mutable {
                        (void)payload;
                        const bool accepted =
                            status == Async::AsyncStatus::kSuccess;
                        done(accepted ? kIOReturnSuccess : kIOReturnIOError,
                             accepted);
                    });
                if (!probeHandle_.IsValid()) {
                    done(kIOReturnNoResources, false);
                }
            },
            500U,
            kIOReturnTimeout);
        startAccepted =
            startResult.status == kIOReturnSuccess && startResult.value;
        if (!startAccepted) {
            if (startResult.status == kIOReturnTimeout &&
                probeHandle_.IsValid()) {
                (void)busOps_.Cancel(probeHandle_);
            }
            finalStatus = startResult.status == kIOReturnSuccess
                ? kIOReturnIOError
                : startResult.status;
            ASFW_LOG_ERROR(Audio,
                           "[RME] Stage 5H FF800 playback-engine start failed kr=0x%x gen=%u value=0x%08x",
                           startResult.status,
                           generation.value,
                           startValue);
        } else {
            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5H FF800 playback engine started channel=%u rate=%u value=0x%08x",
                     route.channel,
                     route.sampleRateHz,
                     startValue);
            finalStatus = runHostBurst();
        }
    }

    // Cleanup is deliberately ordered on success and every failure path:
    // host RUN clear/ACTIVE drain first, then FF800 stop, then IRM release.
    const IOReturn hostCleanupStatus = cleanupHost();
    if (hostCleanupStatus != kIOReturnSuccess &&
        finalStatus == kIOReturnSuccess) {
        finalStatus = hostCleanupStatus;
    }
    if (hostCleanupStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H host cleanup failed kr=0x%x before FF800 stop",
                       hostCleanupStatus);
    }

    const std::array<uint8_t, 4> stopLE = {0x00U, 0x00U, 0x00U, 0x80U};
    const auto stopGeneration = busInfo_.GetGeneration();
    const auto stopNode = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
    const auto stopResult = ASFW::Audio::WaitForAsyncResult<bool>(
        [this, stopGeneration, stopNode, stopLE](auto done) {
            probeHandle_ = busOps_.WriteBlock(
                stopGeneration,
                stopNode,
                MakeAddress(kIsochCommStopAddress),
                std::span<const uint8_t>{stopLE},
                FW::FwSpeed::S400,
                [done](Async::AsyncStatus status,
                       std::span<const uint8_t> payload) mutable {
                    (void)payload;
                    const bool accepted =
                        status == Async::AsyncStatus::kSuccess;
                    done(accepted ? kIOReturnSuccess : kIOReturnIOError,
                         accepted);
                });
            if (!probeHandle_.IsValid()) {
                done(kIOReturnNoResources, false);
            }
        },
        500U,
        kIOReturnTimeout);
    stopAccepted =
        stopResult.status == kIOReturnSuccess && stopResult.value;
    if (!stopAccepted && finalStatus == kIOReturnSuccess) {
        finalStatus = stopResult.status == kIOReturnSuccess
            ? kIOReturnIOError
            : stopResult.status;
    }
    if (!stopAccepted) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H FF800 stop command failed kr=0x%x gen=%u",
                       stopResult.status,
                       stopGeneration.value);
    }

    deviceTxAllocationRequested_ = false;
    deviceTxAllocated_ = false;
    deviceTxChannel_ = 0xFFU;

    const bool hadReservation = playbackResourcesReserved_;
    const uint8_t reservedChannel = reservedPlaybackChannel_;
    const uint32_t reservedBandwidth = reservedPlaybackBandwidthUnits_;
    playbackResourcesReserved_ = false;
    reservedPlaybackChannel_ = 0xFFU;
    reservedPlaybackBandwidthUnits_ = 0U;

    if (!hadReservation || irmClient_ == nullptr || reservedChannel > 63U ||
        reservedBandwidth == 0U) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H cleanup missing held IRM route expectedChannel=%u expectedBandwidth=%u actualChannel=%u actualBandwidth=%u",
                       route.channel,
                       route.bandwidthUnits,
                       reservedChannel,
                       reservedBandwidth);
        if (finalStatus == kIOReturnSuccess) {
            finalStatus = kIOReturnNotReady;
        }
    } else {
        const auto releaseResult = ASFW::Audio::WaitForAsyncResult<bool>(
            [this, reservedChannel, reservedBandwidth](auto done) {
                irmClient_->ReleaseResources(
                    reservedChannel,
                    reservedBandwidth,
                    [done](IRM::AllocationStatus status) mutable {
                        const bool accepted =
                            status == IRM::AllocationStatus::Success;
                        done(accepted ? kIOReturnSuccess : kIOReturnIOError,
                             accepted);
                    });
            },
            1000U,
            kIOReturnTimeout);
        releaseAccepted =
            releaseResult.status == kIOReturnSuccess && releaseResult.value;
        if (!releaseAccepted && finalStatus == kIOReturnSuccess) {
            finalStatus = releaseResult.status == kIOReturnSuccess
                ? kIOReturnIOError
                : releaseResult.status;
        }
        if (!releaseAccepted) {
            ASFW_LOG_ERROR(Audio,
                           "[RME] Stage 5H IRM release failed kr=0x%x channel=%u bandwidth=%u",
                           releaseResult.status,
                           reservedChannel,
                           reservedBandwidth);
        }
    }

    stage5hInFlight_.store(false, std::memory_order_release);

    if (stopAccepted && releaseAccepted) {
        ASFW_LOG(Audio,
                 "[RME] ✅ Stage 5H FF800 playback engine stopped and IRM route released channel=%u bandwidth=%u",
                 reservedChannel,
                 reservedBandwidth);
    } else {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H cleanup completed with errors startAccepted=%u stopAccepted=%u releaseAccepted=%u",
                       startAccepted ? 1U : 0U,
                       stopAccepted ? 1U : 0U,
                       releaseAccepted ? 1U : 0U);
    }

    if (finalStatus == kIOReturnSuccess) {
        ASFW_LOG(Audio,
                 "[RME] ✅ Stage 5H bounded circular silent playback integration passed; Core Audio StartIO remains rejected");
    } else {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5H bounded circular silent playback integration failed kr=0x%x; Core Audio StartIO remains rejected",
                       finalStatus);
    }
    return finalStatus;
}

void RMEFireface800Protocol::ProbeClockConfig() noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.ReadBlock(
        generation,
        node,
        MakeAddress(kClockConfigAddress),
        4,
        FW::FwSpeed::S400,
        [this, generation](Async::AsyncStatus status,
                           std::span<const uint8_t> payload) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }

            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Clock probe failed status=%{public}s bytes=%zu gen=%u",
                               Async::ToString(status),
                               payload.size(),
                               generation.value);
                return;
            }

            const uint32_t raw = ReadLE32(payload.data());
            const uint32_t rate = DecodeSampleRate(raw);
            currentRate_ = rate;
            const char* source = DecodeClockSource(raw);

            ASFW_LOG(Audio,
                     "[RME] ✅ Clock raw=0x%08x rate=%u source=%{public}s gen=%u",
                     raw,
                     rate,
                     source,
                     generation.value);
            ASFW_LOG(Audio,
                     "[RME] Clock options: spdifOut=%{public}s emphasis=%{public}s opticalOut=%{public}s wordSingleSpeed=%{public}s spdifIn=%{public}s",
                     (raw & 0x00000020U) != 0U ? "professional" : "consumer",
                     (raw & 0x00000040U) != 0U ? "on" : "off",
                     (raw & 0x00000100U) != 0U ? "S/PDIF" : "ADAT",
                     (raw & 0x00002000U) != 0U ? "on" : "off",
                     (raw & 0x00000200U) != 0U ? "optical" : "coaxial");

            LogStreamCaps(rate);
            if (rate == 0U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F STF write skipped: decoded rate is zero");
                ProbeSyncStatus();
                return;
            }
            ValidateStfWritePath(rate);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] Clock probe could not be submitted");
    }
}

void RMEFireface800Protocol::ValidateStfWritePath(uint32_t rate) noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
    const std::array<uint8_t, 4> rateLE = {
        static_cast<uint8_t>(rate & 0xFFU),
        static_cast<uint8_t>((rate >> 8U) & 0xFFU),
        static_cast<uint8_t>((rate >> 16U) & 0xFFU),
        static_cast<uint8_t>((rate >> 24U) & 0xFFU),
    };

    ASFW_LOG(Audio,
             "[RME] Stage 5F writing current STF=%u Hz (no rate change) gen=%u",
             rate,
             generation.value);

    probeHandle_ = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kStfAddress),
        std::span<const uint8_t>{rateLE},
        FW::FwSpeed::S400,
        [this, generation, rate](Async::AsyncStatus status,
                                 std::span<const uint8_t> payload) {
            (void)payload;
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F STF write failed status=%{public}s gen=%u",
                               Async::ToString(status),
                               generation.value);
                ProbeSyncStatus();
                return;
            }

            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F STF write accepted: %u Hz; waiting 100 ms before verification",
                     rate);
            IOSleep(100);
            VerifyClockAfterStfWrite(rate);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F STF write could not be submitted");
        ProbeSyncStatus();
    }
}

void RMEFireface800Protocol::VerifyClockAfterStfWrite(uint32_t expectedRate) noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.ReadBlock(
        generation,
        node,
        MakeAddress(kClockConfigAddress),
        4,
        FW::FwSpeed::S400,
        [this, generation, expectedRate](Async::AsyncStatus status,
                                         std::span<const uint8_t> payload) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F clock verification failed status=%{public}s bytes=%zu gen=%u",
                               Async::ToString(status),
                               payload.size(),
                               generation.value);
                ProbeSyncStatus();
                return;
            }

            const uint32_t raw = ReadLE32(payload.data());
            const uint32_t actualRate = DecodeSampleRate(raw);
            if (actualRate == expectedRate) {
                ASFW_LOG(Audio,
                         "[RME] ✅ Stage 5F STF write-path verified: expected=%u actual=%u raw=0x%08x",
                         expectedRate,
                         actualRate,
                         raw);
            } else {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F STF verification mismatch: expected=%u actual=%u raw=0x%08x",
                               expectedRate,
                               actualRate,
                               raw);
            }
            ProbeSyncStatus();
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F clock verification could not be submitted");
        ProbeSyncStatus();
    }
}

void RMEFireface800Protocol::ProbeSyncStatus() noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.ReadBlock(
        generation,
        node,
        MakeAddress(kSyncStatusAddress),
        8,
        FW::FwSpeed::S400,
        [this, generation](Async::AsyncStatus status,
                           std::span<const uint8_t> payload) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }

            if (status != Async::AsyncStatus::kSuccess || payload.size() < 8U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Sync-status probe failed status=%{public}s bytes=%zu gen=%u",
                               Async::ToString(status),
                               payload.size(),
                               generation.value);
                return;
            }

            const uint32_t status0 = ReadLE32(payload.data());
            const uint32_t status1 = ReadLE32(payload.data() + 4U);
            const char* referred = DecodeReferredClockSource(status0, status1);
            const uint32_t referredRate = DecodeReferredRate(status0);

            ASFW_LOG(Audio,
                     "[RME] ✅ Sync status raw0=0x%08x raw1=0x%08x gen=%u",
                     status0,
                     status1,
                     generation.value);
            ASFW_LOG(Audio,
                     "[RME] External sync: word=%{public}s spdif=%{public}s adat1=%{public}s adat2=%{public}s",
                     DecodeExternalState(status0, 0x40000000U, 0x20000000U),
                     DecodeExternalState(status0, 0x00080000U, 0x00040000U),
                     DecodeExternalState(status0, 0x00000400U, 0x00001000U),
                     DecodeExternalState(status0, 0x00000800U, 0x00002000U));
            ASFW_LOG(Audio,
                     "[RME] Referred clock: source=%{public}s rate=%u",
                     referred,
                     referredRate);

            ProbeTxIsochChannel();
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] Sync-status probe could not be submitted");
    }
}

void RMEFireface800Protocol::ProbeTxIsochChannel() noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.ReadBlock(
        generation,
        node,
        MakeAddress(kTxIsochChannelAddress),
        4,
        FW::FwSpeed::S400,
        [this, generation](Async::AsyncStatus status,
                           std::span<const uint8_t> payload) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }

            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] TX isoch-channel probe failed status=%{public}s bytes=%zu gen=%u",
                               Async::ToString(status),
                               payload.size(),
                               generation.value);
                return;
            }

            const uint32_t raw = ReadLE32(payload.data());
            if (raw == 0xFFFFFFFFU) {
                ASFW_LOG(Audio,
                         "[RME] ✅ TX isoch channel: inactive/not allocated raw=0xffffffff gen=%u",
                         generation.value);
            } else {
                ASFW_LOG(Audio,
                         "[RME] ✅ TX isoch channel reported raw=0x%08x channel=%u gen=%u",
                         raw,
                         raw & 0x3FU,
                         generation.value);
            }
            ReservePlaybackResourcesPreflight(currentRate_);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] TX isoch-channel probe could not be submitted");
    }
}


void RMEFireface800Protocol::ReservePlaybackResourcesPreflight(uint32_t rate) noexcept {
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    if (playbackResourcesReserved_) {
        ASFW_LOG(Audio,
                 "[RME] Stage 5F playback resources already reserved channel=%u bandwidth=%u",
                 reservedPlaybackChannel_,
                 reservedPlaybackBandwidthUnits_);
        return;
    }
    if (irmClient_ == nullptr) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F cannot reserve playback resources: IRM client unavailable");
        return;
    }

    const uint32_t bandwidthUnits = CalculatePlaybackBandwidthUnits(rate);
    if (bandwidthUnits == 0U) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F cannot reserve playback resources: invalid rate=%u",
                       rate);
        return;
    }

    irmClient_->ReadResourcesSnapshot(
        [this, rate, bandwidthUnits](IRM::AllocationStatus status, IRM::ResourceSnapshot snapshot) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != IRM::AllocationStatus::Success) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F IRM snapshot failed status=%{public}s",
                               IRM::ToString(status));
                return;
            }

            const uint8_t channel =
                SelectAvailableChannel(snapshot.channelsAvailable31_0,
                                       snapshot.channelsAvailable63_32);
            ASFW_LOG(Audio,
                     "[RME] Stage 5F IRM snapshot bandwidthAvailable=%u requested=%u selectedChannel=%u",
                     snapshot.bandwidthAvailable,
                     bandwidthUnits,
                     channel);

            if (channel == 0xFFU || snapshot.bandwidthAvailable < bandwidthUnits) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F insufficient IRM resources bandwidthAvailable=%u requested=%u channel=%u",
                               snapshot.bandwidthAvailable,
                               bandwidthUnits,
                               channel);
                return;
            }

            irmClient_->AllocateResources(
                channel,
                bandwidthUnits,
                [this, rate, channel, bandwidthUnits](IRM::AllocationStatus allocateStatus) {
                    if (allocateStatus != IRM::AllocationStatus::Success) {
                        ASFW_LOG_ERROR(Audio,
                                       "[RME] Stage 5F IRM reservation failed status=%{public}s channel=%u bandwidth=%u",
                                       IRM::ToString(allocateStatus),
                                       channel,
                                       bandwidthUnits);
                        return;
                    }

                    if (!active_.load(std::memory_order_acquire)) {
                        irmClient_->ReleaseResources(
                            channel, bandwidthUnits, [](IRM::AllocationStatus) {});
                        return;
                    }

                    reservedPlaybackChannel_ = channel;
                    reservedPlaybackBandwidthUnits_ = bandwidthUnits;
                    playbackResourcesReserved_ = true;
                    ASFW_LOG(Audio,
                             "[RME] ✅ Stage 5F IRM playback reservation acquired channel=%u bandwidth=%u",
                             channel,
                             bandwidthUnits);
                    ProgramRxPacketFormat(rate, channel, bandwidthUnits);
                });
        });
}

void RMEFireface800Protocol::ProgramRxPacketFormat(uint32_t rate,
                                                    uint8_t channel,
                                                    uint32_t bandwidthUnits) noexcept {
    const uint32_t dataBlockQuadlets = DecodeChannelCount(rate);
    if (dataBlockQuadlets == 0U || channel > 63U) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F RX packet-format skipped rate=%u dbq=%u channel=%u",
                       rate,
                       dataBlockQuadlets,
                       channel);
        ReleaseReservedResources();
        return;
    }

    // Linux ff-protocol-former.c programs ((DBQ << 3) << 8) | channel.
    const uint32_t format = ((dataBlockQuadlets << 3U) << 8U) | channel;
    const std::array<uint8_t, 4> formatLE = {
        static_cast<uint8_t>(format & 0xFFU),
        static_cast<uint8_t>((format >> 8U) & 0xFFU),
        static_cast<uint8_t>((format >> 16U) & 0xFFU),
        static_cast<uint8_t>((format >> 24U) & 0xFFU),
    };

    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
    probeHandle_ = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kRxPacketFormatAddress),
        std::span<const uint8_t>{formatLE},
        FW::FwSpeed::S400,
        [this, generation, rate, channel, bandwidthUnits, format](
            Async::AsyncStatus status, std::span<const uint8_t> payload) {
            (void)payload;
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F RX packet-format write failed status=%{public}s gen=%u",
                               Async::ToString(status),
                               generation.value);
                ReleaseReservedResources();
                return;
            }

            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F playback route armed rate=%u dbq=%u channel=%u bandwidth=%u format=0x%08x",
                     rate,
                     DecodeChannelCount(rate),
                     channel,
                     bandwidthUnits,
                     format);
            RequestDeviceTxAllocation(rate);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F RX packet-format write could not be submitted");
        ReleaseReservedResources();
    }
}

void RMEFireface800Protocol::RequestDeviceTxAllocation(uint32_t rate) noexcept {
    const uint32_t dataBlockQuadlets = DecodeChannelCount(rate);
    if (dataBlockQuadlets == 0U) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F TX allocation skipped: invalid rate=%u",
                       rate);
        ReleaseReservedResources();
        return;
    }

    const std::array<uint8_t, 4> dbqLE = {
        static_cast<uint8_t>(dataBlockQuadlets & 0xFFU),
        static_cast<uint8_t>((dataBlockQuadlets >> 8U) & 0xFFU),
        static_cast<uint8_t>((dataBlockQuadlets >> 16U) & 0xFFU),
        static_cast<uint8_t>((dataBlockQuadlets >> 24U) & 0xFFU),
    };

    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
    probeHandle_ = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kAllocTxStreamAddress),
        std::span<const uint8_t>{dbqLE},
        FW::FwSpeed::S400,
        [this, generation, rate, dataBlockQuadlets](
            Async::AsyncStatus status, std::span<const uint8_t> payload) {
            (void)payload;
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F device TX allocation request failed status=%{public}s gen=%u",
                               Async::ToString(status),
                               generation.value);
                ReleaseReservedResources();
                return;
            }

            deviceTxAllocationRequested_ = true;
            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F device TX allocation request accepted dbq=%u; polling channel",
                     dataBlockQuadlets);
            PollDeviceTxIsochChannel(rate, 1U);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F device TX allocation request could not be submitted");
        ReleaseReservedResources();
    }
}

void RMEFireface800Protocol::PollDeviceTxIsochChannel(uint32_t rate,
                                                       uint32_t attempt) noexcept {
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.ReadBlock(
        generation,
        node,
        MakeAddress(kTxIsochChannelAddress),
        4,
        FW::FwSpeed::S400,
        [this, generation, rate, attempt](Async::AsyncStatus status,
                                          std::span<const uint8_t> payload) {
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F TX channel poll failed status=%{public}s bytes=%zu attempt=%u gen=%u",
                               Async::ToString(status),
                               payload.size(),
                               attempt,
                               generation.value);
                StopDeviceCommunicationPreflight();
                return;
            }

            const uint32_t raw = ReadLE32(payload.data());
            if (raw == 0xFFFFFFFFU) {
                if (attempt >= 10U) {
                    ASFW_LOG_ERROR(Audio,
                                   "[RME] Stage 5F device TX allocation timed out after %u polls",
                                   attempt);
                    StopDeviceCommunicationPreflight();
                    return;
                }

                IOSleep(50);
                PollDeviceTxIsochChannel(rate, attempt + 1U);
                return;
            }

            const uint8_t channel = static_cast<uint8_t>(raw & 0x3FU);
            deviceTxChannel_ = channel;
            deviceTxAllocated_ = true;
            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F device TX channel allocated raw=0x%08x channel=%u polls=%u rate=%u",
                     raw,
                     channel,
                     attempt,
                     rate);
            StartDeviceCommunicationPreflight(rate);
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F TX channel poll could not be submitted attempt=%u",
                       attempt);
        StopDeviceCommunicationPreflight();
    }
}

void RMEFireface800Protocol::StartDeviceCommunicationPreflight(uint32_t rate) noexcept {
    const uint32_t dataBlockQuadlets = DecodeChannelCount(rate);
    if (dataBlockQuadlets == 0U) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F start command skipped: invalid rate=%u",
                       rate);
        StopDeviceCommunicationPreflight();
        return;
    }

    // Linux ff800_begin_session(): 0x80000000 | DBQ | S800 flag.
    // This machine is linked at S800, verified by topology/self-ID.
    const uint32_t startValue = 0x80000000U | 0x00000800U | dataBlockQuadlets;
    const std::array<uint8_t, 4> startLE = {
        static_cast<uint8_t>(startValue & 0xFFU),
        static_cast<uint8_t>((startValue >> 8U) & 0xFFU),
        static_cast<uint8_t>((startValue >> 16U) & 0xFFU),
        static_cast<uint8_t>((startValue >> 24U) & 0xFFU),
    };

    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};
    probeHandle_ = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kIsochCommStartAddress),
        std::span<const uint8_t>{startLE},
        FW::FwSpeed::S400,
        [this, generation, rate, startValue](Async::AsyncStatus status,
                                             std::span<const uint8_t> payload) {
            (void)payload;
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F start command failed status=%{public}s gen=%u value=0x%08x",
                               Async::ToString(status),
                               generation.value,
                               startValue);
                StopDeviceCommunicationPreflight();
                return;
            }

            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F device isoch start accepted rate=%u dbq=%u txChannel=%u playbackChannel=%u value=0x%08x; running 100 ms without OHCI contexts",
                     rate,
                     DecodeChannelCount(rate),
                     deviceTxChannel_,
                     reservedPlaybackChannel_,
                     startValue);
            IOSleep(100);
            StopDeviceCommunicationPreflight();
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F start command could not be submitted");
        StopDeviceCommunicationPreflight();
    }
}

void RMEFireface800Protocol::StopDeviceCommunicationPreflight() noexcept {
    const std::array<uint8_t, 4> stopLE = {0x00U, 0x00U, 0x00U, 0x80U};
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    probeHandle_ = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kIsochCommStopAddress),
        std::span<const uint8_t>{stopLE},
        FW::FwSpeed::S400,
        [this, generation](Async::AsyncStatus status,
                           std::span<const uint8_t> payload) {
            (void)payload;
            if (!active_.load(std::memory_order_acquire)) {
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F stop command failed status=%{public}s gen=%u; releasing host IRM reservation",
                               Async::ToString(status),
                               generation.value);
                deviceTxAllocationRequested_ = false;
                deviceTxAllocated_ = false;
                deviceTxChannel_ = 0xFFU;
                ReleaseReservedResources();
                return;
            }

            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F stop command accepted; FF800 TX channel may remain latched by design (channel=%u)",
                     deviceTxChannel_);
            deviceTxAllocationRequested_ = false;
            deviceTxAllocated_ = false;
            deviceTxChannel_ = 0xFFU;
            const uint64_t routeSnapshot =
                (static_cast<uint64_t>(currentRate_) << 32U) |
                (static_cast<uint64_t>(
                     reservedPlaybackBandwidthUnits_ & 0x00FFFFFFU)
                 << 8U) |
                static_cast<uint64_t>(reservedPlaybackChannel_);
            playbackPreflightRoute_.store(
                playbackResourcesReserved_ ? routeSnapshot : 0U,
                std::memory_order_release);
            ASFW_LOG(Audio,
                     "[RME] ✅ Stage 5F playback route held for bounded host packet test channel=%u bandwidth=%u deviceStopped=1",
                     reservedPlaybackChannel_,
                     reservedPlaybackBandwidthUnits_);
            RunNoCipWireFormatSelfTest();
        });

    if (!probeHandle_.IsValid()) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F stop command could not be submitted; releasing host IRM reservation");
        deviceTxAllocationRequested_ = false;
        deviceTxAllocated_ = false;
        deviceTxChannel_ = 0xFFU;
        ReleaseReservedResources();
    }
}

void RMEFireface800Protocol::RunNoCipWireFormatSelfTest() noexcept {
    using namespace ASFW::Protocols::Audio::AMDTP;

    constexpr uint32_t kCycles = 8U;
    constexpr uint32_t kSlotCount = 4U;
    constexpr uint32_t kFramesPerDataPacket = 32U;
    constexpr uint32_t kDbq = 12U;
    constexpr uint32_t kExpectedDataPackets = 6U;
    constexpr uint32_t kExpectedEmptyPackets = 2U;
    constexpr uint32_t kExpectedFrames = 192U;
    constexpr uint32_t kDataPacketBytes = kFramesPerDataPacket * kDbq * 4U;

    std::array<PacketTimelineSlot, kSlotCount> slots{};
    AmdtpPacketTimeline timeline{};
    if (!timeline.AttachSlots(slots.data(), static_cast<uint32_t>(slots.size()))) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F wire self-test failed: timeline attach");
        return;
    }

    auto packetStorage = std::unique_ptr<uint8_t[]>(
        new (std::nothrow) uint8_t[kSlotCount * kDataPacketBytes]);
    if (!packetStorage) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F wire self-test failed: packet storage allocation");
        return;
    }

    AmdtpStreamConfig config{};
    config.sampleRate = 192000U;
    config.streamMode = StreamMode::Blocking;
    config.sid = 1U;
    config.dbs = static_cast<uint8_t>(kDbq);
    config.pcmChannels = static_cast<uint8_t>(kDbq);
    config.midiSlots = 0U;
    config.framesPerDataPacket = static_cast<uint8_t>(kFramesPerDataPacket);
    config.maxPacketBytes = kDataPacketBytes;
    config.includeCipHeader = false;

    AmdtpTxPolicy policy{};
    policy.hostToDevicePcmEncoding = PcmSlotEncoding::RawSigned24In32BE;
    policy.initializeNonAudioSlots = false;
    policy.emptyPacketsDuringIdle = true;

    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    if (!packetizer.Configure(config, policy)) {
        ASFW_LOG_ERROR(Audio, "[RME] Stage 5F wire self-test failed: packetizer configure");
        return;
    }
    packetizer.Reset(0U, 0U);

    uint32_t dataPackets = 0U;
    uint32_t emptyPackets = 0U;
    uint32_t frames = 0U;
    bool geometryValid = true;
    bool payloadZero = true;
    bool ohciMetadataValid = true;
    uint32_t metadataCommits = 0U;
    uint32_t skipCycles = 0U;

    Async::HW::OHCIDescriptor skipDescriptor{};
    Async::HW::ITDescriptorBuilder::BuildOutputLast(
        skipDescriptor,
        {
            .dataIOVA = 0U,
            .payloadSize = 0U,
            .branchIOVA = 0x00001000U,
            .zValue = 4U,
            .interruptBits = Async::HW::OHCIDescriptor::kIntNever,
        });
    const uint32_t skipControlHigh =
        skipDescriptor.control >>
        Async::HW::OHCIDescriptor::kControlHighShift;
    const bool skipDescriptorValid =
        ((skipControlHigh >> Async::HW::OHCIDescriptor::kCmdShift) & 0xFU) ==
            Async::HW::OHCIDescriptor::kCmdOutputLast &&
        ((skipControlHigh >> Async::HW::OHCIDescriptor::kKeyShift) & 0x7U) ==
            Async::HW::OHCIDescriptor::kKeyStandard &&
        ((skipControlHigh >> Async::HW::OHCIDescriptor::kBranchShift) & 0x3U) ==
            Async::HW::OHCIDescriptor::kBranchAlways &&
        (skipDescriptor.control & 0xFFFFU) == 0U &&
        skipDescriptor.dataAddress == 0U &&
        Async::HW::DecodeBranchPhys32_AT(skipDescriptor.branchWord) ==
            0x00001000U &&
        (skipDescriptor.branchWord & 0xFU) == 4U &&
        !Async::HW::IsImmediate(skipDescriptor);

    for (uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        uint8_t* packetBytes = packetStorage.get() +
            (cycle % kSlotCount) * kDataPacketBytes;
        for (uint32_t i = 0; i < kDataPacketBytes; ++i) {
            packetBytes[i] = 0xA5U;
        }

        TxPacketSlotView slot{
            .packetIndex = cycle,
            .bytes = packetBytes,
            .capacityBytes = kDataPacketBytes,
        };
        AmdtpTimingState timing{};
        timing.disposition = AmdtpPacketDisposition::Data;

        PreparedTxPacket packet{};
        if (!packetizer.PrepareNextPacket(slot, timing, packet)) {
            ASFW_LOG_ERROR(Audio,
                           "[RME] Stage 5F wire self-test failed: packet preparation cycle=%u",
                           cycle);
            return;
        }

        if (packet.isData) {
            ++dataPackets;
            frames += packet.framesInPacket;
            if (packet.byteCount != kDataPacketBytes ||
                packet.framesInPacket != kFramesPerDataPacket ||
                packet.dbs != kDbq) {
                geometryValid = false;
            }
            for (uint32_t i = 0; i < packet.byteCount; ++i) {
                if (packetBytes[i] != 0U) {
                    payloadZero = false;
                    break;
                }
            }
        } else {
            ++emptyPackets;
            if (packet.byteCount != 0U || packet.framesInPacket != 0U) {
                geometryValid = false;
            }
        }

        // Exercise the same plain helper used by DextTxSlotProvider to publish
        // OHCI metadata. FF800 packets must use tag 0 (no CIP), S400, tcode A,
        // and data_length equal to the raw payload size (or zero when idle).
        const uint32_t headerQ0 = ASFW::IsochTransport::BuildIsochTxHeaderQ0(
            ASFW::IsochTransport::IsochPacketTag::kNoCipHeader);
        const uint32_t headerQ1 =
            ASFW::IsochTransport::BuildIsochTxHeaderQ1(packet.byteCount);
        const uint32_t expectedLength = packet.isData ? kDataPacketBytes : 0U;
        const auto decodedTag =
            ASFW::IsochTransport::DecodeIsochTxHeaderTag(headerQ0);
        const bool shouldSkip =
            ASFW::IsochTransport::ShouldSkipIsochTxPacket(
                decodedTag, packet.byteCount);
        if (shouldSkip) {
            ++skipCycles;
        }
        if (decodedTag !=
                ASFW::IsochTransport::IsochPacketTag::kNoCipHeader ||
            ASFW::IsochTransport::DecodeIsochTxHeaderSpeed(headerQ0) !=
                ASFW::IsochTransport::kIsochSpeedS400 ||
            ASFW::IsochTransport::DecodeIsochTxHeaderTCode(headerQ0) !=
                ASFW::IsochTransport::kIsochDataBlockTCode ||
            ASFW::IsochTransport::DecodeIsochTxHeaderDataLength(headerQ1) !=
                expectedLength ||
            shouldSkip != !packet.isData) {
            ohciMetadataValid = false;
        } else {
            ++metadataCommits;
        }
    }

    if (dataPackets == kExpectedDataPackets &&
        emptyPackets == kExpectedEmptyPackets &&
        frames == kExpectedFrames && geometryValid && payloadZero &&
        ohciMetadataValid && metadataCommits == kCycles &&
        skipCycles == kExpectedEmptyPackets && skipDescriptorValid) {
        ASFW_LOG(Audio,
                 "[RME] ✅ Stage 5F FF800 no-CIP wire self-test passed cycles=%u data=%u empty=%u frames=%u dataBytes=%u dbq=%u headerBytes=0 payload=silence",
                 kCycles,
                 dataPackets,
                 emptyPackets,
                 frames,
                 kDataPacketBytes,
                 kDbq);
        ASFW_LOG(Audio,
                 "[RME] ✅ Stage 5F FF800 OHCI metadata/skip self-test passed cycles=%u tag=0 speed=S400 tcode=0x%x dataLength=%u skipCycles=%u descriptor=OUTPUT_LAST/standard/zero/Z4 commits=%u",
                 kCycles,
                 ASFW::IsochTransport::kIsochDataBlockTCode,
                 kDataPacketBytes,
                 skipCycles,
                 metadataCommits);
        ASFW_LOG(Audio,
                 "[RME] Stage 5F complete: raw no-CIP packet bytes, OHCI tag/data-length metadata, and true idle skip semantics are verified; OHCI contexts and Core Audio streaming remain disabled");
    } else {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F wire self-test mismatch data=%u/%u empty=%u/%u frames=%u/%u geometry=%u zero=%u",
                       dataPackets,
                       kExpectedDataPackets,
                       emptyPackets,
                       kExpectedEmptyPackets,
                       frames,
                       kExpectedFrames,
                       geometryValid ? 1U : 0U,
                       payloadZero ? 1U : 0U);
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F OHCI metadata/skip mismatch valid=%u descriptor=%u skip=%u/%u commits=%u/%u",
                       ohciMetadataValid ? 1U : 0U,
                       skipDescriptorValid ? 1U : 0U,
                       skipCycles,
                       kExpectedEmptyPackets,
                       metadataCommits,
                       kCycles);
    }
}

void RMEFireface800Protocol::BestEffortStopDeviceCommunication() noexcept {
    if (!deviceTxAllocationRequested_ && !deviceTxAllocated_) {
        return;
    }

    const std::array<uint8_t, 4> stopLE = {0x00U, 0x00U, 0x00U, 0x80U};
    const auto generation = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3FU)};

    deviceTxAllocationRequested_ = false;
    deviceTxAllocated_ = false;
    deviceTxChannel_ = 0xFFU;

    const auto handle = busOps_.WriteBlock(
        generation,
        node,
        MakeAddress(kIsochCommStopAddress),
        std::span<const uint8_t>{stopLE},
        FW::FwSpeed::S400,
        [generation](Async::AsyncStatus status,
                     std::span<const uint8_t> payload) {
            (void)payload;
            if (status == Async::AsyncStatus::kSuccess) {
                ASFW_LOG(Audio,
                         "[RME] Stage 5F best-effort stop submitted during shutdown gen=%u",
                         generation.value);
            } else {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F best-effort stop failed during shutdown status=%{public}s gen=%u",
                               Async::ToString(status),
                               generation.value);
            }
        });

    if (!handle.IsValid()) {
        ASFW_LOG_ERROR(Audio,
                       "[RME] Stage 5F best-effort stop could not be submitted during shutdown");
    }
}

void RMEFireface800Protocol::ReleaseReservedResources() noexcept {
    playbackPreflightRoute_.store(0U, std::memory_order_release);
    stage5hInFlight_.store(false, std::memory_order_release);
    if (!playbackResourcesReserved_ || irmClient_ == nullptr) {
        return;
    }

    const uint8_t channel = reservedPlaybackChannel_;
    const uint32_t bandwidthUnits = reservedPlaybackBandwidthUnits_;
    playbackResourcesReserved_ = false;
    reservedPlaybackChannel_ = 0xFFU;
    reservedPlaybackBandwidthUnits_ = 0U;

    irmClient_->ReleaseResources(
        channel,
        bandwidthUnits,
        [channel, bandwidthUnits](IRM::AllocationStatus status) {
            if (status == IRM::AllocationStatus::Success) {
                ASFW_LOG(Audio,
                         "[RME] ✅ Stage 5F released playback IRM reservation channel=%u bandwidth=%u",
                         channel,
                         bandwidthUnits);
            } else {
                ASFW_LOG_ERROR(Audio,
                               "[RME] Stage 5F playback IRM release status=%{public}s channel=%u bandwidth=%u",
                               IRM::ToString(status),
                               channel,
                               bandwidthUnits);
            }
        });
}

void RMEFireface800Protocol::LogStreamCaps(uint32_t rate) const noexcept {
    const uint32_t channels = DecodeChannelCount(rate);
    ASFW_LOG(Audio,
             "[RME] Stream geometry: mode=%{public}s rate=%u captureChannels=%u playbackChannels=%u sampleFormat=PCM24-in-32",
             DecodeStreamMode(rate),
             rate,
             channels,
             channels);
    ASFW_LOG(Audio,
             "[RME] Supported FF800 geometry: low=28x28 mid=20x20 high=12x12 MIDI=1-in/1-out");
}

uint32_t RMEFireface800Protocol::ReadLE32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

uint32_t RMEFireface800Protocol::DecodeSampleRate(uint32_t value) noexcept {
    switch (value & 0x0000001EU) {
    case 0x00000002U: return 32000;
    case 0x00000000U: return 44100;
    case 0x00000006U: return 48000;
    case 0x0000000AU: return 64000;
    case 0x00000008U: return 88200;
    case 0x0000000EU: return 96000;
    case 0x00000012U: return 128000;
    case 0x00000010U: return 176400;
    case 0x00000016U: return 192000;
    default: return 0;
    }
}

const char* RMEFireface800Protocol::DecodeClockSource(uint32_t value) noexcept {
    if ((value & 0x00000001U) != 0U) {
        return "internal";
    }

    switch (value & 0x00001C00U) {
    case 0x00000000U: return "ADAT1";
    case 0x00000400U: return "ADAT2";
    case 0x00000C00U: return "S/PDIF";
    case 0x00001000U: return "word clock";
    case 0x00001800U: return "LTC/TCO";
    default: return "unknown external";
    }
}

const char* RMEFireface800Protocol::DecodeExternalState(uint32_t value,
                                                         uint32_t lockedMask,
                                                         uint32_t syncedMask) noexcept {
    if ((value & lockedMask) == 0U) {
        return "none";
    }
    return (value & syncedMask) != 0U ? "sync" : "lock";
}

const char* RMEFireface800Protocol::DecodeReferredClockSource(uint32_t status0,
                                                               uint32_t status1) noexcept {
    if ((status1 & 0x00000001U) != 0U) {
        return "internal";
    }

    switch (status0 & 0x01C00000U) {
    case 0x00000000U: return "ADAT1";
    case 0x00400000U: return "ADAT2";
    case 0x00C00000U: return "S/PDIF";
    case 0x01000000U: return "word clock";
    case 0x01400000U: return "TCO";
    default: return "none";
    }
}

uint32_t RMEFireface800Protocol::DecodeReferredRate(uint32_t status0) noexcept {
    switch (status0 & 0x1E000000U) {
    case 0x02000000U: return 32000;
    case 0x04000000U: return 44100;
    case 0x06000000U: return 48000;
    case 0x08000000U: return 64000;
    case 0x0A000000U: return 88200;
    case 0x0C000000U: return 96000;
    case 0x0E000000U: return 128000;
    case 0x10000000U: return 176400;
    case 0x12000000U: return 192000;
    default: return 0;
    }
}

const char* RMEFireface800Protocol::DecodeStreamMode(uint32_t rate) noexcept {
    switch (rate) {
    case 32000:
    case 44100:
    case 48000:
        return "low/single-speed";
    case 64000:
    case 88200:
    case 96000:
        return "mid/double-speed";
    case 128000:
    case 176400:
    case 192000:
        return "high/quad-speed";
    default:
        return "unknown";
    }
}

uint32_t RMEFireface800Protocol::DecodeChannelCount(uint32_t rate) noexcept {
    switch (rate) {
    case 32000:
    case 44100:
    case 48000:
        return 28;
    case 64000:
    case 88200:
    case 96000:
        return 20;
    case 128000:
    case 176400:
    case 192000:
        return 12;
    default:
        return 0;
    }
}

uint32_t RMEFireface800Protocol::DecodeSytInterval(uint32_t rate) noexcept {
    switch (rate) {
    case 32000:
    case 44100:
    case 48000:
        return 8U;
    case 64000:
    case 88200:
    case 96000:
        return 16U;
    case 128000:
    case 176400:
    case 192000:
        return 32U;
    default:
        return 0U;
    }
}

uint32_t RMEFireface800Protocol::CalculatePlaybackBandwidthUnits(uint32_t rate) noexcept {
    const uint32_t dataBlockQuadlets = DecodeChannelCount(rate);
    const uint32_t sytInterval = DecodeSytInterval(rate);
    if (dataBlockQuadlets == 0U || sytInterval == 0U) {
        return 0U;
    }

    // FF800 uses blocking raw PCM with no CIP header. The maximum payload is
    // SYT interval * data-block quadlets * 4 bytes. Match the project's
    // conservative Linux iso-resources fallback: 12-byte packet overhead,
    // S800 scaling, then 512 allocation units of gap-count fallback margin.
    const uint32_t maxPayloadBytes = sytInterval * dataBlockQuadlets * 4U;
    const uint32_t alignedPayloadBytes = (maxPayloadBytes + 3U) & ~3U;
    const uint32_t packetBytesAtS800 = (12U + alignedPayloadBytes + 1U) / 2U;
    return packetBytesAtS800 + 512U;
}

uint8_t RMEFireface800Protocol::SelectAvailableChannel(uint32_t channels31_0,
                                                       uint32_t channels63_32) noexcept {
    for (uint8_t channel = 0; channel < 64U; ++channel) {
        const uint32_t reg = channel < 32U ? channels31_0 : channels63_32;
        const uint32_t mask = uint32_t{1} << (31U - (channel % 32U));
        if ((reg & mask) != 0U) {
            return channel;
        }
    }
    return 0xFFU;
}

} // namespace ASFW::Audio::RME
