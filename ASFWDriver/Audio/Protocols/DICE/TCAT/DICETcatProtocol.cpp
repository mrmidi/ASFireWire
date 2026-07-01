// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

namespace {

[[nodiscard]] bool HasUsableRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    return caps.sampleRateHz != 0 &&
        caps.hostInputPcmChannels != 0 &&
        caps.hostOutputPcmChannels != 0;
}

void LogRuntimeCaps(const char* source, const AudioStreamRuntimeCaps& caps) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: runtime caps source=%{public}s rate=%u in=%u out=%u d2hSlots=%u h2dSlots=%u usable=%u",
             source,
             caps.sampleRateHz,
             caps.hostInputPcmChannels,
             caps.hostOutputPcmChannels,
             caps.deviceToHostAm824Slots,
             caps.hostToDeviceAm824Slots,
             HasUsableRuntimeCaps(caps) ? 1U : 0U);
}

void LogStreamConfigSummary(const char* label, const StreamConfig& config) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: %{public}s stream summary count=%u pcm=%u midi=%u am824=%u entrySize=%u parsedEntrySize=%u",
             label,
             config.numStreams,
             config.TotalPcmChannels(),
             config.TotalMidiPorts(),
             config.TotalAm824Slots(),
             config.entrySizeBytes,
             config.parsedEntrySizeBytes);
}

} // namespace

DICETcatProtocol::DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   uint16_t nodeId,
                                   ::ASFW::IRM::IRMClient* irmClient)
    : busInfo_(busInfo)
    , irmClient_(irmClient)
    , io_(busOps, busInfo, nodeId)
    , diceReader_(io_) {
}

IOReturn DICETcatProtocol::Initialize() {
    if (!duplexCtrl_) {
        duplexCtrl_.emplace(diceReader_, io_, busInfo_, nullptr /*workQueue*/, GeneralSections{});
        duplexCtrl_->SetTeardownCancelToken(teardownCancel_);
    }

    initialized_ = true;
    ASFW_LOG(DICE, "DICETcatProtocol::Initialize defers generic discovery until runtime");
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::Shutdown() {
    if (duplexCtrl_) {
        if (duplexCtrl_->IsPrepared() || duplexCtrl_->IsRunning()) {
            const IOReturn stopStatus = duplexCtrl_->StopDuplex();
            if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown duplex stop failed: 0x%x", stopStatus);
            }
        }

        duplexCtrl_->ReleaseOwner([](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown ReleaseOwner failed: 0x%x", status);
            }
        });
    }

    sections_ = {};
    sectionsLoaded_ = false;
    initialized_ = false;
    ResetRuntimeCaps();
    return kIOReturnSuccess;
}

bool DICETcatProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    outCaps.deviceToHostIsoChannel =
        static_cast<uint8_t>(deviceToHostIsoChannel_.load(std::memory_order_relaxed));
    outCaps.hostToDeviceIsoChannel =
        static_cast<uint8_t>(hostToDeviceIsoChannel_.load(std::memory_order_relaxed));

    // Per-stream geometry: the runtimeCapsValid_ acquire-load above establishes
    // happens-before with the writer's release-store, so the plain arrays are
    // safe to read here.
    outCaps.deviceToHostStreamCount = deviceToHostStreamCount_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceStreamCount = hostToDeviceStreamCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        outCaps.deviceToHostStreams[i] = deviceToHostStreams_[i];
        outCaps.hostToDeviceStreams[i] = hostToDeviceStreams_[i];
    }
    return true;
}

void DICETcatProtocol::EnsureRuntimeStreamGeometry(VoidCallback callback) {
    EnsureRuntimeCapsLoaded(std::move(callback));
}

void DICETcatProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
    if (duplexCtrl_) {
        duplexCtrl_->SetTeardownCancelToken(cancel);
    }
}

void DICETcatProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                     const DiceDesiredClockConfig& desiredClock,
                                     PrepareCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->PrepareDuplex(
        channels,
        desiredClock,
        [this, callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ProgramRx(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramRx(std::move(callback));
}

void DICETcatProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramTxAndEnableDuplex(std::move(callback));
}

void DICETcatProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ConfirmDuplexStart(
        [this, callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
                                        ClockApplyCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ApplyClockConfig(
        desiredClock,
        [this, callback = std::move(callback)](IOReturn status, DiceClockApplyResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ReadDuplexHealth(HealthCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus, {});
            return;
        }

        diceReader_.ReadGlobalState(
            sections_,
            [this, callback = std::move(callback)](IOReturn status, GlobalState global) mutable {
                if (status != kIOReturnSuccess) {
                    callback(status, {});
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                (void)GetRuntimeAudioStreamCaps(caps);

                callback(status,
                         DiceDuplexHealthResult{
                             .generation = busInfo_.GetGeneration(),
                             .appliedClock =
                                 DiceDesiredClockConfig{
                                     .sampleRateHz = global.sampleRate,
                                     .clockSelect = global.clockSelect,
                                 },
                             .runtimeCaps = caps,
                             .notification = global.notification,
                             .status = global.status,
                             .extStatus = global.extStatus,
                         });
            });
    });
}

void DICETcatProtocol::PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
    PrepareDuplex(channels,
                  DiceDesiredClockConfig{
                      .sampleRateHz = 48000U,
                      .clockSelect = kDiceClockSelect48kInternal,
                  },
                  [callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult) mutable {
                      callback(status);
                  });
}

void DICETcatProtocol::ProgramRxForDuplex48k(VoidCallback callback) {
    ProgramRx([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    ProgramTxAndEnableDuplex([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ConfirmDuplex48kStart(VoidCallback callback) {
    ConfirmDuplexStart([callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult) mutable {
        callback(status);
    });
}

IOReturn DICETcatProtocol::StopDuplex() {
    if (!duplexCtrl_) {
        return kIOReturnSuccess;
    }
    return duplexCtrl_->StopDuplex();
}

void DICETcatProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                            Protocols::AVC::FCPTransport* transport) {
    (void)transport;
    io_.SetNodeId(nodeId);
}

void DICETcatProtocol::EnsureSectionsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (sectionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    diceReader_.ReadGeneralSections([this, callback = std::move(callback)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DICETcatProtocol: failed to read general sections: 0x%x", status);
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
        ASFW_LOG(DICE,
                 "DICETcatProtocol: loaded sections global=%u/%u tx=%u/%u rx=%u/%u ext=%u/%u",
                 sections_.global.offset,
                 sections_.global.size,
                 sections_.txStreamFormat.offset,
                 sections_.txStreamFormat.size,
                 sections_.rxStreamFormat.offset,
                 sections_.rxStreamFormat.size,
                 sections_.extSync.offset,
                 sections_.extSync.size);
        callback(kIOReturnSuccess);
    });
}

void DICETcatProtocol::EnsureRuntimeCapsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (runtimeCapsValid_.load(std::memory_order_acquire)) {
        callback(kIOReturnSuccess);
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus);
            return;
        }

        struct RuntimeCapsState {
            GlobalState global;
            StreamConfig tx;
            StreamConfig rx;
        };

        auto state = std::make_shared<RuntimeCapsState>();
        diceReader_.ReadGlobalState(
            sections_,
            [this, state, callback = std::move(callback)](IOReturn globalStatus, GlobalState global) mutable {
                if (globalStatus != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read global state: 0x%x", globalStatus);
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                ASFW_LOG(DICE,
                         "DICETcatProtocol: global state rate=%u clockSelect=0x%08x status=0x%08x extStatus=0x%08x notification=0x%08x",
                         global.sampleRate,
                         global.clockSelect,
                         global.status,
                         global.extStatus,
                         global.notification);
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            ASFW_LOG(DICE, "DICETcatProtocol: failed to read TX stream config: 0x%x", txStatus);
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        LogStreamConfigSummary("TX", state->tx);
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read RX stream config: 0x%x", rxStatus);
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                LogStreamConfigSummary("RX", state->rx);
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                AudioStreamRuntimeCaps caps{};
                                (void)GetRuntimeAudioStreamCaps(caps);
                                LogRuntimeCaps("standard-dice", caps);
                                if (!HasUsableRuntimeCaps(caps)) {
                                    ASFW_LOG(DICE,
                                             "DICETcatProtocol: standard DICE discovery produced zero or partial caps; audio publication should fail closed");
                                }
                                callback(kIOReturnSuccess);
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = tx.TotalPcmChannels(),
        .hostOutputPcmChannels = rx.TotalPcmChannels(),
        .deviceToHostAm824Slots = tx.TotalAm824Slots(),
        .hostToDeviceAm824Slots = rx.TotalAm824Slots(),
        .sampleRateHz = global.sampleRate,
        .deviceToHostIsoChannel = tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
        .hostToDeviceIsoChannel = rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
    };

    // Per-stream wire geometry from the DICE TX_NUMBER/RX_NUMBER headers. Stream
    // count includes streams the device reports with iso=-1 (disabled) that the
    // host must still arm for a multi-stream device such as the Venice F32
    // (2×16). Mirrors DICEDuplexBringupController's per-stream fill.
    auto fillPerStream = [](const StreamConfig& sc,
                            uint32_t& outCount,
                            AudioStreamWireInfo* outStreams) noexcept {
        const uint32_t count = (sc.numStreams < kMaxAudioStreamsPerDirection)
                                   ? sc.numStreams
                                   : kMaxAudioStreamsPerDirection;
        outCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = sc.streams[i];
            outStreams[i].isoChannel =
                (entry.isoChannel >= 0 && entry.isoChannel <= 0x3F)
                    ? static_cast<uint8_t>(entry.isoChannel)
                    : AudioStreamWireInfo::kInvalidIsoChannel;
            outStreams[i].pcmChannels = static_cast<uint16_t>(entry.pcmChannels);
            outStreams[i].am824Slots = static_cast<uint16_t>(entry.Am824Slots());
            outStreams[i].midiPorts = static_cast<uint16_t>(entry.midiPorts);
        }
    };
    fillPerStream(tx, caps.deviceToHostStreamCount, caps.deviceToHostStreams);
    fillPerStream(rx, caps.hostToDeviceStreamCount, caps.hostToDeviceStreams);

    CacheRuntimeCaps(caps);
}

void DICETcatProtocol::CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    hostInputPcmChannels_.store(caps.hostInputPcmChannels, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(caps.deviceToHostAm824Slots, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(caps.hostOutputPcmChannels, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(caps.hostToDeviceAm824Slots, std::memory_order_relaxed);
    runtimeSampleRateHz_.store(caps.sampleRateHz, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(caps.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(caps.hostToDeviceIsoChannel, std::memory_order_relaxed);

    // Per-stream geometry: write the plain arrays + counts BEFORE the
    // release-store of runtimeCapsValid_ so readers that pass the acquire-load
    // observe a consistent snapshot.
    deviceToHostStreamCount_.store(caps.deviceToHostStreamCount, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(caps.hostToDeviceStreamCount, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = caps.deviceToHostStreams[i];
        hostToDeviceStreams_[i] = caps.hostToDeviceStreams[i];
    }

    runtimeCapsValid_.store(true, std::memory_order_release);
    LogRuntimeCaps("cache", caps);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    deviceToHostStreamCount_.store(0, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = AudioStreamWireInfo{};
        hostToDeviceStreams_[i] = AudioStreamWireInfo{};
    }
}

} // namespace ASFW::Audio::DICE::TCAT
