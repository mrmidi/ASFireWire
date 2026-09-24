#include <gtest/gtest.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Protocols/Backends/AudioDuplexCoordinator.hpp"
#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/Duplex/IDuplexDeviceControl.hpp"
#include "Audio/Protocols/IDeviceProtocol.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Bus/IRM/IRMTypes.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Testing/HostDriverKitStubs.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::AudioDuplexCoordinator;
using ASFW::Audio::IDeviceProtocol;
using ASFW::Audio::IDuplexDeviceControl;
using ASFW::Audio::IIsochDuplexHostTransport;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::DuplexClockApplyResult;
using ASFW::Audio::DuplexClockRequestOutcome;
using ASFW::Audio::DuplexConfirmResult;
using ASFW::Audio::DuplexHealthResult;
using ASFW::Audio::DuplexPrepareResult;
using ASFW::Audio::DuplexStageResult;
using ASFW::Audio::DuplexRestartErrorClass;
using ASFW::Audio::DuplexRestartFailureCause;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartSession;
using ASFW::Audio::DuplexLifecycleKind;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::Register32;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;
using ASFW::IRM::IRMClient;

constexpr uint64_t kTestGuid = 0x00130E0402004713ULL;
constexpr uint32_t kQueueBytes = 4096;
constexpr uint32_t kFocusriteVendorId = ASFW::DeviceProfiles::Audio::kFocusriteVendorId;
constexpr uint32_t kSPro24DspModelId = ASFW::DeviceProfiles::Audio::kSPro24DspModelId;
constexpr uint32_t kApogeeVendorId = ASFW::DeviceProfiles::Audio::kApogeeVendorId;
constexpr uint32_t kApogeeDuetModelId = ASFW::DeviceProfiles::Audio::kApogeeDuetModelId;
constexpr uint32_t kMAudioVendorId = ASFW::DeviceProfiles::Audio::kMAudioVendorId;
constexpr uint32_t kMAudioFireWire1814ModelId =
    ASFW::DeviceProfiles::Audio::kMAudioFireWire1814ModelId;
constexpr AudioClockConfig kSupportedClock{
    .sampleRateHz = 48000U,
};
constexpr AudioStreamRuntimeCaps kDefaultRuntimeCaps{
    .hostInputPcmChannels = 8,
    .hostOutputPcmChannels = 8,
    .deviceToHostAm824Slots = 17,
    .hostToDeviceAm824Slots = 9,
    .sampleRateHz = 48000,
};

struct SharedCallLog {
    void Add(std::string entry) {
        std::scoped_lock lock(mutex);
        entries.push_back(std::move(entry));
    }

    [[nodiscard]] std::vector<std::string> Snapshot() const {
        std::scoped_lock lock(mutex);
        return entries;
    }

    void Clear() {
        std::scoped_lock lock(mutex);
        entries.clear();
    }

    mutable std::mutex mutex;
    std::vector<std::string> entries;
};

class NullFireWireBus final : public IFireWireBus {
  public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 1};
    }

    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 2};
    }

    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>,
                     uint32_t responseLength, FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        std::array<uint8_t, 8> zeroes{};
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(zeroes.data(), responseLength));
        return AsyncHandle{.value = 3};
    }

    bool Cancel(AsyncHandle) override { return false; }

    [[nodiscard]] FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    [[nodiscard]] uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    [[nodiscard]] Generation GetGeneration() const override { return Generation{1}; }
    [[nodiscard]] NodeId GetLocalNodeID() const override { return NodeId{0}; }
};

class FakeDirectAudioBindingSource final : public ASFW::Audio::Runtime::IDirectAudioBindingSource {
  public:
    bool CopyDirectAudioBinding(
        ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out.generation = 1;
        out.valid = true;
        out.inputBase = reinterpret_cast<float*>(0x1234);
        out.inputFrames = 512;
        out.inputChannels = 8;
        out.outputBase = reinterpret_cast<const float*>(0x5678);
        out.outputFrames = 512;
        out.outputChannels = 8;
        out.control = reinterpret_cast<ASFW::Audio::Runtime::AudioTransportControlBlock*>(0x9abc);
        out.sampleRateHz = 48000;
        return true;
    }
};

class FakeIsochDuplexHostTransport final : public IIsochDuplexHostTransport {
  public:
    explicit FakeIsochDuplexHostTransport(SharedCallLog& log) noexcept : log_(log) {}

    kern_return_t BeginSplitDuplex(uint64_t guid) noexcept override {
        log_.Add("host.begin");
        lastGuid = guid;
        assignedChannelMask_ = 0;
        ++beginCalls;
        return beginStatus;
    }

    kern_return_t
    ReservePlaybackResources(uint64_t guid, IRMClient&, uint64_t allowedChannels,
                             uint32_t packetBandwidthUnits,
                             ASFW::Audio::Backends::IRMReservationResult& outResult) noexcept override {
        log_.Add("host.reserve_playback");
        lastGuid = guid;
        lastPlaybackAllowedChannels = allowedChannels;
        lastPlaybackBandwidth = packetBandwidthUnits;
        ++reservePlaybackCalls;
        outResult = {};
        outResult.status = reservePlaybackStatus;
        if (reservePlaybackStatus == kIOReturnSuccess) {
            outResult.channel = SelectChannel(allowedChannels);
            if (outResult.channel == ASFW::Audio::AudioStreamWireInfo::kInvalidIsoChannel) {
                outResult.status = kIOReturnNoResources;
                outResult.failure = ASFW::Audio::Backends::IsochReserveFailure::kChannelBusy;
                return kIOReturnNoResources;
            }
            lastPlaybackChannel = outResult.channel;
        }
        return reservePlaybackStatus;
    }

    kern_return_t
    ReserveCaptureResources(uint64_t guid, IRMClient&, uint64_t allowedChannels,
                            uint32_t packetBandwidthUnits,
                            ASFW::Audio::Backends::IRMReservationResult& outResult) noexcept override {
        log_.Add("host.reserve_capture");
        lastGuid = guid;
        lastCaptureAllowedChannels = allowedChannels;
        lastCaptureBandwidth = packetBandwidthUnits;
        ++reserveCaptureCalls;
        outResult = {};
        outResult.status = reserveCaptureStatus;
        if (reserveCaptureStatus == kIOReturnSuccess) {
            outResult.channel = SelectChannel(allowedChannels);
            if (outResult.channel == ASFW::Audio::AudioStreamWireInfo::kInvalidIsoChannel) {
                outResult.status = kIOReturnNoResources;
                outResult.failure = ASFW::Audio::Backends::IsochReserveFailure::kChannelBusy;
                return kIOReturnNoResources;
            }
            lastCaptureChannel = outResult.channel;
        }
        return reserveCaptureStatus;
    }

    kern_return_t PrepareReceive(
        uint8_t channel, HardwareInterface&,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        const ASFW::Audio::DirectRxFormatDescriptor& format = {}) noexcept override {
        log_.Add("host.prepare_receive");
        lastReceiveMotuPcmChunks = format.motuPcmChunks;
        lastReceiveMotuPorts = format.motuPorts;
        lastReceiveChannel = channel;
        lastReceiveBindingSource = bindingSource;
        lastReceiveWireFormat = format.wireFormat;
        lastReceiveAm824Slots = format.am824Slots;
        lastReceiveStreamChannels = format.streamChannels;
        lastReceiveTrustConfiguredStride = format.trustConfiguredStride;
        lastReceiveCaptureChannelMap = format.captureChannelMap;
        ++prepareReceiveCalls;
        return prepareReceiveStatus;
    }

