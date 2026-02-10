// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspProtocol.cpp - Focusrite Saffire Pro 24 DSP protocol implementation

#include "SPro24DspProtocol.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Async/AsyncSubsystem.hpp"

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// Wire Format Helpers
// ============================================================================

namespace {

float FloatFromWire(const uint8_t* data) {
    uint32_t bits = DICETransaction::QuadletFromWire(data);
    float f;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&f, &bits, sizeof(float));
    return f;
}

void FloatToWire(float value, uint8_t* data) {
    uint32_t bits;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&bits, &value, sizeof(float));
    DICETransaction::QuadletToWire(bits, data);
}

} // anonymous namespace

// ============================================================================
// CompressorState
// ============================================================================

CompressorState CompressorState::FromWire(const uint8_t* data) {
    CompressorState s;
    
    for (size_t ch = 0; ch < 2; ++ch) {
        const uint8_t* block = data + ch * kCoefBlockSize;
        s.output[ch]    = FloatFromWire(block + 0x00);
        s.threshold[ch] = FloatFromWire(block + 0x04);
        s.ratio[ch]     = FloatFromWire(block + 0x08);
        s.attack[ch]    = FloatFromWire(block + 0x0C);
        s.release[ch]   = FloatFromWire(block + 0x10);
    }
    
    return s;
}

void CompressorState::ToWire(uint8_t* data) const {
    for (size_t ch = 0; ch < 2; ++ch) {
        uint8_t* block = data + ch * kCoefBlockSize;
        FloatToWire(output[ch],    block + 0x00);
        FloatToWire(threshold[ch], block + 0x04);
        FloatToWire(ratio[ch],     block + 0x08);
        FloatToWire(attack[ch],    block + 0x0C);
        FloatToWire(release[ch],   block + 0x10);
    }
}

// ============================================================================
// ReverbState
// ============================================================================

ReverbState ReverbState::FromWire(const uint8_t* data) {
    ReverbState s;
    s.size = FloatFromWire(data + 0x70);
    s.air = FloatFromWire(data + 0x74);
    
    float on = FloatFromWire(data + 0x78);
    s.enabled = on > 0.5f;
    
    float mag = FloatFromWire(data + 0x80);
    float sign = FloatFromWire(data + 0x84);
    s.preFilter = (sign >= 0.5f) ? mag : -mag;
    
    return s;
}

void ReverbState::ToWire(uint8_t* data) const {
    FloatToWire(size, data + 0x70);
    FloatToWire(air, data + 0x74);
    FloatToWire(enabled ? 1.0f : 0.0f, data + 0x78);
    FloatToWire(enabled ? 0.0f : 1.0f, data + 0x7C);
    FloatToWire((preFilter < 0.0f) ? -preFilter : preFilter, data + 0x80);
    FloatToWire((preFilter >= 0.0f) ? 1.0f : 0.0f, data + 0x84);
}

// ============================================================================
// EffectGeneralParams
// ============================================================================

EffectGeneralParams EffectGeneralParams::FromWire(const uint8_t* data) {
    EffectGeneralParams p;
    uint32_t flags = DICETransaction::QuadletFromWire(data);
    
    p.eqAfterComp[0] = (flags & 0x01) != 0;
    p.eqAfterComp[1] = (flags & 0x02) != 0;
    p.compEnable[0]  = (flags & 0x04) != 0;
    p.compEnable[1]  = (flags & 0x08) != 0;
    p.eqEnable[0]    = (flags & 0x10) != 0;
    p.eqEnable[1]    = (flags & 0x20) != 0;
    
    return p;
}

void EffectGeneralParams::ToWire(uint8_t* data) const {
    uint32_t flags = 0;
    if (eqAfterComp[0]) flags |= 0x01;
    if (eqAfterComp[1]) flags |= 0x02;
    if (compEnable[0])  flags |= 0x04;
    if (compEnable[1])  flags |= 0x08;
    if (eqEnable[0])    flags |= 0x10;
    if (eqEnable[1])    flags |= 0x20;
    DICETransaction::QuadletToWire(flags, data);
}

// ============================================================================
// SPro24DspProtocol Implementation
// ============================================================================

SPro24DspProtocol::SPro24DspProtocol(Async::AsyncSubsystem& subsystem, uint16_t nodeId)
    : subsystem_(subsystem)
    , tx_(nodeId)
{
    ASFW_LOG(DICE, "SPro24DspProtocol created for node 0x%04x", nodeId);
}

IOReturn SPro24DspProtocol::Initialize() {
    // Start async initialization - this will trigger capability discovery
    InitializeAsync([](IOReturn status) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "SPro24DspProtocol async initialization failed: 0x%x", status);
        }
    });
    return kIOReturnSuccess;
}

