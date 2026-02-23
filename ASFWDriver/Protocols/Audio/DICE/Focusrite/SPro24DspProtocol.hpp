// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspProtocol.hpp - Focusrite Saffire Pro 24 DSP protocol implementation
// Reference: snd-firewire-ctl-services/protocols/dice/src/focusrite/spro24dsp.rs

#pragma once

#include "SaffireproCommon.hpp"
#include "../Core/DICETransaction.hpp"
#include "../Core/DICETypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// Forward declare AsyncSubsystem
namespace ASFW::Async {
    class AsyncSubsystem;
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
// DSP Coefficient Layout
// ============================================================================

/// Size of one DSP coefficient block (in bytes)
constexpr size_t kCoefBlockSize = 0x88;

/// Number of coefficient blocks
constexpr size_t kCoefBlockCount = 8;

/// Block indices for DSP effects
namespace CoefBlock {
    constexpr size_t kCompressor = 2;
    constexpr size_t kEqualizer  = 2;
    constexpr size_t kReverb     = 3;
}

// ============================================================================
// DSP Effect States
// ============================================================================

/// Compressor state (2-channel)
struct CompressorState {
    std::array<float, 2> output{};     ///< Output volume (0.0 to 64.0)
    std::array<float, 2> threshold{};  ///< Threshold (-1.25 to 0.0)
    std::array<float, 2> ratio{};      ///< Ratio (0.03125 to 0.5)
    std::array<float, 2> attack{};     ///< Attack (-0.9375 to -1.0)
    std::array<float, 2> release{};    ///< Release (0.9375 to 1.0)
    
    /// Parse from wire format (2 × kCoefBlockSize bytes)
    static CompressorState FromWire(const uint8_t* data);
    
    /// Serialize to wire format
    void ToWire(uint8_t* data) const;
};

/// Reverb state
struct ReverbState {
    float size{0.0f};       ///< Room size (0.0 to 1.0)
    float air{0.0f};        ///< Air/damping (0.0 to 1.0)
    bool enabled{false};    ///< Reverb enabled
    float preFilter{0.0f};  ///< Pre-filter value (-1.0 to 1.0)
    
    /// Parse from wire format (kCoefBlockSize bytes)
    static ReverbState FromWire(const uint8_t* data);
    
    /// Serialize to wire format
    void ToWire(uint8_t* data) const;
};

/// Channel strip general parameters
struct EffectGeneralParams {
    std::array<bool, 2> eqAfterComp{};   ///< EQ after compressor
    std::array<bool, 2> compEnable{};    ///< Compressor enabled
    std::array<bool, 2> eqEnable{};      ///< Equalizer enabled
    
    /// Parse from wire format (4 bytes)
    static EffectGeneralParams FromWire(const uint8_t* data);
    
    /// Serialize to wire format
    void ToWire(uint8_t* data) const;
};

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
    /// @param subsystem  Reference to async subsystem
    /// @param nodeId     Target device node ID
    SPro24DspProtocol(Async::AsyncSubsystem& subsystem, uint16_t nodeId);
    
    /// Initialize protocol (reads sections, caches state)
    /// Note: This starts async operations. Use InitializeAsync for callback-based init.
    IOReturn Initialize() override;
    
    /// Shutdown protocol
    IOReturn Shutdown() override;
    
    /// Get device name
    const char* GetName() const override { return "Focusrite Saffire Pro 24 DSP"; }
    
    /// Device has DSP effects
    bool HasDsp() const override { return true; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    
    /// Configure device for 48kHz duplex streaming (TX ch0 / RX ch1).
    IOReturn StartDuplex48k() override;
    
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
    Async::AsyncSubsystem& subsystem_;
    DICETransaction tx_;
    GeneralSections sections_{};
    uint32_t appSectionBase_{0};
    bool initialized_{false};

    // Runtime-discovered DICE stream caps (authoritative after async capability discovery).
    std::atomic<uint32_t> runtimeSampleRateHz_{0};
    std::atomic<uint32_t> hostInputPcmChannels_{0};    // DICE TX stream 0 PCM
    std::atomic<uint32_t> hostOutputPcmChannels_{0};   // DICE RX stream 0 PCM
    std::atomic<uint32_t> deviceToHostAm824Slots_{0};  // DICE TX stream 0 slots
    std::atomic<uint32_t> hostToDeviceAm824Slots_{0};  // DICE RX stream 0 slots
    std::atomic<bool> runtimeCapsValid_{false};
    
    /// Send software notice to commit changes
    void SendSwNotice(SwNotice notice, VoidCallback callback);
    
    /// Read from application section
    void ReadAppSection(uint32_t offset, size_t size, DICEReadCallback callback);
    
    /// Write to application section
    void WriteAppSection(uint32_t offset, const uint8_t* data, size_t size, DICEWriteCallback callback);
};

} // namespace ASFW::Audio::DICE::Focusrite