    kern_return_t PrepareTransmit(uint8_t channel, HardwareInterface&,
                                  uint8_t sourceId,
                                  ASFW::FW::FwSpeed speed) noexcept override {
        log_.Add("host.prepare_transmit");
        lastTransmitChannel = channel;
        lastTransmitSourceId = sourceId;
        lastTransmitSpeed = speed;
        ++prepareTransmitCalls;
        return prepareTransmitStatus;
    }

    kern_return_t PrepareReceiveStream(
        uint32_t streamIndex, uint8_t channel, HardwareInterface&,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource, uint32_t channelOffset,
        const ASFW::Audio::DirectRxFormatDescriptor& format = {}) noexcept override {
        log_.Add("host.prepare_receive_stream");
        lastSecondaryReceiveMotuPcmChunks = format.motuPcmChunks;
        lastSecondaryReceiveIndex = streamIndex;
        lastSecondaryReceiveChannel = channel;
        lastSecondaryReceiveOffset = channelOffset;
        lastSecondaryReceiveChannels = format.streamChannels;
        lastSecondaryReceiveBindingSource = bindingSource;
        lastSecondaryReceiveCaptureChannelMap = format.captureChannelMap;
        ++prepareReceiveStreamCalls;
        return prepareReceiveStatus;
    }

    kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel, HardwareInterface&,
                                        uint8_t sourceId,
                                        ASFW::FW::FwSpeed speed) noexcept override {
        log_.Add("host.prepare_transmit_stream");
        lastSecondaryTransmitIndex = streamIndex;
        lastSecondaryTransmitChannel = channel;
        lastSecondaryTransmitSourceId = sourceId;
        lastSecondaryTransmitSpeed = speed;
        ++prepareTransmitStreamCalls;
        return prepareTransmitStatus;
    }

    kern_return_t StartPreparedReceive() noexcept override {
        log_.Add("host.start_receive");
        ++startReceiveCalls;
        return startReceiveStatus;
    }

    kern_return_t StartPreparedTransmit() noexcept override {
        log_.Add("host.start_transmit");
        ++startTransmitCalls;
        return startTransmitStatus;
    }

    kern_return_t StopPreparedReceive() noexcept override {
        log_.Add("host.stop_receive");
        ++stopReceiveCalls;
        return stopReceiveStatus;
    }

    kern_return_t StopPreparedTransmit() noexcept override {
        log_.Add("host.stop_transmit");
        ++stopTransmitCalls;
        return stopTransmitStatus;
    }

    kern_return_t StopAll() noexcept override {
        log_.Add("host.stop");
        ++stopCalls;
        return stopStatus;
    }

    kern_return_t beginStatus{kIOReturnSuccess};
    kern_return_t reservePlaybackStatus{kIOReturnSuccess};
    kern_return_t reserveCaptureStatus{kIOReturnSuccess};
    kern_return_t prepareReceiveStatus{kIOReturnSuccess};
    kern_return_t prepareTransmitStatus{kIOReturnSuccess};
    kern_return_t startReceiveStatus{kIOReturnSuccess};
    kern_return_t startTransmitStatus{kIOReturnSuccess};
    kern_return_t stopStatus{kIOReturnSuccess};
    kern_return_t stopReceiveStatus{kIOReturnSuccess};
    kern_return_t stopTransmitStatus{kIOReturnSuccess};

    uint64_t lastGuid{0};
    uint8_t lastPlaybackChannel{0};
    uint64_t lastPlaybackAllowedChannels{0};
    uint32_t lastPlaybackBandwidth{0};
    uint8_t lastCaptureChannel{0};
    uint64_t lastCaptureAllowedChannels{0};
    uint32_t lastCaptureBandwidth{0};
    uint8_t lastReceiveChannel{0};
    ASFW::Audio::Runtime::IDirectAudioBindingSource* lastReceiveBindingSource{nullptr};
    ASFW::Encoding::AudioWireFormat lastReceiveWireFormat{ASFW::Encoding::AudioWireFormat::kAM824};
    uint32_t lastReceiveAm824Slots{0};
    uint32_t lastReceiveStreamChannels{0};
    bool lastReceiveTrustConfiguredStride{false};
    ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap lastReceiveCaptureChannelMap{};
    uint32_t lastReceiveMotuPcmChunks{0};
    ASFW::Encoding::Motu::MotuPortMap lastReceiveMotuPorts{};
    ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap lastSecondaryReceiveCaptureChannelMap{};
    uint8_t lastTransmitChannel{0};
    uint8_t lastTransmitSourceId{0};
    ASFW::FW::FwSpeed lastTransmitSpeed{ASFW::FW::FwSpeed::S100};
    uint32_t lastTransmitMode{0};
    uint32_t lastTransmitPcmChannels{0};
    uint32_t lastTransmitDataBlockSize{0};
    ASFW::Encoding::AudioWireFormat lastTransmitWireFormat{ASFW::Encoding::AudioWireFormat::kAM824};
    ASFW::Audio::Runtime::IDirectAudioBindingSource* lastTransmitBindingSource{nullptr};

    int beginCalls{0};
    int reservePlaybackCalls{0};
    int reserveCaptureCalls{0};
    int prepareReceiveCalls{0};
    int prepareTransmitCalls{0};
    int prepareReceiveStreamCalls{0};
    int prepareTransmitStreamCalls{0};
    uint32_t lastSecondaryReceiveIndex{0};
    uint8_t lastSecondaryReceiveChannel{0};
    uint32_t lastSecondaryReceiveOffset{0};
    uint32_t lastSecondaryReceiveChannels{0};
    uint32_t lastSecondaryReceiveMotuPcmChunks{0};
    ASFW::Audio::Runtime::IDirectAudioBindingSource* lastSecondaryReceiveBindingSource{nullptr};
    uint32_t lastSecondaryTransmitIndex{0};
    uint8_t lastSecondaryTransmitChannel{0};
    uint8_t lastSecondaryTransmitSourceId{0};
    ASFW::FW::FwSpeed lastSecondaryTransmitSpeed{ASFW::FW::FwSpeed::S100};
    int startReceiveCalls{0};
    int startTransmitCalls{0};
    int stopCalls{0};
    int stopReceiveCalls{0};
    int stopTransmitCalls{0};

  private:
    [[nodiscard]] uint8_t SelectChannel(uint64_t allowedChannels) noexcept {
        const uint64_t available = allowedChannels & ~assignedChannelMask_;
        for (uint8_t channel = 0; channel < 64; ++channel) {
            const uint64_t bit = uint64_t{1} << channel;
            if ((available & bit) != 0) {
                assignedChannelMask_ |= bit;
                return channel;
            }
        }
        return ASFW::Audio::AudioStreamWireInfo::kInvalidIsoChannel;
    }

    SharedCallLog& log_;
    uint64_t assignedChannelMask_{0};
};

class FakeDiceProtocol final : public IDeviceProtocol, public IDuplexDeviceControl {
  public:
    FakeDiceProtocol(SharedCallLog& log, IRMClient& irmClient) noexcept
        : log_(log), irmClient_(irmClient) {}

