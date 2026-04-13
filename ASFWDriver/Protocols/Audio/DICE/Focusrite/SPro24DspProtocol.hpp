// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspProtocol.hpp - Focusrite Saffire Pro 24 DSP protocol implementation
// Reference: snd-firewire-ctl-services/protocols/dice/src/focusrite/spro24dsp.rs

#pragma once

#include "SaffireproCommon.hpp"
#include "SPro24DspTypes.hpp"
#include "../Core/DICETypes.hpp"
#include "../TCAT/DICETcatProtocol.hpp"
#include "../../IDeviceProtocol.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// Device Identification
// ============================================================================

/// Focusrite vendor ID (OUI)
constexpr uint32_t kFocusriteVendorId = 0x00130e;

/// Saffire Pro 24 DSP model ID
constexpr uint32_t kSPro24DspModelId = 0x000008;

// ============================================================================
// SPro24DspProtocol
// ============================================================================

/// Protocol handler for Focusrite Saffire Pro 24 DSP
/// 
/// This class provides async-callback-based access to device parameters.
/// All operations are asynchronous since they involve FireWire transactions.
class SPro24DspProtocol : public Audio::IDeviceProtocol {
public:
    /// Callback types for async operations
    using InitCallback = std::function<void(IOReturn)>;
    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;
    
    /// Construct protocol handler
    /// @param busOps     FireWire bus operations port
    /// @param busInfo    FireWire bus info port
    /// @param nodeId     Target device node ID
    SPro24DspProtocol(Protocols::Ports::FireWireBusOps& busOps,
                      Protocols::Ports::FireWireBusInfo& busInfo,
                      uint16_t nodeId,
                      ::ASFW::IRM::IRMClient* irmClient = nullptr);
    
    /// Initialize protocol (generic DICE init is delegated to the TCAT core)
    IOReturn Initialize() override;
    
    /// Shutdown protocol
    IOReturn Shutdown() override;
    
    /// Get device name
    const char* GetName() const override { return "Focusrite Saffire Pro 24 DSP"; }
    Audio::DICE::IDICEDuplexProtocol* AsDiceDuplexProtocol() noexcept override { return tcat_.AsDiceDuplexProtocol(); }
    const Audio::DICE::IDICEDuplexProtocol* AsDiceDuplexProtocol() const noexcept override { return tcat_.AsDiceDuplexProtocol(); }
    
    /// Device has DSP effects
    bool HasDsp() const override { return true; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    
    /// Configure device for 48kHz duplex streaming (TX ch0 / RX ch1).
    void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) override;
    void ProgramRxForDuplex48k(VoidCallback callback) override;
    void ProgramTxAndEnableDuplex48k(VoidCallback callback) override;
    void ConfirmDuplex48kStart(VoidCallback callback) override;
    IOReturn StopDuplex() override;
    ::ASFW::IRM::IRMClient* GetIRMClient() const override { return tcat_.GetIRMClient(); }
    void UpdateRuntimeContext(uint16_t nodeId,
                              Protocols::AVC::FCPTransport* transport) override;
    
    // ========================================================================
    // Async Initialization
    // ========================================================================
    
    /// Initialize protocol asynchronously
    void InitializeAsync(InitCallback callback);
    
    // ========================================================================
    // DSP Control (Async)
    // ========================================================================
    
    /// Enable/disable DSP
    void EnableDsp(bool enable, VoidCallback callback);
    
    /// Get effect general parameters
    void GetEffectParams(ResultCallback<EffectGeneralParams> callback);
    
    /// Set effect general parameters
    void SetEffectParams(const EffectGeneralParams& params, VoidCallback callback);
    
    /// Get compressor state
    void GetCompressorState(ResultCallback<CompressorState> callback);
    
    /// Set compressor state
    void SetCompressorState(const CompressorState& state, VoidCallback callback);
    
    /// Get reverb state
    void GetReverbState(ResultCallback<ReverbState> callback);
    
    /// Set reverb state
    void SetReverbState(const ReverbState& state, VoidCallback callback);
    
    // ========================================================================
    // Input/Output Control (Async)
    // ========================================================================
    
    /// Get input parameters
    void GetInputParams(ResultCallback<InputParams> callback);
    
    /// Set input parameters
    void SetInputParams(const InputParams& params, VoidCallback callback);
    
    /// Get output group state
    void GetOutputGroupState(ResultCallback<OutputGroupState> callback);
    
    /// Set output group state
    void SetOutputGroupState(const OutputGroupState& state, VoidCallback callback);

    // ========================================================================
    // TODO: Test only - Stream Control
    // ========================================================================
    
    /// Start isochronous TX stream for testing (48kHz, channel 0)
    /// This is a simplified test - real implementation would handle IRM allocation
    void StartStreamTest(VoidCallback callback);

private:
    TCAT::DICETcatProtocol tcat_;
    ExtensionSections extensionSections_{};
    uint32_t appSectionBase_{0};
    uint32_t commandSectionBase_{0};
    uint32_t routerSectionBase_{0};
    uint32_t currentConfigBase_{0};
    bool extensionsLoaded_{false};
    
    /// Send software notice to commit changes
    void SendSwNotice(SwNotice notice, VoidCallback callback);
    void EnsureExtensionsLoaded(VoidCallback callback);
    void ReadAppQuad(uint32_t offset, std::function<void(IOReturn, uint32_t)> callback);
    void WriteAppQuad(uint32_t offset, uint32_t value, VoidCallback callback);

    void HandleExtensionSectionsRead(IOReturn status,
                                     ExtensionSections sections,
                                     InitCallback callback);
    
    /// Read from application section
    void ReadAppSection(uint32_t offset, size_t size, DICEReadCallback callback);
    
    /// Write to application section
    void WriteAppSection(uint32_t offset, const uint8_t* data, size_t size, DICEWriteCallback callback);
};

} // namespace ASFW::Audio::DICE::Focusrite
