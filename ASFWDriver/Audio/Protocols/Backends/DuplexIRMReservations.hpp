// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexIRMReservations.hpp - bounded lifecycle owner for duplex IRM resources

#pragma once

#include "../AudioTypes.hpp"
#include "SyncAsyncBridge.hpp"
#include "../../../Bus/IRM/IRMClient.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>

#include <array>
#include <atomic>
#include <memory>

namespace ASFW::Audio::Backends {

struct IRMReservationResult final {
    kern_return_t status{kIOReturnError};
    uint8_t channel{AudioStreamWireInfo::kInvalidIsoChannel};
};

class DuplexIRMReservations final {
  public:
    DuplexIRMReservations() = default;
    ~DuplexIRMReservations() { ReleaseAll(); }

    DuplexIRMReservations(const DuplexIRMReservations&) = delete;
    DuplexIRMReservations& operator=(const DuplexIRMReservations&) = delete;

    [[nodiscard]] kern_return_t Reserve(IRM::IRMClient& client, uint8_t channel,
                                        uint32_t bandwidthUnits) noexcept {
        return ReserveSpecific(client, channel, bandwidthUnits);
    }

    [[nodiscard]] IRMReservationResult ReserveAny(IRM::IRMClient& client,
                                                  uint64_t allowedChannels,
                                                  uint32_t bandwidthUnits) noexcept {
        if (count_ >= entries_.size()) {
            return {.status = kIOReturnNoResources};
        }
        if (allowedChannels == 0) {
            return {.status = kIOReturnNoResources};
        }

        uint64_t candidates = allowedChannels;
        while (candidates != 0) {
            const auto snapshotRes = WaitForAsyncResult<std::pair<IRM::AllocationStatus, IRM::ResourceSnapshot>>(
                [&client](auto callback) {
                    client.ReadResourcesSnapshot(
                        [cb = std::move(callback)](IRM::AllocationStatus status, IRM::ResourceSnapshot snapshot) mutable {
                            cb(kIOReturnSuccess, std::make_pair(status, snapshot));
                        });
                },
                kWaitTimeoutMs,
                kIOReturnTimeout,
                nullptr,
                kWaitPollMs);

            const auto [snapshotStatus, snapshot] = (snapshotRes.status == kIOReturnTimeout)
                ? std::make_pair(IRM::AllocationStatus::Timeout, IRM::ResourceSnapshot{})
                : snapshotRes.value;

            if (snapshotStatus != IRM::AllocationStatus::Success) {
                return {.status = MapStatus(snapshotStatus)};
            }
            if (snapshot.bandwidthAvailable < bandwidthUnits) {
                return {.status = kIOReturnNoResources};
            }

            const uint8_t channel = FirstAvailableChannel(snapshot, candidates);
            if (channel == AudioStreamWireInfo::kInvalidIsoChannel) {
                return {.status = kIOReturnNoResources};
            }

            // A competing initiator can consume the selected channel between
            // the snapshot and CAS. Exclude that candidate and continue only
            // for a definite no-resource result; timeouts and generation
            // changes have indeterminate/different ownership semantics.
            candidates &= ~(uint64_t{1} << channel);
            const kern_return_t status = ReserveSpecific(client, channel, bandwidthUnits);
            if (status == kIOReturnSuccess) {
                return {.status = status, .channel = channel};
            }
            if (status != kIOReturnNoResources) {
                return {.status = status};
            }
        }

        return {.status = kIOReturnNoResources};
    }

    void ReleaseAll() noexcept {
        while (count_ > 0) {
            Entry& entry = entries_[--count_];
            if (entry.client == nullptr) {
                entry = {};
                continue;
            }

            (void)WaitForAsyncResult<IRM::AllocationStatus>(
                [&entry](auto callback) {
                    entry.client->ReleaseResources(
                        entry.channel, entry.bandwidthUnits,
                        [cb = std::move(callback)](IRM::AllocationStatus status) mutable {
                            cb(kIOReturnSuccess, status);
                        });
                },
                kWaitTimeoutMs,
                kIOReturnTimeout,
                nullptr,
                kWaitPollMs);
            entry = {};
        }
    }

    // An IRM allocation belongs to the bus generation in which it was made.
    // Once that generation is invalid (including provider removal), a release
    // transaction is both unnecessary and unable to complete. Apple
    // IOFWIsochChannel.cpp:1243-1269 likewise clears local allocation state
    // after a bus reset instead of releasing resources on the old generation.
    void InvalidateAfterGenerationChange() noexcept {
        while (count_ > 0) {
            entries_[--count_] = {};
        }
    }

    [[nodiscard]] size_t Count() const noexcept { return count_; }