    IOReturn Initialize() override { return kIOReturnSuccess; }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return "FakeDICE"; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override {
        outCaps = currentCaps_;
        return true;
    }

    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override {
        log_.Add("device.prepare");
        {
            std::unique_lock lock(mutex_);
            ++prepareCalls;
            lastChannels_ = channels;
            lastDesiredClock_ = desiredClock;
            if (deferPrepareCallback_) {
                deferredPrepareCallback_ = std::move(callback);
                prepareBlocked_ = true;
                cv_.notify_all();
                return;
            }
            if (holdPrepare_) {
                prepareBlocked_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this] { return !holdPrepare_; });
                prepareBlocked_ = false;
            }
        }

        if (prepareStatus == kIOReturnSuccess) {
            currentClock_ = desiredClock;
            currentCaps_ = prepareCaps_;
        }

        callback(prepareStatus, DuplexPrepareResult{
                                    .generation = Generation{1},
                                    .channels = channels,
                                    .appliedClock = currentClock_,
                                    .runtimeCaps = currentCaps_,
                                });
    }

    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept override {
        // Simulate the protocol adapter consuming the IRM result before PCR
        // programming; logging is intentionally omitted so lifecycle ordering
        // assertions remain about wire/host stages only.
        lastChannels_ = channels;
    }

    [[nodiscard]] const AudioDuplexChannels& LastChannels() const noexcept {
        return lastChannels_;
    }

    [[nodiscard]] AudioClockConfig LastDesiredClock() const noexcept {
        std::scoped_lock lock(mutex_);
        return lastDesiredClock_;
    }

    void ProgramRx(StageCallback callback) override {
        log_.Add("device.program_rx");
        ++programRxCalls;
        callback(programRxStatus, DuplexStageResult{
                                      .generation = Generation{1},
                                      .channels = lastChannels_,
                                      .phase = DuplexRestartPhase::kDeviceRxProgrammed,
                                      .runtimeCaps = currentCaps_,
                                  });
    }

    void ProgramTxAndEnableDuplex(StageCallback callback) override {
        log_.Add("device.program_tx");
        ++programTxCalls;
        callback(programTxStatus, DuplexStageResult{
                                      .generation = Generation{1},
                                      .channels = lastChannels_,
                                      .phase = DuplexRestartPhase::kDeviceTxArmed,
                                      .runtimeCaps = currentCaps_,
                                  });
    }

    void ConfirmDuplexStart(ConfirmCallback callback) override {
        log_.Add("device.confirm");
        ++confirmCalls;
        {
            std::unique_lock lock(mutex_);
            if (deferConfirmCallback_) {
                log_.Add("device.confirm.interim");
                deferredConfirmCallback_ = std::move(callback);
                confirmBlocked_ = true;
                cv_.notify_all();
                return;
            }
        }
        if (confirmStatus == kIOReturnSuccess) {
            currentCaps_ = confirmCaps_;
        }
        callback(confirmStatus, DuplexConfirmResult{
                                    .generation = Generation{1},
                                    .channels = lastChannels_,
                                    .appliedClock = currentClock_,
                                    .runtimeCaps = currentCaps_,
                                    .notification = 0x20,
                                    .status = 0x201,
                                    .extStatus = 0,
                                });
    }

    void SetDeferConfirmCallback(bool defer) {
        std::scoped_lock lock(mutex_);
        deferConfirmCallback_ = defer;
        if (!defer) {
            deferredConfirmCallback_ = {};
            confirmBlocked_ = false;
        }
        cv_.notify_all();
    }

    bool WaitUntilConfirmBlocked() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(2), [this] {
            return confirmBlocked_ && static_cast<bool>(deferredConfirmCallback_);
        });
    }

    void CompleteDeferredConfirm(IOReturn status) {
        ConfirmCallback callback;
        {
            std::scoped_lock lock(mutex_);
            callback = std::move(deferredConfirmCallback_);
            confirmBlocked_ = false;
        }
        if (status == kIOReturnSuccess) {
            currentCaps_ = confirmCaps_;
        }
        log_.Add(status == kIOReturnSuccess ? "device.confirm.accepted"
                                            : "device.confirm.rejected");
        callback(status, DuplexConfirmResult{
                            .generation = Generation{1},
                            .channels = lastChannels_,
                            .appliedClock = currentClock_,
                            .runtimeCaps = currentCaps_,
                            .notification = 0x20,
                            .status = 0x201,
                            .extStatus = 0,
                        });
    }

    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override {
        log_.Add("device.apply_clock");
        {
            std::unique_lock lock(mutex_);
            ++applyClockCalls;
            lastDesiredClock_ = desiredClock;
            if (holdApply_) {
                applyBlocked_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this] { return !holdApply_; });
                applyBlocked_ = false;
            }
        }

        if (applyClockStatus == kIOReturnSuccess) {
            currentClock_ = desiredClock;
            currentCaps_ = applyCaps_;
        }

        callback(applyClockStatus, DuplexClockApplyResult{
                                       .generation = Generation{1},
                                       .appliedClock = currentClock_,
                                       .runtimeCaps = currentCaps_,
                                   });
    }

    void ReadDuplexHealth(HealthCallback callback) override {
        log_.Add("device.health");
        const size_t readIndex = static_cast<size_t>(healthReadCalls++);
        const uint32_t statusValue =
            healthStatusSequence.empty()
                ? healthStatusValue
                : healthStatusSequence[std::min(readIndex, healthStatusSequence.size() - 1)];
        callback(healthStatus, DuplexHealthResult{
                                   .generation = healthGeneration,
                                   .appliedClock = currentClock_,
                                   .runtimeCaps = currentCaps_,
                                   .sourceLocked = ASFW::Audio::DICE::IsSourceLocked(statusValue),
                                   .nominalRateHz = ASFW::Audio::DICE::NominalRateHz(statusValue),
                                   .notification = healthNotification,
                                   .status = statusValue,
                                   .extStatus = healthExtStatusValue,
                               });
    }

    void DisconnectPlayback(VoidCallback callback) override {
        log_.Add("device.disconnect_playback");
        ++disconnectPlaybackCalls;
        callback(disconnectPlaybackStatus);
    }

    void DisconnectCapture(VoidCallback callback) override {
        log_.Add("device.disconnect_capture");
        ++disconnectCaptureCalls;
        callback(disconnectCaptureStatus);
    }

    IOReturn StopDuplex() override {
        log_.Add("device.stop");
        ++stopCalls;
        return stopStatus;
    }

    IRMClient* GetIRMClient() const override { return &irmClient_; }

    void SetHoldPrepare(bool hold) {
        std::scoped_lock lock(mutex_);
        holdPrepare_ = hold;
        cv_.notify_all();
    }

    void SetDeferPrepareCallback(bool defer) {
        std::scoped_lock lock(mutex_);
        deferPrepareCallback_ = defer;
        if (!defer) {
            deferredPrepareCallback_ = {};
            prepareBlocked_ = false;
        }
        cv_.notify_all();
    }

    PrepareCallback TakeDeferredPrepareCallback() {
        std::scoped_lock lock(mutex_);
        prepareBlocked_ = false;
        return std::move(deferredPrepareCallback_);
    }

    void SetHoldApply(bool hold) {
        std::scoped_lock lock(mutex_);
        holdApply_ = hold;
        cv_.notify_all();
    }

    bool WaitUntilPrepareBlocked(int expectedCalls) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(2), [this, expectedCalls] {
            return prepareCalls >= expectedCalls && prepareBlocked_;
        });
    }

    AudioClockConfig currentClock_{kSupportedClock};
    AudioStreamRuntimeCaps currentCaps_{kDefaultRuntimeCaps};
    AudioStreamRuntimeCaps prepareCaps_{kDefaultRuntimeCaps};
    AudioStreamRuntimeCaps confirmCaps_{kDefaultRuntimeCaps};
    AudioStreamRuntimeCaps applyCaps_{kDefaultRuntimeCaps};

    IOReturn prepareStatus{kIOReturnSuccess};
    IOReturn programRxStatus{kIOReturnSuccess};
    IOReturn programTxStatus{kIOReturnSuccess};
    IOReturn confirmStatus{kIOReturnSuccess};
    IOReturn applyClockStatus{kIOReturnSuccess};
    IOReturn healthStatus{kIOReturnSuccess};
    IOReturn stopStatus{kIOReturnSuccess};
    IOReturn disconnectPlaybackStatus{kIOReturnSuccess};
    IOReturn disconnectCaptureStatus{kIOReturnSuccess};

    uint32_t healthNotification{0x20};
    uint32_t healthStatusValue{0x201};
    uint32_t healthExtStatusValue{0};
    std::vector<uint32_t> healthStatusSequence{};
    Generation healthGeneration{1};

    int prepareCalls{0};
    int programRxCalls{0};
    int programTxCalls{0};
    int confirmCalls{0};
    int applyClockCalls{0};
    int healthReadCalls{0};
    int stopCalls{0};
    int disconnectPlaybackCalls{0};
    int disconnectCaptureCalls{0};

  private:
    SharedCallLog& log_;
    IRMClient& irmClient_;
    AudioDuplexChannels lastChannels_{};
    AudioClockConfig lastDesiredClock_{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool holdPrepare_{false};
    bool deferPrepareCallback_{false};
    bool holdApply_{false};
    bool prepareBlocked_{false};
    bool applyBlocked_{false};
    bool deferConfirmCallback_{false};
    bool confirmBlocked_{false};
    PrepareCallback deferredPrepareCallback_{};
    ConfirmCallback deferredConfirmCallback_{};
};

