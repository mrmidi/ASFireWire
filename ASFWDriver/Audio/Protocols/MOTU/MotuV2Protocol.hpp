// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV2Protocol.hpp - Device protocol for MOTU protocol-v2 register devices.
//
// Thin adapter: all wire encoding/decoding lives in the pure codecs of
// MotuV2Registers.hpp; this class owns only transport (async register IO against
// kAddrBase + offset) and the cached device state those reads produce.
//
// Device-side streaming bring-up IS implemented here, through FamilyDriver.
// The host-side half -- replaying the device's per-data-block SPH presentation times --
// lives in Audio/Wire/MOTU (MotuEventOffsetCache captures, MotuTxTiming stamps), because
// it belongs with packet timing rather than register control.

#pragma once

#include "MotuV2Registers.hpp"
#include "../IDeviceProtocol.hpp"
#include "../Duplex/FamilyDriver.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace ASFW::Audio::Motu {

/// Device clock state as reported by the clock status register (0x0b14).
struct ClockStatus {
    uint32_t raw{0};
    uint32_t sampleRateHz{0};                ///< 0 when the rate index is unknown.
    std::optional<ClockSourceV2> source{};   ///< nullopt for reserved source codes.
};

/// MOTU protocol-v2 register device.
///
/// Also serves as its own FamilyDriver: the audio session reaches every protocol
/// through IDeviceProtocol::AsFamilyDriver(), so that is the seam a new family must
/// implement to be driven at all.
class MotuV2Protocol final : public IDeviceProtocol, public FamilyDriver {
public:
    using ClockStatusCallback = std::function<void(IOReturn, ClockStatus)>;
    using CompletionCallback = std::function<void(IOReturn)>;

    MotuV2Protocol(Protocols::Ports::FireWireBusOps& busOps,
                   Protocols::Ports::FireWireBusInfo& busInfo,
                   Discovery::DeviceRegistry& routeRegistry,
                   const Discovery::DeviceRouteToken& route,
                   uint32_t unitSwVersion,
                   ::ASFW::IRM::IRMClient* irmClient = nullptr);

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override;

    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;

    /// Report the device's stream geometry. Before PrepareDuplex has run this answers
    /// from the model's fixed chunk table rather than failing, so the nub can be
    /// published before streaming -- see MotuAudioBackend::EnsureNubForGuid for why that
    /// ordering matters.
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;

    /// Port names in host channel order (MotuPortLayout.hpp). They come from the model's
    /// static table, so unlike DICE they are available before PrepareDuplex. Optical
    /// extras past the table are left unnamed for the caller to synthesize.
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    //==========================================================================
    // Duplex bring-up (IDeviceProtocol hooks).
    //
    // MOTU v2 activates both directions in a single write to the iso-comm
    // control register, so the device-side choreography is only two registers:
    // packet format first, then iso-comm (motu-stream.c:376-401,
    // snd_motu_stream_start_duplex). The host owns iso channel allocation and
    // hands the assignments in via AudioDuplexChannels.
    //
    // These hooks are only the register half. MOTU is duplex-always and recovers its
    // media clock from the host replaying the device's own cadence -- both the
    // data-blocks-per-packet sequence and the per-block SPH presentation times
    // (motu-stream.c:205-207). That half lives in Audio/Wire/MOTU: MotuEventOffsetCache
    // captures the offsets on receive, MotuTxTiming stamps them back on transmit.
    //==========================================================================

    // IDeviceProtocol -> FamilyDriver. Returning `this` is what makes the audio
    // session able to drive this protocol at all.
    Audio::FamilyDriver* AsFamilyDriver() noexcept override { return this; }

