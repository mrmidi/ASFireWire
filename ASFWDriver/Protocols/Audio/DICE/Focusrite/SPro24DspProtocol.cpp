// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspProtocol.cpp - Focusrite Saffire Pro 24 DSP protocol implementation

#include "SPro24DspProtocol.hpp"
#include "SPro24DspRouting.hpp"
#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <array>
#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// Wire Format Helpers
// ============================================================================

namespace {
} // anonymous namespace

// ============================================================================
// SPro24DspProtocol Implementation
// ============================================================================

SPro24DspProtocol::SPro24DspProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                     Protocols::Ports::FireWireBusInfo& busInfo,
                                     uint16_t nodeId,
                                     IRM::IRMClient* irmClient)
    : tcat_(busOps, busInfo, nodeId, irmClient)
{
    ASFW_LOG(DICE, "SPro24DspProtocol created for node 0x%04x", nodeId);
}

bool SPro24DspProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    return tcat_.GetRuntimeAudioStreamCaps(outCaps);
}

IOReturn SPro24DspProtocol::Initialize() {
    InitializeAsync([](IOReturn status) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "SPro24DspProtocol initialization failed: 0x%x", status);
        }
    });
    return kIOReturnSuccess;
}

void SPro24DspProtocol::InitializeAsync(InitCallback callback) {
    ASFW_LOG(DICE, "SPro24DspProtocol::InitializeAsync defers generic DICE discovery to TCAT runtime");
    callback(tcat_.Initialize());
}

void SPro24DspProtocol::HandleExtensionSectionsRead(IOReturn status,
                                                    ExtensionSections sections,
                                                    InitCallback callback) {
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "Failed to read Focusrite extension sections: 0x%x", status);
        callback(status);
        return;
    }

    extensionSections_ = sections;
    appSectionBase_ = ASFW::Audio::DICE::ExtensionAbsoluteOffset(extensionSections_.application);
    commandSectionBase_ = ASFW::Audio::DICE::ExtensionAbsoluteOffset(extensionSections_.command);
    routerSectionBase_ = ASFW::Audio::DICE::ExtensionAbsoluteOffset(extensionSections_.router);
    currentConfigBase_ = ASFW::Audio::DICE::ExtensionAbsoluteOffset(extensionSections_.currentConfig);

    if (appSectionBase_ == kDICEExtensionOffset ||
        commandSectionBase_ == kDICEExtensionOffset ||
        routerSectionBase_ == kDICEExtensionOffset ||
        currentConfigBase_ == kDICEExtensionOffset) {
        ASFW_LOG(DICE, "SPro24DspProtocol: missing required TCAT extension section(s)");
        callback(kIOReturnNotFound);
        return;
    }

    extensionsLoaded_ = true;
    ASFW_LOG(DICE,
             "SPro24DspProtocol: loaded Focusrite extension bases app=0x%08x cmd=0x%08x router=0x%08x current=0x%08x",
             appSectionBase_,
             commandSectionBase_,
             routerSectionBase_,
             currentConfigBase_);
    callback(kIOReturnSuccess);
}

void SPro24DspProtocol::EnsureExtensionsLoaded(VoidCallback callback) {
    if (extensionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    tcat_.Transaction().ReadExtensionSections(
        [this, callback = std::move(callback)](IOReturn status, ExtensionSections sections) mutable {
            HandleExtensionSectionsRead(status, sections, std::move(callback));
        });
}

IOReturn SPro24DspProtocol::Shutdown() {
    ASFW_LOG(DICE, "SPro24DspProtocol::Shutdown");
    extensionSections_ = {};
    appSectionBase_ = 0;
    commandSectionBase_ = 0;
    routerSectionBase_ = 0;
    currentConfigBase_ = 0;
    extensionsLoaded_ = false;
    return tcat_.Shutdown();
}

void SPro24DspProtocol::PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
    tcat_.PrepareDuplex48k(channels, std::move(callback));
}