// A DICE unit publishes the vendor OUI as its specifier with interface version
// 1; a TA 1394 AV/C unit publishes 0x00A02D / 0x010001. The device catalog
// constrains the unit it selects, so a ROM with no unit directory at all
// resolves to nothing -- which is correct, and is what a real bus never shows.
constexpr uint32_t kDiceInterfaceVersion = 0x000001;
constexpr uint32_t kTa1394AvcSpecifier = 0x00A02D;
constexpr uint32_t kTa1394AvcVersion = 0x010001;

ConfigROM MakeConfigRom(uint64_t guid, uint32_t vendorId = kFocusriteVendorId,
                        uint32_t modelId = kSPro24DspModelId, Generation gen = Generation{1},
                        std::optional<uint32_t> unitSpecifier = std::nullopt,
                        uint32_t unitVersion = kDiceInterfaceVersion) {
    ConfigROM rom{};
    rom.gen = gen;
    rom.firstSeen = gen;
    rom.lastValidated = gen;
    rom.nodeId = 2;
    rom.bib.guid = guid;
    rom.bib.maxRec = 8;
    rom.rootDirMinimal = {
        RomEntry{.key = CfgKey::VendorId, .value = vendorId},
        RomEntry{.key = CfgKey::ModelId, .value = modelId},
    };
    ASFW::Discovery::UnitDirectory unit{};
    unit.offsetQuadlets = 5;
    unit.unitSpecId = unitSpecifier.value_or(vendorId);
    unit.unitSwVersion = unitVersion;
    rom.unitDirectories.push_back(unit);
    return rom;
}

ConfigROM MakeAvcConfigRom(uint64_t guid, uint32_t vendorId, uint32_t modelId,
                           Generation gen = Generation{1}) {
    return MakeConfigRom(guid, vendorId, modelId, gen, kTa1394AvcSpecifier,
                         kTa1394AvcVersion);
}

class AudioDuplexCoordinatorTests : public ::testing::Test {
  protected:
    AudioDuplexCoordinatorTests()
        : irmClient_(bus_), hostTransport_(log_),
          protocol_(std::make_shared<FakeDiceProtocol>(log_, irmClient_)),
          coordinator_(registry_, runtime_, hostTransport_, hardware_, &cancel_,
                       [this](uint64_t) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* {
                           return &bindingSource_;
                       }) {
        hardware_.SetTestRegister(Register32::kNodeID, 0);
        InstallDevice(protocol_);
    }

    void InstallDevice(const std::shared_ptr<IDeviceProtocol>& protocol) {
        (void)registry_.UpsertFromROM(MakeConfigRom(kTestGuid), LinkPolicy{});
        runtime_.Insert(kTestGuid, protocol);
    }

    // asyncSpeed is SpeedPolicy's demotable per-node speed; isochSpeed is the
    // Self-ID path speed. They are deliberately separate, so tests must be able
    // to drive them apart.
    void InstallDeviceWithLinkSpeed(const std::shared_ptr<IDeviceProtocol>& protocol,
                                    ASFW::FW::FwSpeed isochSpeed,
                                    ASFW::FW::FwSpeed asyncSpeed) {
        (void)registry_.UpsertFromROM(
            MakeConfigRom(kTestGuid),
            LinkPolicy{.localToNode = asyncSpeed, .isochToNode = isochSpeed});
        runtime_.Insert(kTestGuid, protocol);
    }

    void InstallDeviceWithLinkSpeed(const std::shared_ptr<IDeviceProtocol>& protocol,
                                    ASFW::FW::FwSpeed speed) {
        InstallDeviceWithLinkSpeed(protocol, speed, speed);
    }

    void InstallDeviceAtGeneration(Generation gen,
                                   const std::shared_ptr<IDeviceProtocol>& protocol) {
        (void)registry_.UpsertFromROM(
            MakeConfigRom(kTestGuid, kFocusriteVendorId, kSPro24DspModelId, gen), LinkPolicy{});
        runtime_.Insert(kTestGuid, protocol);
        protocol_->healthGeneration = gen;
    }

    [[nodiscard]] std::optional<DuplexRestartSession> GetSession() const {
        return coordinator_.GetSession(kTestGuid);
    }

    [[nodiscard]] std::vector<std::string> LogSnapshot() const { return log_.Snapshot(); }

    void ClearLog() { log_.Clear(); }