void SPro24DspProtocol::InitializeAsync(InitCallback callback) {
    ASFW_LOG(DICE, "SPro24DspProtocol::InitializeAsync starting capability discovery");
    
    // Use ReadCapabilities for full discovery (global + TX + RX streams)
    tx_.ReadCapabilities(subsystem_, [this, callback](IOReturn status, DICECapabilities caps) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "Failed to read DICE capabilities: 0x%x", status);
            callback(status);
            return;
        }
        
        // Store sections for later use
        tx_.ReadGeneralSections(subsystem_, [this, callback, caps](IOReturn status, GeneralSections sections) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "Failed to read general sections: 0x%x", status);
                callback(status);
                return;
            }
            
            sections_ = sections;
            
            // Application section is at TX section offset (per TCAT DICE spec)
            appSectionBase_ = sections_.txStreamFormat.offset * 4;
            initialized_ = true;
            
            ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
            ASFW_LOG(DICE, "SPro24DspProtocol Initialized Successfully");
            ASFW_LOG(DICE, "  Current Rate: %u Hz", caps.global.sampleRate);
            ASFW_LOG(DICE, "  TX Channels:  %u (streams=%u)", caps.txStreams.TotalChannels(), caps.txStreams.numStreams);
            ASFW_LOG(DICE, "  RX Channels:  %u (streams=%u)", caps.rxStreams.TotalChannels(), caps.rxStreams.numStreams);
            ASFW_LOG(DICE, "  Nickname:     '%{public}s'", caps.global.nickname);
            ASFW_LOG(DICE, "  App Section:  0x%08x", appSectionBase_);
            ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");

            // Keep protocol initialization side-effect free.
            // Stream start is orchestrated by ASFWAudioNub/ASFWAudioDriver bring-up.
            ASFW_LOG(DICE, "SPro24DspProtocol: Skipping StartStreamTest (managed by audio path)");
            
            callback(kIOReturnSuccess);
        });
    });
}

IOReturn SPro24DspProtocol::Shutdown() {
    ASFW_LOG(DICE, "SPro24DspProtocol::Shutdown");
    initialized_ = false;
    return kIOReturnSuccess;
}

IOReturn SPro24DspProtocol::StartDuplex48k() {
    if (!initialized_) {
        ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k rejected (not initialized)");
        return kIOReturnNotReady;
    }

    StartStreamTest([](IOReturn status) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k failed: 0x%x", status);
        } else {
            ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k configured");
        }
    });
    return kIOReturnSuccess;
}

void SPro24DspProtocol::ReadAppSection(uint32_t offset, size_t size, DICEReadCallback callback) {
    tx_.ReadBlock(subsystem_, appSectionBase_ + offset, size, callback);
}

void SPro24DspProtocol::WriteAppSection(uint32_t offset, const uint8_t* data, size_t size, DICEWriteCallback callback) {
    tx_.WriteBlock(subsystem_, appSectionBase_ + offset, data, size, callback);
}

void SPro24DspProtocol::SendSwNotice(SwNotice notice, VoidCallback callback) {
    tx_.WriteQuadlet(subsystem_, appSectionBase_ + kSwNoticeOffset, static_cast<uint32_t>(notice), callback);
}

void SPro24DspProtocol::EnableDsp(bool enable, VoidCallback callback) {
    uint32_t value = enable ? 1 : 0;
    tx_.WriteQuadlet(subsystem_, appSectionBase_ + kDspEnableOffset, value, 
                     [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::DspChanged, callback);
    });
}

void SPro24DspProtocol::GetEffectParams(ResultCallback<EffectGeneralParams> callback) {
    tx_.ReadQuadlet(subsystem_, appSectionBase_ + kEffectGeneralOffset,
                    [callback](IOReturn status, uint32_t value) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        uint8_t data[4];
        DICETransaction::QuadletToWire(value, data);
        callback(kIOReturnSuccess, EffectGeneralParams::FromWire(data));
    });
}

void SPro24DspProtocol::SetEffectParams(const EffectGeneralParams& params, VoidCallback callback) {
    uint8_t data[4];
    params.ToWire(data);
    uint32_t value = DICETransaction::QuadletFromWire(data);
    
    tx_.WriteQuadlet(subsystem_, appSectionBase_ + kEffectGeneralOffset, value,
                     [this, callback](IOReturn status) {
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
        callback(kIOReturnSuccess, CompressorState::FromWire(data));
    });
}

void SPro24DspProtocol::SetCompressorState(const CompressorState& state, VoidCallback callback) {
    // Note: Need to allocate buffer that outlives async call
    auto buffer = std::make_shared<std::array<uint8_t, 2 * kCoefBlockSize>>();
    state.ToWire(buffer->data());
    
    WriteAppSection(kCoefOffset + CoefBlock::kCompressor * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::CoefChanged, callback);
    });
}

void SPro24DspProtocol::GetReverbState(ResultCallback<ReverbState> callback) {
    ReadAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, kCoefBlockSize,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, ReverbState::FromWire(data));
    });
}

void SPro24DspProtocol::SetReverbState(const ReverbState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, kCoefBlockSize>>();
    state.ToWire(buffer->data());
    
    WriteAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::CoefChanged, callback);
    });
}

void SPro24DspProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    ReadAppSection(kInputOffset, 8,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, InputParams::FromWire(data));
    });
}