    // ---- Stage chains (callback form; the FamilyDriver steps below wait on them) ----
    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback);
    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept;
    void ProgramRx(StageCallback callback);
    void ProgramTxAndEnableDuplex(StageCallback callback);
    void ConfirmDuplexStart(ConfirmCallback callback);
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback);
    void ReadDuplexHealth(HealthCallback callback);
    [[nodiscard]] IOReturn StopDuplex() override;

    // ---- FamilyDriver ----
    // Each step starts the callback chain above and waits for it
    // (FamilyStageWait.hpp), so the chains and their wire traffic are unchanged.
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;
    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    void AssignChannels(const AudioDuplexChannels& channels) override;
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override;
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override;
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override;
    [[nodiscard]] IOReturn DisconnectPlayback() override;
    [[nodiscard]] IOReturn DisconnectCapture() override;
    [[nodiscard]] IOReturn BreakConnections() override;
    [[nodiscard]] IOReturn Stop() override;

    /// Read and decode the clock status register.
    void ReadClockStatus(ClockStatusCallback callback);

    /// Read-modify-write the sample rate, preserving the clock source and all other bits.
    /// Fails with kIOReturnUnsupported for a rate the device does not implement.
    void SetSampleRate(uint32_t rateHz, CompletionCallback callback);

    /// Tell the device where to post its 4-byte async notifications. The caller owns the
    /// host address range (it must already be allocated and listening) and is responsible
    /// for re-registering after a bus reset, which clears the device-side value.
    /// `hostAddress` must lie inside [kAsyncMessageRegionStart, kAsyncMessageRegionEnd];
    /// anything else fails with kIOReturnBadArgument without touching the device.
    void RegisterAsyncMessageAddress(uint16_t hostNodeId,
                                     uint64_t hostAddress,
                                     CompletionCallback callback);

    /// Clear the notification address, releasing the device back to front-panel control
    /// (motu-transaction.c:121-133 writes zero to both halves on teardown).
    void ReleaseAsyncMessageAddress(CompletionCallback callback);

    /// True once a registration write has succeeded and not yet been released.
    [[nodiscard]] bool HasRegisteredAsyncAddress() const noexcept {
        return asyncAddressRegistered_.load(std::memory_order_acquire);
    }

    /// Last successfully decoded sample rate, or 0 before the first read completes.
    [[nodiscard]] uint32_t CachedSampleRateHz() const noexcept {
        return cachedSampleRateHz_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t UnitSwVersion() const noexcept { return unitSwVersion_; }

private:
    // Service teardown: the stage waits give up once it reads true.
    const std::atomic<bool>* teardownCancel_{nullptr};

    [[nodiscard]] static Async::FWAddress AddressOf(Reg reg) noexcept;

    /// Write the address-hi/address-lo pair as one logical operation. Both halves must
    /// land: the device only acts on a complete address, so a failed second write leaves
    /// it holding a half-updated value and the caller sees the failure.
    void WriteAsyncAddrPair(AsyncAddrValues values,
                            bool registered,
                            CompletionCallback callback);

    /// Apply the model-specific fetching-mode write around streaming, if this model
    /// needs one. A no-op success for the 828mk2 and 896HD.
    void ApplyFetchingModeIfNeeded(bool enable, CompletionCallback callback);

    /// Read-modify-write one register: read it, transform the value, write it back.
    /// Every duplex register on this device is RMW (reserved/low bits must survive), so
    /// the read failure and the write failure both surface through `callback`.
    void ModifyRegister(Reg reg,
                        std::function<uint32_t(uint32_t)> transform,
                        CompletionCallback callback);

    Protocols::Ports::ProtocolRegisterIO io_;
    /// Kept for the link speed written into the packet-format register; the register IO
    /// owns its own copy but does not expose speed resolution.
    Protocols::Ports::FireWireBusInfo& busInfo_;
    /// Iso channel/bandwidth allocation is owned by the coordinator, which reaches it
    /// through GetIRMClient(); the protocol only carries the handle.
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    const uint32_t unitSwVersion_;
    std::atomic<uint32_t> cachedSampleRateHz_{0};
    std::atomic<bool> asyncAddressRegistered_{false};
    bool initialized_{false};

    // Iso channels the host assigned, latched by PrepareDuplex48k and consumed by
    // ProgramTxAndEnableDuplex48k. Device-relative naming: RX is host->device
    // (playback), TX is device->host (capture).
    std::atomic<uint8_t> deviceRxChannel_{0};
    std::atomic<uint8_t> deviceTxChannel_{0};
    std::atomic<bool> duplexPrepared_{false};
    std::atomic<bool> duplexActive_{false};

    // Chunk geometry resolved during PrepareDuplex (fixed baseline plus any ADAT
    // extras), reported back through the duplex result structs.
    std::atomic<uint32_t> txPcmChunks_{0};
    std::atomic<uint32_t> rxPcmChunks_{0};
    std::atomic<uint32_t> preparedRateHz_{0};

    /// Fill the runtime capability block from the geometry resolved by PrepareDuplex.
    [[nodiscard]] AudioStreamRuntimeCaps MakeRuntimeCaps() const noexcept;
};

} // namespace ASFW::Audio::Motu