    // Blocks until the coordinator's stored session reports `reason` as its pending clock
    // request. RequestClockConfig keeps a single pending slot per GUID written under the
    // coordinator lock, so this lets a test serialize concurrent submissions: wait until one
    // request is observably enqueued before launching the next, instead of racing for the lock.
    [[nodiscard]] bool WaitForPendingClockReason(DuplexRestartReason reason) const {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto session = GetSession();
            if (session.has_value() && session->hasPendingClockRequest &&
                session->pendingReason == reason) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    NullFireWireBus bus_{};
    IRMClient irmClient_;
    HardwareInterface hardware_{};
    DeviceRegistry registry_{};
    AudioRuntimeRegistry runtime_{};
    SharedCallLog log_{};
    FakeIsochDuplexHostTransport hostTransport_;
    std::shared_ptr<FakeDiceProtocol> protocol_;
    FakeDirectAudioBindingSource bindingSource_{};
    std::atomic<bool> cancel_{false};
    AudioDuplexCoordinator coordinator_;
};

// The value the IRM reservation is charged at is the same value the isochronous
// transmit context runs at. Charging for one speed and transmitting at another
// is how a reservation that fits turns into packets the bus was not paid for.
TEST_F(AudioDuplexCoordinatorTests, TransmitSpeedIsTheSpeedTheReservationWasChargedAt) {
    InstallDeviceWithLinkSpeed(protocol_, ASFW::FW::FwSpeed::S200);

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(hostTransport_.lastTransmitSpeed, ASFW::FW::FwSpeed::S200);
    // 9 host->device AM824 slots x 8 events x 4 bytes + 8 CIP bytes = 296 bytes
    // of payload; 74 payload quadlets + 3 header quadlets, doubled for S200.
    EXPECT_EQ(hostTransport_.lastPlaybackBandwidth,
              ASFW::IRM::PacketBandwidthUnits(8 + 8 * 9 * 4,
                                              static_cast<uint8_t>(ASFW::FW::FwSpeed::S200)));
}

// Isochronous speed comes from Self-ID, never from async outcomes. SpeedPolicy
// demotes localToNode when a device times out a request — the Midas Venice F24
// genuinely needs async at S200 — and isoch used to inherit that demotion,
// which doubled the bandwidth charge (`unitsAtS1600 >> speedCode`) and put a
// two-node S400 bus at the edge of the 4915-unit budget for no reason.
//
// Apple keeps these apart deliberately: isoch speed comes from the PHY
// (IOFWIsochChannel.cpp:653) while the demotable per-node-pair fSpeedVector
// (demoted at IOFireWireController.cpp:2755-2759) is read only by async
// transmit (:7058).
TEST_F(AudioDuplexCoordinatorTests, AsyncSpeedDemotionDoesNotLowerIsochronousSpeed) {
    InstallDeviceWithLinkSpeed(protocol_,
                               /*isochSpeed=*/ASFW::FW::FwSpeed::S400,
                               /*asyncSpeed=*/ASFW::FW::FwSpeed::S200);

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(hostTransport_.lastTransmitSpeed, ASFW::FW::FwSpeed::S400);
    EXPECT_EQ(hostTransport_.lastPlaybackBandwidth,
              ASFW::IRM::PacketBandwidthUnits(8 + 8 * 9 * 4,
                                              static_cast<uint8_t>(ASFW::FW::FwSpeed::S400)));

    // And the charge really is half what the demoted async speed would have cost.
    EXPECT_EQ(hostTransport_.lastPlaybackBandwidth * 2,
              ASFW::IRM::PacketBandwidthUnits(8 + 8 * 9 * 4,
                                              static_cast<uint8_t>(ASFW::FW::FwSpeed::S200)));
}

TEST_F(AudioDuplexCoordinatorTests, S100IsochronousPathIsNotAnUnsetSpeed) {
    InstallDeviceWithLinkSpeed(protocol_,
                               /*isochSpeed=*/ASFW::FW::FwSpeed::S100,
                               /*asyncSpeed=*/ASFW::FW::FwSpeed::S400);

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(hostTransport_.lastTransmitSpeed, ASFW::FW::FwSpeed::S100);
    // Nine slots, eight events and an eight-byte CIP header: 296 payload
    // bytes plus 12 packet overhead bytes, scaled by four at S100.
    EXPECT_EQ(hostTransport_.lastPlaybackBandwidth, 1232U);
}

TEST_F(AudioDuplexCoordinatorTests, ColdStartTransitionsIdleToRunning) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kInitialStart);
    EXPECT_TRUE(session->deviceRunning);
    EXPECT_TRUE(session->hostTransmitStarted);
    EXPECT_TRUE(session->hostReceiveStarted);
    EXPECT_EQ(session->runtimeCaps.hostOutputPcmChannels,
              kDefaultRuntimeCaps.hostOutputPcmChannels);
    EXPECT_EQ(hostTransport_.reservePlaybackCalls, 1);
    EXPECT_EQ(hostTransport_.reserveCaptureCalls, 1);
    EXPECT_EQ(hostTransport_.prepareReceiveCalls, 1);
    EXPECT_EQ(hostTransport_.prepareTransmitCalls, 1);
    EXPECT_EQ(hostTransport_.startReceiveCalls, 1);
    EXPECT_EQ(hostTransport_.startTransmitCalls, 1);
    EXPECT_EQ(protocol_->prepareCalls, 1);
    EXPECT_EQ(protocol_->programRxCalls, 1);
    EXPECT_EQ(protocol_->programTxCalls, 1);
    EXPECT_EQ(protocol_->healthReadCalls, 3);
    EXPECT_EQ(protocol_->confirmCalls, 1);

    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{
                                 "host.begin",
                                 "device.prepare",
                                 "host.reserve_playback",
                                 "host.reserve_capture",
                                 "host.prepare_receive",
                                 "host.prepare_transmit",
                                 "device.health",
                                 "device.health",
                                 "device.health",
                                 "device.program_rx",
                                 "device.program_tx",
                                 "host.start_receive",
                                 "host.start_transmit",
                                 "device.confirm",
                             }));
}

TEST_F(AudioDuplexCoordinatorTests,
       MAudioDelayedStartConfirmationKeepsHostDuplexRunningUntilFinalAcceptance) {
    (void)registry_.UpsertFromROM(
        MakeAvcConfigRom(kTestGuid, kMAudioVendorId, kMAudioFireWire1814ModelId),
        LinkPolicy{});
    ClearLog();
    protocol_->SetDeferConfirmCallback(true);

    std::promise<IOReturn> startPromise;
    auto startFuture = startPromise.get_future();
    std::thread startThread(
        [&] { startPromise.set_value(coordinator_.StartStreaming(kTestGuid)); });

    const bool confirmBlocked = protocol_->WaitUntilConfirmBlocked();
    if (!confirmBlocked) {
        protocol_->SetDeferConfirmCallback(false);
        startThread.join();
        FAIL() << "start did not reach deferred duplex confirmation";
        return;
    }
    auto session = GetSession();
    if (!session.has_value()) {
        protocol_->CompleteDeferredConfirm(kIOReturnSuccess);
        startThread.join();
        FAIL() << "deferred confirmation had no start session";
        return;
    }
    EXPECT_TRUE(session->hostTransmitStarted);
    EXPECT_TRUE(session->hostReceiveStarted);
    EXPECT_FALSE(session->deviceRunning);
    EXPECT_EQ(hostTransport_.startTransmitCalls, 1);
    EXPECT_EQ(hostTransport_.startReceiveCalls, 1);
    EXPECT_EQ(hostTransport_.stopTransmitCalls, 0);
    EXPECT_EQ(hostTransport_.stopReceiveCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->stopCalls, 0);
    EXPECT_EQ(protocol_->disconnectPlaybackCalls, 0);
    EXPECT_EQ(protocol_->disconnectCaptureCalls, 0);
    EXPECT_EQ(LogSnapshot().back(), "device.confirm.interim");
    const auto beforeFinal = LogSnapshot();
    const auto txStart = std::find(beforeFinal.begin(), beforeFinal.end(), "host.start_transmit");
    const auto rxStart = std::find(beforeFinal.begin(), beforeFinal.end(), "host.start_receive");
    EXPECT_NE(txStart, beforeFinal.end());
    EXPECT_NE(rxStart, beforeFinal.end());
    if (txStart != beforeFinal.end() && rxStart != beforeFinal.end()) {
        EXPECT_LT(txStart, rxStart);
    }

    // The adapter reports its final accepted response after the interim. The
    // coordinator must then complete the same start instead of rolling it back.
    protocol_->CompleteDeferredConfirm(kIOReturnSuccess);
    EXPECT_EQ(startFuture.get(), kIOReturnSuccess);
    startThread.join();

    session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_TRUE(session->hostTransmitStarted);
    EXPECT_TRUE(session->hostReceiveStarted);
    EXPECT_TRUE(session->deviceRunning);
    EXPECT_EQ(hostTransport_.stopTransmitCalls, 0);
    EXPECT_EQ(hostTransport_.stopReceiveCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->stopCalls, 0);
    EXPECT_EQ(LogSnapshot().back(), "device.confirm.accepted");
}

TEST_F(AudioDuplexCoordinatorTests, RemoteDeviceLossRejectsRestartUntilRediscovery) {
    coordinator_.CancelRemoteDevice(kTestGuid);
    EXPECT_TRUE(coordinator_.IsDeviceOperationCancelled(kTestGuid));

    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNoDevice);
    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnAborted);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(protocol_->prepareCalls, 0);

    coordinator_.AcknowledgeDevicePresent(kTestGuid);
    EXPECT_FALSE(coordinator_.IsDeviceOperationCancelled(kTestGuid));
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
}