  private:
    [[nodiscard]] kern_return_t ReserveSpecific(IRM::IRMClient& client, uint8_t channel,
                                                uint32_t bandwidthUnits) noexcept {
        if (count_ >= entries_.size() || channel > 63) {
            return kIOReturnNoResources;
        }

        // Linux cmp.c:188-209 and iso-resources.c:91-147 reserve channel and
        // bandwidth as one lifecycle-owned resource before establishing a PCR.
        const auto res = WaitForAsyncResult<IRM::AllocationStatus>(
            [&client, channel, bandwidthUnits](auto callback) {
                client.AllocateResources(
                    channel, bandwidthUnits,
                    [cb = std::move(callback)](IRM::AllocationStatus status) mutable {
                        cb(kIOReturnSuccess, status);
                    });
            },
            kWaitTimeoutMs,
            kIOReturnTimeout,
            nullptr,
            kWaitPollMs);

        const IRM::AllocationStatus status = (res.status == kIOReturnTimeout)
            ? IRM::AllocationStatus::Timeout
            : res.value;
        if (status != IRM::AllocationStatus::Success) {
            return MapStatus(status);
        }

        entries_[count_++] = Entry{
            .client = &client,
            .channel = channel,
            .bandwidthUnits = bandwidthUnits,
        };
        return kIOReturnSuccess;
    }
    static constexpr uint32_t kWaitTimeoutMs = 12000;
    static constexpr uint32_t kWaitPollMs = 5;

    struct Entry {
        IRM::IRMClient* client{nullptr};
        uint8_t channel{0};
        uint32_t bandwidthUnits{0};
    };

    [[nodiscard]] static bool IsAvailable(const IRM::ResourceSnapshot& snapshot,
                                          uint8_t channel) noexcept {
        const uint32_t registerValue = channel < 32 ? snapshot.channelsAvailable31_0
                                                    : snapshot.channelsAvailable63_32;
        const uint32_t bit = uint32_t{1} << (31U - (channel % 32U));
        return (registerValue & bit) != 0;
    }

    [[nodiscard]] static uint8_t FirstAvailableChannel(const IRM::ResourceSnapshot& snapshot,
                                                       uint64_t candidates) noexcept {
        for (uint8_t channel = 0; channel < 64; ++channel) {
            if ((candidates & (uint64_t{1} << channel)) != 0 &&
                IsAvailable(snapshot, channel)) {
                return channel;
            }
        }
        return AudioStreamWireInfo::kInvalidIsoChannel;
    }

    [[nodiscard]] static kern_return_t MapStatus(IRM::AllocationStatus status) noexcept {
        switch (status) {
            case IRM::AllocationStatus::Success:
                return kIOReturnSuccess;
            case IRM::AllocationStatus::NoResources:
                return kIOReturnNoResources;
            case IRM::AllocationStatus::GenerationMismatch:
                return kIOReturnOffline;
            case IRM::AllocationStatus::Timeout:
                return kIOReturnTimeout;
            case IRM::AllocationStatus::NotFound:
                return kIOReturnNoDevice;
            case IRM::AllocationStatus::Failed:
                return kIOReturnError;
        }
        return kIOReturnError;
    }

    // One instance owns one direction, so the bound is independently enforced
    // for playback and capture by IsochDuplexHostTransport's two instances.
    std::array<Entry, kMaxAudioStreamsPerDirection> entries_{};
    size_t count_{0};
};

class DuplexIRMReservationPair final {
  public:
    [[nodiscard]] kern_return_t ReservePlayback(IRM::IRMClient& client, uint8_t channel,
                                                uint32_t bandwidthUnits) noexcept {
        const kern_return_t status = playback_.Reserve(client, channel, bandwidthUnits);
        if (status != kIOReturnSuccess) {
            ReleaseAll();
        }
        return status;
    }

    [[nodiscard]] kern_return_t ReserveCapture(IRM::IRMClient& client, uint8_t channel,
                                               uint32_t bandwidthUnits) noexcept {
        const kern_return_t status = capture_.Reserve(client, channel, bandwidthUnits);
        if (status != kIOReturnSuccess) {
            ReleaseAll();
        }
        return status;
    }

    [[nodiscard]] IRMReservationResult ReserveAnyPlayback(IRM::IRMClient& client,
                                                          uint64_t allowedChannels,
                                                          uint32_t bandwidthUnits) noexcept {
        const IRMReservationResult result =
            playback_.ReserveAny(client, allowedChannels, bandwidthUnits);
        if (result.status != kIOReturnSuccess) {
            ReleaseAll();
        }
        return result;
    }

    [[nodiscard]] IRMReservationResult ReserveAnyCapture(IRM::IRMClient& client,
                                                         uint64_t allowedChannels,
                                                         uint32_t bandwidthUnits) noexcept {
        const IRMReservationResult result =
            capture_.ReserveAny(client, allowedChannels, bandwidthUnits);
        if (result.status != kIOReturnSuccess) {
            ReleaseAll();
        }
        return result;
    }

    void ReleaseAll() noexcept {
        capture_.ReleaseAll();
        playback_.ReleaseAll();
    }

    void InvalidateAfterGenerationChange() noexcept {
        capture_.InvalidateAfterGenerationChange();
        playback_.InvalidateAfterGenerationChange();
    }

    [[nodiscard]] size_t PlaybackCount() const noexcept { return playback_.Count(); }
    [[nodiscard]] size_t CaptureCount() const noexcept { return capture_.Count(); }

  private:
    DuplexIRMReservations playback_{};
    DuplexIRMReservations capture_{};
};

} // namespace ASFW::Audio::Backends
