// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

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
    return true;
}

void DICETcatProtocol::PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady);
        return;
    }
    duplexCtrl_->PrepareDuplex48k(channels, std::move(callback));
}

void DICETcatProtocol::ProgramRxForDuplex48k(VoidCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady);
        return;
    }
    duplexCtrl_->ProgramRxForDuplex48k(std::move(callback));
}

void DICETcatProtocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady);
        return;
    }
    duplexCtrl_->ProgramTxAndEnableDuplex48k(std::move(callback));
}

void DICETcatProtocol::ConfirmDuplex48kStart(VoidCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady);
        return;
    }

    duplexCtrl_->ConfirmDuplex48kStart([this, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }

        EnsureRuntimeCapsLoaded(std::move(callback));
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
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
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
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                callback(kIOReturnSuccess);
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    if (tx.numStreams > 0) {
        const auto& tx0 = tx.streams[0];
        hostInputPcmChannels_.store(tx0.pcmChannels, std::memory_order_relaxed);
        deviceToHostAm824Slots_.store(tx0.Am824Slots(), std::memory_order_relaxed);
    } else {
        hostInputPcmChannels_.store(0, std::memory_order_relaxed);
        deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    }

    if (rx.numStreams > 0) {
        const auto& rx0 = rx.streams[0];
        hostOutputPcmChannels_.store(rx0.pcmChannels, std::memory_order_relaxed);
        hostToDeviceAm824Slots_.store(rx0.Am824Slots(), std::memory_order_relaxed);
    } else {
        hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
        hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    }

    runtimeSampleRateHz_.store(global.sampleRate, std::memory_order_relaxed);
    runtimeCapsValid_.store(true, std::memory_order_release);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
}

} // namespace ASFW::Audio::DICE::TCAT