TEST_F(AudioDuplexCoordinatorTests,
       AvcProfileReservesBothDirectionsAndInterleavesHostStartsWithDeviceStages) {
    (void)registry_.UpsertFromROM(
        MakeAvcConfigRom(kTestGuid, kApogeeVendorId, kApogeeDuetModelId), LinkPolicy{});
    ClearLog();

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(hostTransport_.reservePlaybackCalls, 1);
    EXPECT_EQ(hostTransport_.reserveCaptureCalls, 1);
    EXPECT_EQ(hostTransport_.lastPlaybackAllowedChannels, ~uint64_t{0});
    EXPECT_EQ(hostTransport_.lastCaptureAllowedChannels, ~uint64_t{0});
    EXPECT_EQ(hostTransport_.lastPlaybackChannel, 0U);
    EXPECT_EQ(hostTransport_.lastCaptureChannel, 1U);
    EXPECT_EQ(protocol_->LastChannels().hostToDeviceIsoChannel, 0U);
    EXPECT_EQ(protocol_->LastChannels().deviceToHostIsoChannel, 1U);
    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->channels.hostToDeviceIsoChannel, 0U);
    EXPECT_EQ(session->channels.deviceToHostIsoChannel, 1U);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{
                                 "host.begin",
                                 "device.prepare",
                                 "host.reserve_playback",
                                 "host.reserve_capture",
                                 "host.prepare_receive",
                                 "host.prepare_transmit",
                                 "device.health",
                                 "device.health",
                                 "device.health",
                                 "host.start_receive",
                                 "device.program_rx",
                                 "host.start_transmit",
                                 "device.program_tx",
                                 "device.confirm",
                             }));

    ClearLog();
    protocol_->disconnectPlaybackStatus = kIOReturnTimeout;
    protocol_->disconnectCaptureStatus = kIOReturnError;
    hostTransport_.stopTransmitStatus = kIOReturnError;
    hostTransport_.stopReceiveStatus = kIOReturnTimeout;
    // FW-61: cleanup still proceeds through both directions, but a context
    // that fails to quiesce must be reported to the caller so its DMA-visible
    // mapping is not treated as safely releasable.
    ASSERT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnError);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{
                                 "device.disconnect_playback",
                                 "host.stop_transmit",
                                 "device.disconnect_capture",
                                 "host.stop_receive",
                                 "host.stop",
                             }));
    EXPECT_EQ(protocol_->stopCalls, 0);
}

TEST_F(AudioDuplexCoordinatorTests,
       ApogeeDuetStartForces48kBeforeDevicePreparationEvenAfterPersistedRate) {
    (void)registry_.UpsertFromROM(
        MakeAvcConfigRom(kTestGuid, kApogeeVendorId, kApogeeDuetModelId), LinkPolicy{});

    // A developer-only/manual rate request can leave an old value in the
    // session. Until dynamic Duet rate changes exist, a subsequent normal
    // start must not reuse it.
    ASSERT_EQ(coordinator_.RequestClockConfig(
                  kTestGuid, AudioClockConfig{.sampleRateHz = 44100U},
                  DuplexRestartReason::kManualReconfigure),
              kIOReturnSuccess);
    EXPECT_EQ(protocol_->LastDesiredClock().sampleRateHz, 44100U);

    ClearLog();
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(protocol_->LastDesiredClock().sampleRateHz, 48000U);
    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->desiredClock.sampleRateHz, 48000U);
    EXPECT_EQ(session->appliedClock.sampleRateHz, 48000U);

    const auto log = LogSnapshot();
    const auto prepare = std::find(log.begin(), log.end(), "device.prepare");
    const auto reservePlayback = std::find(log.begin(), log.end(), "host.reserve_playback");
    ASSERT_NE(prepare, log.end());
    ASSERT_NE(reservePlayback, log.end());
    EXPECT_LT(prepare, reservePlayback);
}

TEST_F(AudioDuplexCoordinatorTests, MAudioSpecialProfileIsRestrictedTo48k) {
    (void)registry_.UpsertFromROM(
        MakeAvcConfigRom(kTestGuid, kMAudioVendorId, kMAudioFireWire1814ModelId),
        LinkPolicy{});

    EXPECT_EQ(coordinator_.RequestClockConfig(
                  kTestGuid, AudioClockConfig{.sampleRateHz = 44100U},
                  DuplexRestartReason::kSampleRateChange), kIOReturnUnsupported);
    EXPECT_EQ(coordinator_.RequestClockConfig(
                  kTestGuid, AudioClockConfig{.sampleRateHz = 88200U},
                  DuplexRestartReason::kSampleRateChange), kIOReturnUnsupported);
    ASSERT_EQ(coordinator_.RequestClockConfig(
                  kTestGuid, AudioClockConfig{.sampleRateHz = 48000U},
                  DuplexRestartReason::kSampleRateChange), kIOReturnSuccess);
    EXPECT_EQ(protocol_->LastDesiredClock().sampleRateHz, 48000U);

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    EXPECT_EQ(protocol_->LastDesiredClock().sampleRateHz, 48000U);
    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->desiredClock.sampleRateHz, 48000U);
    EXPECT_EQ(session->appliedClock.sampleRateHz, 48000U);
}

TEST_F(AudioDuplexCoordinatorTests, TeardownCancelAbortsInFlightPrepare) {
    protocol_->SetDeferPrepareCallback(true);

    auto start =
        std::async(std::launch::async, [this] { return coordinator_.StartStreaming(kTestGuid); });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(1));
    cancel_.store(true, std::memory_order_release);

    ASSERT_EQ(start.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(start.get(), kIOReturnAborted);
    EXPECT_EQ(protocol_->programRxCalls, 0);
    EXPECT_EQ(protocol_->programTxCalls, 0);
    EXPECT_EQ(protocol_->confirmCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(coordinator_.TeardownAbortCount(), 1U);
}

// Abort provenance: when the device completes with kIOReturnAborted on its own,
// latching cancellation before the caller handles the result must NOT increment
// the lifecycle-abort counter (TeardownAbortCount). Only predicate-triggered aborts
// count toward lifecycle aborts.
TEST_F(AudioDuplexCoordinatorTests,
       DeviceAbortWithLatchingTeardownDoesNotIncrementTeardownAbortCount) {
    protocol_->SetDeferPrepareCallback(true);

    auto start =
        std::async(std::launch::async, [this] { return coordinator_.StartStreaming(kTestGuid); });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(1));
    auto cb = protocol_->TakeDeferredPrepareCallback();
    cb(kIOReturnAborted, DuplexPrepareResult{});
    cancel_.store(true, std::memory_order_release);

    ASSERT_EQ(start.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(start.get(), kIOReturnAborted);
    EXPECT_EQ(coordinator_.TeardownAbortCount(), 0U);
}

TEST_F(AudioDuplexCoordinatorTests,
       GlobalClockRequiresConsecutiveStableReadsBeforeHostIsochStarts) {
    protocol_->healthStatusSequence = {
        0x201, 0x200, 0x201, 0x201, 0x201,
    };

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    EXPECT_EQ(protocol_->healthReadCalls, 5);
    EXPECT_EQ(hostTransport_.startReceiveCalls, 1);
    EXPECT_EQ(hostTransport_.startTransmitCalls, 1);
}

TEST_F(AudioDuplexCoordinatorTests, GlobalClockHealthFailureRollsBackBeforeHostIsochStarts) {
    protocol_->healthStatus = kIOReturnNoDevice;

    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNoDevice);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kFailed);
    ASSERT_TRUE(session->lastFailure.has_value());
    EXPECT_EQ(session->lastFailure->failedPhase, DuplexRestartPhase::kWaitingGlobalClock);
    EXPECT_EQ(session->lastFailure->cause, DuplexRestartFailureCause::kGlobalClockLock);
    EXPECT_EQ(hostTransport_.startReceiveCalls, 0);
    EXPECT_EQ(hostTransport_.startTransmitCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);
}

TEST_F(AudioDuplexCoordinatorTests, StopStreamingClearsRestartProgressAndStopsHostAndDevice) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Idle);
    EXPECT_FALSE(session->deviceRunning);
    EXPECT_FALSE(session->hostTransmitStarted);
    EXPECT_FALSE(session->hostReceiveStarted);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{"host.stop", "device.stop"}));
}