void SPro24DspProtocol::ProgramRxForDuplex48k(VoidCallback callback) {
    tcat_.ProgramRxForDuplex48k(std::move(callback));
}

void SPro24DspProtocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    tcat_.ProgramTxAndEnableDuplex48k(std::move(callback));
}

void SPro24DspProtocol::ConfirmDuplex48kStart(VoidCallback callback) {
    tcat_.ConfirmDuplex48kStart(std::move(callback));
}

IOReturn SPro24DspProtocol::StopDuplex() {
    return tcat_.StopDuplex();
}

void SPro24DspProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                             Protocols::AVC::FCPTransport* transport) {
    tcat_.UpdateRuntimeContext(nodeId, transport);
}

void SPro24DspProtocol::ReadAppQuad(uint32_t offset,
                                    std::function<void(IOReturn, uint32_t)> callback) {
    EnsureExtensionsLoaded([this, offset, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            callback(status, 0);
            return;
        }

        tcat_.IO().ReadQuadBE(MakeDICEAddress(appSectionBase_ + offset),
                              [callback = std::move(callback)](Async::AsyncStatus transportStatus, uint32_t value) mutable {
                                  callback(Protocols::Ports::MapAsyncStatusToIOReturn(transportStatus), value);
                              });
    });
}

void SPro24DspProtocol::WriteAppQuad(uint32_t offset, uint32_t value, VoidCallback callback) {
    EnsureExtensionsLoaded([this, offset, value, callback = std::move(callback)](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }

        tcat_.IO().WriteQuadBE(MakeDICEAddress(appSectionBase_ + offset),
                               value,
                               [callback = std::move(callback)](Async::AsyncStatus transportStatus) mutable {
                                   callback(Protocols::Ports::MapAsyncStatusToIOReturn(transportStatus));
                               });
    });
}

void SPro24DspProtocol::ReadAppSection(uint32_t offset, size_t size, DICEReadCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    EnsureExtensionsLoaded([this, offset, size, callbackState](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            Common::InvokeSharedCallback(callbackState, status, nullptr, size_t{0});
            return;
        }

        tcat_.IO().ReadBlock(MakeDICEAddress(appSectionBase_ + offset),
                             static_cast<uint32_t>(size),
                             [callbackState](Async::AsyncStatus transportStatus, std::span<const uint8_t> payload) {
                                 const bool hasPayload = transportStatus == Async::AsyncStatus::kSuccess ||
                                                         transportStatus == Async::AsyncStatus::kShortRead;
                                 Common::InvokeSharedCallback(callbackState,
                                                             Protocols::Ports::MapAsyncStatusToIOReturn(transportStatus),
                                                             hasPayload ? payload.data() : nullptr,
                                                             hasPayload ? payload.size() : size_t{0});
                             });
    });
}

void SPro24DspProtocol::WriteAppSection(uint32_t offset,
                                        const uint8_t* data,
                                        size_t size,
                                        DICEWriteCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    EnsureExtensionsLoaded([this, offset, data, size, callbackState](IOReturn status) mutable {
        if (status != kIOReturnSuccess) {
            Common::InvokeSharedCallback(callbackState, status);
            return;
        }

        tcat_.IO().WriteBlock(MakeDICEAddress(appSectionBase_ + offset),
                              std::span<const uint8_t>(data, size),
                              [callbackState](Async::AsyncStatus transportStatus) {
                                  Common::InvokeSharedCallback(callbackState,
                                                              Protocols::Ports::MapAsyncStatusToIOReturn(transportStatus));
                              });
    });
}

void SPro24DspProtocol::SendSwNotice(SwNotice notice, VoidCallback callback) {
    WriteAppQuad(kSwNoticeOffset, static_cast<uint32_t>(notice), std::move(callback));
}