void SPro24DspProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, 8>>();
    params.ToWire(buffer->data());
    
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
    ReadAppSection(kOutputGroupOffset, 64,  // Approximate size
                   [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, OutputGroupState::FromWire(data, size / 8));
    });
}

void SPro24DspProtocol::SetOutputGroupState(const OutputGroupState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, 64>>();
    state.ToWire(buffer->data());
    
    WriteAppSection(kOutputGroupOffset, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::OutputGroupChanged, callback);
    });
}

// ============================================================================
// TODO: Test only - Stream Control
// ============================================================================

void SPro24DspProtocol::StartStreamTest(VoidCallback callback) {
    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
    ASFW_LOG(DICE, "StartStreamTest: Beginning 48kHz DUPLEX stream test");
    ASFW_LOG(DICE, "  TX (Device→Host): Channel 0 - Recording");
    ASFW_LOG(DICE, "  RX (Host→Device): Channel 1 - Playback");
    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
    
    // Step 1: Set clock to 48kHz with internal source
    // Clock select = (rate_index << 8) | source
    // rate_index 2 = 48kHz, source 0x0C = Internal
    uint32_t clockSelect = (ClockRateIndex::k48000 << 8) | static_cast<uint32_t>(ClockSource::Internal);
    
    ASFW_LOG(DICE, "Step 1: Setting clock select to 0x%08x (48kHz Internal)", clockSelect);
    
    tx_.WriteQuadlet(subsystem_, sections_.global.offset + GlobalOffset::kClockSelect, clockSelect,
                     [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "❌ Failed to set clock select: 0x%x", status);
            callback(status);
            return;
        }
        
        ASFW_LOG(DICE, "✅ Clock select written");
        
        // Step 2: Set TX isochronous channel to 0
        uint32_t txChannel = 0;
        
        ASFW_LOG(DICE, "Step 2: Setting TX isoch channel to %u (Device→Host)", txChannel);
        
        tx_.WriteQuadlet(subsystem_, sections_.txStreamFormat.offset + TxOffset::kIsochronous, txChannel,
                         [this, callback](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "❌ Failed to set TX isoch channel: 0x%x", status);
                callback(status);
                return;
            }
            
            ASFW_LOG(DICE, "✅ TX isoch channel set to 0");
            
            // Step 3: Set TX speed to S400 (speed code 2)
            uint32_t speed = 2;  // S400
            
            ASFW_LOG(DICE, "Step 3: Setting TX speed to S400");
            
            tx_.WriteQuadlet(subsystem_, sections_.txStreamFormat.offset + TxOffset::kSpeed, speed,
                             [this, callback](IOReturn status) {
                if (status != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "❌ Failed to set TX speed: 0x%x", status);
                    callback(status);
                    return;
                }
                
                ASFW_LOG(DICE, "✅ TX speed set to S400");
                
                // Step 4: Set RX isochronous channel to 1 (for playback)
                uint32_t rxChannel = 1;
                
                ASFW_LOG(DICE, "Step 4: Setting RX isoch channel to %u (Host→Device)", rxChannel);
                
                tx_.WriteQuadlet(subsystem_, sections_.rxStreamFormat.offset + RxOffset::kIsochronous, rxChannel,
                                 [this, callback](IOReturn status) {
                    if (status != kIOReturnSuccess) {
                        ASFW_LOG(DICE, "❌ Failed to set RX isoch channel: 0x%x", status);
                        callback(status);
                        return;
                    }
                    
                    ASFW_LOG(DICE, "✅ RX isoch channel set to 1");
                    
                    // Step 5: Enable streaming (this starts both TX and RX)
                    uint32_t enable = 1;
                    
                    ASFW_LOG(DICE, "Step 5: Enabling streaming (both directions)");
                    
                    tx_.WriteQuadlet(subsystem_, sections_.global.offset + GlobalOffset::kEnable, enable,
                                     [this, callback](IOReturn status) {
                        if (status != kIOReturnSuccess) {
                            ASFW_LOG(DICE, "❌ Failed to enable streaming: 0x%x", status);
                            callback(status);
                            return;
                        }
                        
                        ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
                        ASFW_LOG(DICE, "✅ DUPLEX STREAMING ENABLED!");
                        ASFW_LOG(DICE, "   TX (Device→Host): ch 0, 48kHz, S400 - RECORDING");
                        ASFW_LOG(DICE, "   RX (Host→Device): ch 1, 48kHz       - PLAYBACK");
                        ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
                        
                        // Read back RX channel count to see what device reports
                        tx_.ReadQuadlet(subsystem_, sections_.rxStreamFormat.offset + RxOffset::kNumberAudio,
                                       [callback](IOReturn status, uint32_t rxAudioChannels) {
                            if (status == kIOReturnSuccess) {
                                ASFW_LOG(DICE, "📊 RX (playback) channels: %u", rxAudioChannels);
                            }
                            callback(kIOReturnSuccess);
                        });
                    });
                });
            });
        });
    });
}

} // namespace ASFW::Audio::DICE::Focusrite