TEST_F(AudioDuplexCoordinatorTests, DuplicateStopDoesNotReenterGlobalHostTeardown) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ASSERT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    EXPECT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnSuccess);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);
    EXPECT_TRUE(LogSnapshot().empty());
}

TEST_F(AudioDuplexCoordinatorTests, IdleClockApplyUsesDeviceOnlyPathAndReturnsToIdle) {
    protocol_->applyCaps_ = AudioStreamRuntimeCaps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 18,
        .hostToDeviceAm824Slots = 10,
        .sampleRateHz = 48000,
    };

    ASSERT_EQ(coordinator_.RequestClockConfig(kTestGuid, kSupportedClock,
                                              DuplexRestartReason::kManualReconfigure),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Idle);
    EXPECT_EQ(session->reason, DuplexRestartReason::kManualReconfigure);
    EXPECT_EQ(session->runtimeCaps.hostInputPcmChannels, 10U);
    EXPECT_EQ(session->runtimeCaps.hostOutputPcmChannels, 10U);
    EXPECT_EQ(protocol_->applyClockCalls, 1);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{"device.apply_clock"}));
}

// Hardware repro (Saffire Pro 24 DSP, 2026-09-24): run at 48 kHz, stop, pick
// 44.1 kHz in Audio MIDI Setup (the idle clock-apply path), start again. The
// start used the earlier run's 48 kHz desiredClock over the idle apply's
// 44.1 kHz appliedClock, relocked the device at 48 kHz while CoreAudio
// rendered 44.1 kHz, and playback came out about 9% sharp.
TEST_F(AudioDuplexCoordinatorTests, StartAfterIdleClockChangeUsesTheNewRate) {
    constexpr AudioClockConfig k44100{.sampleRateHz = 44100U};
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ASSERT_EQ(protocol_->LastDesiredClock().sampleRateHz, 48000U);
    ASSERT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnSuccess);

    // The device relocks at 44.1 kHz: STATUS locked, nominal rate index 1. The
    // start waits for the device to report the requested rate before streaming.
    protocol_->applyCaps_.sampleRateHz = 44100U;
    protocol_->healthStatusValue = 0x101U;
    ASSERT_EQ(coordinator_.RequestClockConfig(kTestGuid, k44100,
                                              DuplexRestartReason::kSampleRateChange),
              kIOReturnSuccess);
    ASSERT_EQ(protocol_->applyClockCalls, 1);
    const auto idle = GetSession();
    ASSERT_TRUE(idle.has_value());
    EXPECT_EQ(idle->desiredClock.sampleRateHz, 44100U);
    EXPECT_EQ(idle->appliedClock.sampleRateHz, 44100U);

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    EXPECT_EQ(protocol_->LastDesiredClock().sampleRateHz, 44100U);
}

TEST_F(AudioDuplexCoordinatorTests, RunningClockRequestPerformsFullStopAndRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();
    const int prepareBefore = protocol_->prepareCalls;

    ASSERT_EQ(coordinator_.RequestClockConfig(kTestGuid, kSupportedClock,
                                              DuplexRestartReason::kManualReconfigure),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kManualReconfigure);
    EXPECT_EQ(protocol_->applyClockCalls, 0);
    EXPECT_EQ(protocol_->prepareCalls, prepareBefore + 1);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, BusResetRecoveryRestartsRunningSessionOnNewGeneration) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();
    InstallDeviceAtGeneration(Generation{2}, protocol_);

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kBusResetRebind),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kBusResetRebind);
    EXPECT_EQ(session->topologyGeneration, Generation{2});
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, TimingLossRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, CycleInconsistentRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(
        coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterCycleInconsistent),
        kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kRecoverAfterCycleInconsistent);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, TxFaultRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTxFault),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kRecoverAfterTxFault);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, LockLossRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterLockLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kRecoverAfterLockLoss);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(AudioDuplexCoordinatorTests, ClockOperationInFlightTracksHostInitiatedChanges) {
    // Idle, streaming steady-state, and post-change steady-state must all read "not in
    // flight" — health probes use this to tell a genuine device-initiated clock move from
    // the echo of the host's own change (the Logic rate-switch feedback loop).
    EXPECT_FALSE(coordinator_.IsOperationInFlight(kTestGuid));

    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    EXPECT_FALSE(coordinator_.IsOperationInFlight(kTestGuid));

    protocol_->SetHoldPrepare(true);
    std::promise<IOReturn> requestPromise;
    std::future<IOReturn> requestFuture = requestPromise.get_future();
    std::thread requestThread([&] {
        requestPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DuplexRestartReason::kSampleRateChange));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(2));
    EXPECT_TRUE(coordinator_.IsOperationInFlight(kTestGuid));

    protocol_->SetHoldPrepare(false);
    EXPECT_EQ(requestFuture.get(), kIOReturnSuccess);
    requestThread.join();

    EXPECT_FALSE(coordinator_.IsOperationInFlight(kTestGuid));
}

TEST_F(AudioDuplexCoordinatorTests, LatestPendingClockRequestWinsDuringRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> firstPromise;
    std::future<IOReturn> firstFuture = firstPromise.get_future();
    std::thread firstThread([&] {
        firstPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DuplexRestartReason::kManualReconfigure));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(2));

    std::promise<IOReturn> secondPromise;
    std::promise<IOReturn> thirdPromise;
    std::future<IOReturn> secondFuture = secondPromise.get_future();
    std::future<IOReturn> thirdFuture = thirdPromise.get_future();

    std::thread secondThread([&] {
        secondPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DuplexRestartReason::kRecoverAfterTimingLoss));
    });

    // The pending slot's winner is decided by lock-acquisition order, so the second request
    // must be observably enqueued before the third is launched; otherwise the two threads
    // race for the lock and which one "wins" is nondeterministic (the source of CI flake).
    ASSERT_TRUE(WaitForPendingClockReason(DuplexRestartReason::kRecoverAfterTimingLoss));

    std::thread thirdThread([&] {
        thirdPromise.set_value(coordinator_.RequestClockConfig(kTestGuid, kSupportedClock,
                                                               DuplexRestartReason::kBusResetRebind));
    });

    // The third request must supersede the second before prepare is released so the drain
    // order is deterministic: second -> kSuperseded/kIOReturnAborted, third -> kApplied.
    ASSERT_TRUE(WaitForPendingClockReason(DuplexRestartReason::kBusResetRebind));

    protocol_->SetHoldPrepare(false);

    EXPECT_EQ(firstFuture.get(), kIOReturnSuccess);
    EXPECT_EQ(secondFuture.get(), kIOReturnAborted);
    EXPECT_EQ(thirdFuture.get(), kIOReturnSuccess);

    firstThread.join();
    secondThread.join();
    thirdThread.join();

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_EQ(session->reason, DuplexRestartReason::kBusResetRebind);
    ASSERT_TRUE(session->lastClockCompletion.has_value());
    EXPECT_EQ(session->lastClockCompletion->outcome, DuplexClockRequestOutcome::kApplied);
    EXPECT_EQ(session->lastClockCompletion->reason, DuplexRestartReason::kBusResetRebind);
    EXPECT_EQ(protocol_->prepareCalls, 3);
    EXPECT_EQ(hostTransport_.stopCalls, 2);
    EXPECT_EQ(protocol_->stopCalls, 2);
}