void SPro24DspProtocol::EnableDsp(bool enable, VoidCallback callback) {
    uint32_t value = enable ? 1 : 0;
    WriteAppQuad(kDspEnableOffset, value, [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::DspChanged, callback);
    });
}

void SPro24DspProtocol::GetEffectParams(ResultCallback<EffectGeneralParams> callback) {
    ReadAppQuad(kEffectGeneralOffset, [callback](IOReturn status, uint32_t value) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        uint8_t data[4];
        ASFW::FW::WriteBE32(data, value);
        callback(kIOReturnSuccess, EffectGeneralParams::Deserialize(data));
    });
}

void SPro24DspProtocol::SetEffectParams(const EffectGeneralParams& params, VoidCallback callback) {
    uint8_t data[4];
    params.Serialize(data);
    const uint32_t value = ASFW::FW::ReadBE32(data);
    
    WriteAppQuad(kEffectGeneralOffset, value, [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::EffectChanged, callback);
    });
}

void SPro24DspProtocol::GetCompressorState(ResultCallback<CompressorState> callback) {
    ReadAppSection(kCoefOffset + CoefBlock::kCompressor * kCoefBlockSize, 2 * kCoefBlockSize,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, CompressorState::Deserialize(data));
    });
}

void SPro24DspProtocol::SetCompressorState(const CompressorState& state, VoidCallback callback) {
    // Note: Need to allocate buffer that outlives async call
    auto buffer = std::make_shared<std::array<uint8_t, 2 * kCoefBlockSize>>();
    state.Serialize(buffer->data());

    WriteAppSection(kCoefOffset + CoefBlock::kCompressor * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        // Per Linux reference (spro24dsp.rs lines 770-771): send BOTH
        // CompCh0 and CompCh1 SW notices after compressor state write.
        SendSwNotice(SwNotice::CompCh0, [this, callback](IOReturn s1) {
            if (s1 != kIOReturnSuccess) {
                callback(s1);
                return;
            }
            SendSwNotice(SwNotice::CompCh1, callback);
        });
    });
}

void SPro24DspProtocol::GetReverbState(ResultCallback<ReverbState> callback) {
    ReadAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, kCoefBlockSize,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, ReverbState::Deserialize(data));
    });
}

void SPro24DspProtocol::SetReverbState(const ReverbState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, kCoefBlockSize>>();
    state.Serialize(buffer->data());

    WriteAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        // Per Linux reference: REVERB_SW_NOTICE = 0x1A
        SendSwNotice(SwNotice::Reverb, callback);
    });
}

void SPro24DspProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    ReadAppSection(kInputOffset, 8,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, InputParams::Deserialize(data));
    });
}

void SPro24DspProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, 8>>();
    params.Serialize(buffer->data());

    WriteAppSection(kInputOffset, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::InputChanged, callback);
    });
}

void SPro24DspProtocol::GetOutputGroupState(ResultCallback<OutputGroupState> callback) {
    ReadAppSection(kOutputGroupOffset, kOutputGroupStateSize,
                   [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        if (size < kOutputGroupStateSize) {
            callback(kIOReturnUnderrun, {});
            return;
        }
        callback(kIOReturnSuccess, OutputGroupState::Deserialize(data));
    });
}

void SPro24DspProtocol::SetOutputGroupState(const OutputGroupState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, kOutputGroupStateSize>>();
    state.Serialize(buffer->data());

    WriteAppSection(kOutputGroupOffset, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::DimMute, [this, callback](IOReturn dimStatus) {
            if (dimStatus != kIOReturnSuccess) {
                callback(dimStatus);
                return;
            }
            SendSwNotice(SwNotice::OutputSrc, callback);
        });
    });
}

// ============================================================================
// TODO: Test only - Stream Control
// ============================================================================

void SPro24DspProtocol::StartStreamTest(VoidCallback callback) {
    const AudioDuplexChannels channels{};
    PrepareDuplex48k(channels, std::move(callback));
}

} // namespace ASFW::Audio::DICE::Focusrite