TEST_F(AudioDuplexCoordinatorTests, StopStreamingAbortsClockRequestsDuringRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> firstPromise;
    std::future<IOReturn> firstFuture = firstPromise.get_future();
    std::thread firstThread([&] {
        firstPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DuplexRestartReason::kManualReconfigure));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(2));

    std::promise<IOReturn> secondPromise;
    std::future<IOReturn> secondFuture = secondPromise.get_future();
    std::thread secondThread([&] {
        secondPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DuplexRestartReason::kBusResetRebind));
    });

    std::promise<IOReturn> stopPromise;
    std::future<IOReturn> stopFuture = stopPromise.get_future();
    std::thread stopThread([&] { stopPromise.set_value(coordinator_.StopStreaming(kTestGuid)); });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protocol_->SetHoldPrepare(false);

    EXPECT_EQ(firstFuture.get(), kIOReturnAborted);
    EXPECT_EQ(secondFuture.get(), kIOReturnAborted);
    EXPECT_EQ(stopFuture.get(), kIOReturnSuccess);

    firstThread.join();
    secondThread.join();
    stopThread.join();

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Idle);
    ASSERT_TRUE(session->lastClockCompletion.has_value());
    EXPECT_EQ(session->lastClockCompletion->outcome, DuplexClockRequestOutcome::kAbortedByStop);
    EXPECT_EQ(protocol_->prepareCalls, 2);
    // The in-flight clock operation already stopped the session. The explicit
    // stop acknowledges that terminal state without duplicating global host
    // teardown or protocol stop.
    EXPECT_EQ(hostTransport_.stopCalls, 2);
    EXPECT_EQ(protocol_->stopCalls, 2);
}

TEST_F(AudioDuplexCoordinatorTests, RouteRebindDuringPrepareInvalidatesRestartEpoch) {
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> startPromise;
    std::future<IOReturn> startFuture = startPromise.get_future();
    std::thread startThread(
        [&] { startPromise.set_value(coordinator_.StartStreaming(kTestGuid)); });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(1));
    registry_.InvalidateLiveMappingsForBusReset();
    InstallDeviceAtGeneration(Generation{1}, protocol_);
    protocol_->SetHoldPrepare(false);

    EXPECT_EQ(startFuture.get(), kIOReturnAborted);
    startThread.join();

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Idle);
    // Idle carries no terminal error by construction; assert that structurally.
    EXPECT_TRUE(std::holds_alternative<ASFW::Audio::LifecycleIdle>(session->lifecycle));
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->errorClass, DuplexRestartErrorClass::kEpochInvalidated);
    EXPECT_EQ(session->lastInvalidation->cause, DuplexRestartFailureCause::kPrepare);
    EXPECT_TRUE(session->lastInvalidation->retryable);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    // The old route has been invalidated. Host DMA must stop, while the driver
    // must not send a device stop command to the newly rebound route.
    EXPECT_EQ(protocol_->stopCalls, 0);
}

TEST_F(AudioDuplexCoordinatorTests, ProgramRxFailureRollsBackHostAndDeviceInOrder) {
    protocol_->programRxStatus = kIOReturnNoDevice;

    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNoDevice);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kFailed);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Failed);
    // Terminal error is the Failed alternative's payload (single authority).
    ASSERT_TRUE(std::holds_alternative<ASFW::Audio::LifecycleFailed>(session->lifecycle));
    EXPECT_EQ(std::get<ASFW::Audio::LifecycleFailed>(session->lifecycle).terminalError,
              kIOReturnNoDevice);
    ASSERT_TRUE(session->lastFailure.has_value());
    EXPECT_EQ(session->lastFailure->errorClass, DuplexRestartErrorClass::kStageFailure);
    EXPECT_EQ(session->lastFailure->cause, DuplexRestartFailureCause::kProgramRx);
    EXPECT_TRUE(session->lastFailure->rollbackAttempted);
    EXPECT_FALSE(session->hostReceiveStarted);
    EXPECT_FALSE(session->deviceRunning);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{
                                 "host.begin",
                                 "device.prepare",
                                 "host.reserve_playback",
                                 "host.reserve_capture",
                                 "host.prepare_receive",
                                 "host.prepare_transmit",
                                 "device.health",
                                 "device.health",
                                 "device.health",
                                 "device.program_rx",
                                 "host.stop",
                                 "device.stop",
                             }));
}

TEST_F(AudioDuplexCoordinatorTests, UnsupportedClockConfigFailsBeforeHostAllocation) {
    // 2x/4x rates are not yet validated end-to-end and must be rejected before
    // any host allocation. (44.1 kHz is a supported 1x rate as of the dynamic
    // sample-rate work.)
    const AudioClockConfig unsupportedClock{
        .sampleRateHz = 96000U,
    };

    EXPECT_EQ(coordinator_.RequestClockConfig(kTestGuid, unsupportedClock,
                                              DuplexRestartReason::kSampleRateChange),
              kIOReturnUnsupported);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->applyClockCalls, 0);
    EXPECT_FALSE(GetSession().has_value());
}

TEST_F(AudioDuplexCoordinatorTests,
       RecoveryTriggerIsIgnoredWhenSessionIsIdleWithoutFootprint) {
    // kIOReturnUnsupported, not kIOReturnSuccess (FW-146). An ignored recovery
    // ran nothing, and callers act on the difference: AVCAudioBackend used to
    // read this as "recovery succeeded" and reset the timing-loss escalation
    // budget, so a device that always declined could never exhaust its
    // attempts. The rest of this test already asserts that nothing ran —
    // beginCalls == 0 and the session stays Idle — which is precisely why
    // reporting success here was wrong.
    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Idle);
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->errorClass, DuplexRestartErrorClass::kEpochInvalidated);
    EXPECT_EQ(session->lastInvalidation->cause, DuplexRestartFailureCause::kTimingLoss);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->prepareCalls, 0);
}

TEST_F(AudioDuplexCoordinatorTests, RetryableFailedSessionRestartsAndClearsLastFailure) {
    protocol_->programRxStatus = kIOReturnTimeout;
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnTimeout);

    auto failedSession = GetSession();
    ASSERT_TRUE(failedSession.has_value());
    ASSERT_TRUE(failedSession->lastFailure.has_value());
    EXPECT_TRUE(failedSession->lastFailure->retryable);

    protocol_->programRxStatus = kIOReturnSuccess;
    ClearLog();

    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kRunning);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Running);
    EXPECT_FALSE(session->lastFailure.has_value());
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->cause, DuplexRestartFailureCause::kTimingLoss);
}

TEST_F(AudioDuplexCoordinatorTests, NonRetryableFailedSessionDoesNotRestartOnRecovery) {
    protocol_->programRxStatus = kIOReturnUnsupported;
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnUnsupported);

    const auto failedSession = GetSession();
    ASSERT_TRUE(failedSession.has_value());
    ASSERT_TRUE(failedSession->lastFailure.has_value());
    EXPECT_FALSE(failedSession->lastFailure->retryable);

    ClearLog();
    protocol_->programRxStatus = kIOReturnSuccess;

    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DuplexRestartPhase::kFailed);
    EXPECT_EQ(KindOf(session->lifecycle), DuplexLifecycleKind::Failed);
    EXPECT_EQ(protocol_->prepareCalls, 1);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
}

} // namespace


TEST_F(AudioDuplexCoordinatorTests, ChangedEndpointGeometryRejectsStartRecoveryAndClock) {
    coordinator_.SetEndpointStartGuard([](uint64_t) { return false; });
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNotReady);
    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnNotReady);
    EXPECT_EQ(coordinator_.RequestClockConfig(kTestGuid, {},
                                             DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnNotReady);
    // Rediscovery must not reopen a geometry latch; recreation owns that.
    coordinator_.AcknowledgeDevicePresent(kTestGuid);
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNotReady);
}

TEST_F(AudioDuplexCoordinatorTests, StaleTimingFaultCannotRecoverNewerSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    const auto original = GetSession();
    ASSERT_TRUE(original.has_value());
    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          original->restartId), kIOReturnSuccess);
    ClearLog();
    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          original->restartId), kIOReturnAborted);
    EXPECT_TRUE(LogSnapshot().empty());
}
