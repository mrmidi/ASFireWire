#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Backends/DiceDuplexRestartCoordinator.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "IRM/IRMClient.hpp"
#include "Protocols/Audio/DeviceProtocolFactory.hpp"
#include "Protocols/Audio/IDeviceProtocol.hpp"
#include "Protocols/Audio/DICE/Core/IDICEDuplexProtocol.hpp"

#include <condition_variable>
#include <chrono>
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
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::DICE::DiceClockApplyResult;
using ASFW::Audio::DICE::DiceClockRequestOutcome;
using ASFW::Audio::DICE::DiceDesiredClockConfig;
using ASFW::Audio::DICE::DiceDuplexConfirmResult;
using ASFW::Audio::DICE::DiceDuplexHealthResult;
using ASFW::Audio::DICE::DiceDuplexPrepareResult;
using ASFW::Audio::DICE::DiceDuplexStageResult;
using ASFW::Audio::DICE::DiceRestartErrorClass;
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartState;
using ASFW::Audio::DICE::DiceRestartSession;
using ASFW::Audio::DICE::IDICEDuplexProtocol;
using ASFW::Audio::DiceDuplexRestartCoordinator;
using ASFW::Audio::IDeviceProtocol;
using ASFW::Audio::IDiceHostTransport;
using ASFW::Audio::IDiceQueueMemoryProvider;
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
constexpr uint32_t kFocusriteVendorId = ASFW::Audio::DeviceProtocolFactory::kFocusriteVendorId;
constexpr uint32_t kSPro24DspModelId = ASFW::Audio::DeviceProtocolFactory::kSPro24DspModelId;
constexpr DiceDesiredClockConfig kSupportedClock{
    .sampleRateHz = 48000U,
    .clockSelect = ASFW::Audio::DICE::kDiceClockSelect48kInternal,
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
    AsyncHandle ReadBlock(Generation,
                          NodeId,
                          FWAddress,
                          uint32_t,
                          FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 1};
    }

    AsyncHandle WriteBlock(Generation,
                           NodeId,
                           FWAddress,
                           std::span<const uint8_t>,
                           FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 2};
    }

    AsyncHandle Lock(Generation,
                     NodeId,
                     FWAddress,
                     LockOp,
                     std::span<const uint8_t>,
                     uint32_t responseLength,
                     FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        std::array<uint8_t, 8> zeroes{};
        callback(AsyncStatus::kSuccess,
                 std::span<const uint8_t>(zeroes.data(), responseLength));
        return AsyncHandle{.value = 3};
    }

    bool Cancel(AsyncHandle) override { return false; }

    [[nodiscard]] FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    [[nodiscard]] uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    [[nodiscard]] Generation GetGeneration() const override { return Generation{1}; }
    [[nodiscard]] NodeId GetLocalNodeID() const override { return NodeId{0}; }
};

class FakeQueueMemoryProvider final : public IDiceQueueMemoryProvider {
public:
    FakeQueueMemoryProvider() {
        rxMem_ = MakeBuffer(kQueueBytes);
        txMem_ = MakeBuffer(kQueueBytes);
    }

    kern_return_t CopyRxQueueMemory(OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
                                    uint64_t& outBytes) noexcept override {
        outMem = rxMem_;
        outBytes = kQueueBytes;
        ++rxCopies;
        return kIOReturnSuccess;
    }

    kern_return_t CopyTransmitQueueMemory(OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
                                          uint64_t& outBytes) noexcept override {
        outMem = txMem_;
        outBytes = kQueueBytes;
        ++txCopies;
        return kIOReturnSuccess;
    }

    int rxCopies{0};
    int txCopies{0};

private:
    static OSSharedPtr<IOBufferMemoryDescriptor> MakeBuffer(uint64_t length) {
        IOBufferMemoryDescriptor* raw = nullptr;
        EXPECT_EQ(IOBufferMemoryDescriptor::Create(0, length, 16, &raw), kIOReturnSuccess);
        return OSSharedPtr<IOBufferMemoryDescriptor>(raw, OSNoRetain);
    }

    OSSharedPtr<IOBufferMemoryDescriptor> rxMem_{};
    OSSharedPtr<IOBufferMemoryDescriptor> txMem_{};
};

class FakeDiceHostTransport final : public IDiceHostTransport {
public:
    explicit FakeDiceHostTransport(SharedCallLog& log) noexcept
        : log_(log) {}

    kern_return_t BeginSplitDuplex(uint64_t guid) noexcept override {
        log_.Add("host.begin");
        lastGuid = guid;
        ++beginCalls;
        return beginStatus;
    }

    kern_return_t ReservePlaybackResources(uint64_t guid,
                                           IRMClient&,
                                           uint8_t channel,
                                           uint32_t bandwidthUnits) noexcept override {
        log_.Add("host.reserve_playback");
        lastGuid = guid;
        lastPlaybackChannel = channel;
        lastPlaybackBandwidth = bandwidthUnits;
        ++reservePlaybackCalls;
        return reservePlaybackStatus;
    }

    kern_return_t ReserveCaptureResources(uint64_t guid,
                                          IRMClient&,
                                          uint8_t channel,
                                          uint32_t bandwidthUnits) noexcept override {
        log_.Add("host.reserve_capture");
        lastGuid = guid;
        lastCaptureChannel = channel;
        lastCaptureBandwidth = bandwidthUnits;
        ++reserveCaptureCalls;
        return reserveCaptureStatus;
    }

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface&,
                               const OSSharedPtr<IOBufferMemoryDescriptor>&,
                               uint64_t rxBytes) noexcept override {
        log_.Add("host.start_receive");
        lastReceiveChannel = channel;
        lastReceiveBytes = rxBytes;
        ++startReceiveCalls;
        return startReceiveStatus;
    }

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface&,
                                uint8_t sourceId,
                                uint32_t streamModeRaw,
                                uint32_t pcmChannels,
                                uint32_t dataBlockSize,
                                ASFW::Encoding::AudioWireFormat wireFormat,
                                const OSSharedPtr<IOBufferMemoryDescriptor>&,
                                uint64_t txBytes,
                                const int32_t*,
                                uint64_t,
                                uint32_t) noexcept override {
        log_.Add("host.start_transmit");
        lastTransmitChannel = channel;
        lastTransmitSourceId = sourceId;
        lastTransmitMode = streamModeRaw;
        lastTransmitPcmChannels = pcmChannels;
        lastTransmitDataBlockSize = dataBlockSize;
        lastTransmitWireFormat = wireFormat;
        lastTransmitBytes = txBytes;
        ++startTransmitCalls;
        return startTransmitStatus;
    }

    kern_return_t StopDuplex(uint64_t guid, IRMClient*) noexcept override {
        log_.Add("host.stop");
        lastGuid = guid;
        ++stopCalls;
        return stopStatus;
    }

    kern_return_t beginStatus{kIOReturnSuccess};
    kern_return_t reservePlaybackStatus{kIOReturnSuccess};
    kern_return_t reserveCaptureStatus{kIOReturnSuccess};
    kern_return_t startReceiveStatus{kIOReturnSuccess};
    kern_return_t startTransmitStatus{kIOReturnSuccess};
    kern_return_t stopStatus{kIOReturnSuccess};

    uint64_t lastGuid{0};
    uint8_t lastPlaybackChannel{0};
    uint32_t lastPlaybackBandwidth{0};
    uint8_t lastCaptureChannel{0};
    uint32_t lastCaptureBandwidth{0};
    uint8_t lastReceiveChannel{0};
    uint64_t lastReceiveBytes{0};
    uint8_t lastTransmitChannel{0};
    uint8_t lastTransmitSourceId{0};
    uint32_t lastTransmitMode{0};
    uint32_t lastTransmitPcmChannels{0};
    uint32_t lastTransmitDataBlockSize{0};
    ASFW::Encoding::AudioWireFormat lastTransmitWireFormat{
        ASFW::Encoding::AudioWireFormat::kAM824};
    uint64_t lastTransmitBytes{0};

    int beginCalls{0};
    int reservePlaybackCalls{0};
    int reserveCaptureCalls{0};
    int startReceiveCalls{0};
    int startTransmitCalls{0};
    int stopCalls{0};

private:
    SharedCallLog& log_;
};

class FakeDiceProtocol final : public IDeviceProtocol, public IDICEDuplexProtocol {
public:
    FakeDiceProtocol(SharedCallLog& log, IRMClient& irmClient) noexcept
        : log_(log)
        , irmClient_(irmClient) {}

    IOReturn Initialize() override { return kIOReturnSuccess; }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return "FakeDICE"; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override {
        outCaps = currentCaps_;
        return true;
    }

    IDICEDuplexProtocol* AsDiceDuplexProtocol() noexcept override { return this; }
    const IDICEDuplexProtocol* AsDiceDuplexProtocol() const noexcept override { return this; }

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const DiceDesiredClockConfig& desiredClock,
                       PrepareCallback callback) override {
        log_.Add("device.prepare");
        {
            std::unique_lock lock(mutex_);
            ++prepareCalls;
            lastChannels_ = channels;
            lastDesiredClock_ = desiredClock;
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

        callback(prepareStatus,
                 DiceDuplexPrepareResult{
                     .generation = Generation{1},
                     .channels = channels,
                     .appliedClock = currentClock_,
                     .runtimeCaps = currentCaps_,
                 });
    }

    void ProgramRx(StageCallback callback) override {
        log_.Add("device.program_rx");
        ++programRxCalls;
        callback(programRxStatus,
                 DiceDuplexStageResult{
                     .generation = Generation{1},
                     .channels = lastChannels_,
                     .phase = DiceRestartPhase::kDeviceRxProgrammed,
                     .runtimeCaps = currentCaps_,
                 });
    }

    void ProgramTxAndEnableDuplex(StageCallback callback) override {
        log_.Add("device.program_tx");
        ++programTxCalls;
        callback(programTxStatus,
                 DiceDuplexStageResult{
                     .generation = Generation{1},
                     .channels = lastChannels_,
                     .phase = DiceRestartPhase::kDeviceTxArmed,
                     .runtimeCaps = currentCaps_,
                 });
    }

    void ConfirmDuplexStart(ConfirmCallback callback) override {
        log_.Add("device.confirm");
        ++confirmCalls;
        if (confirmStatus == kIOReturnSuccess) {
            currentCaps_ = confirmCaps_;
        }
        callback(confirmStatus,
                 DiceDuplexConfirmResult{
                     .generation = Generation{1},
                     .channels = lastChannels_,
                     .appliedClock = currentClock_,
                     .runtimeCaps = currentCaps_,
                     .notification = 0x20,
                     .status = 0x201,
                     .extStatus = 0,
                 });
    }

    void ApplyClockConfig(const DiceDesiredClockConfig& desiredClock,
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

        callback(applyClockStatus,
                 DiceClockApplyResult{
                     .generation = Generation{1},
                     .appliedClock = currentClock_,
                     .runtimeCaps = currentCaps_,
                 });
    }

    void ReadDuplexHealth(HealthCallback callback) override {
        ++healthReadCalls;
        callback(healthStatus,
                 DiceDuplexHealthResult{
                     .generation = Generation{1},
                     .appliedClock = currentClock_,
                     .runtimeCaps = currentCaps_,
                     .notification = healthNotification,
                     .status = healthStatusValue,
                     .extStatus = healthExtStatusValue,
                 });
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

    void SetHoldApply(bool hold) {
        std::scoped_lock lock(mutex_);
        holdApply_ = hold;
        cv_.notify_all();
    }

    bool WaitUntilPrepareBlocked(int expectedCalls) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock,
                            std::chrono::seconds(2),
                            [this, expectedCalls] {
                                return prepareCalls >= expectedCalls && prepareBlocked_;
                            });
    }

    DiceDesiredClockConfig currentClock_{kSupportedClock};
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

    uint32_t healthNotification{0x20};
    uint32_t healthStatusValue{0x201};
    uint32_t healthExtStatusValue{0};

    int prepareCalls{0};
    int programRxCalls{0};
    int programTxCalls{0};
    int confirmCalls{0};
    int applyClockCalls{0};
    int healthReadCalls{0};
    int stopCalls{0};

private:
    SharedCallLog& log_;
    IRMClient& irmClient_;
    AudioDuplexChannels lastChannels_{};
    DiceDesiredClockConfig lastDesiredClock_{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool holdPrepare_{false};
    bool holdApply_{false};
    bool prepareBlocked_{false};
    bool applyBlocked_{false};
};

ConfigROM MakeConfigRom(uint64_t guid,
                        uint32_t vendorId = kFocusriteVendorId,
                        uint32_t modelId = kSPro24DspModelId,
                        Generation gen = Generation{1}) {
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
    return rom;
}

class DiceDuplexRestartCoordinatorTests : public ::testing::Test {
protected:
    DiceDuplexRestartCoordinatorTests()
        : irmClient_(bus_)
        , hostTransport_(log_)
        , protocol_(std::make_shared<FakeDiceProtocol>(log_, irmClient_))
        , coordinator_(registry_,
                       hostTransport_,
                       hardware_,
                       [](uint64_t) {
                           return std::make_unique<FakeQueueMemoryProvider>();
                       }) {
        hardware_.SetTestRegister(Register32::kNodeID, 0);
        InstallDevice(protocol_);
    }

    void InstallDevice(const std::shared_ptr<IDeviceProtocol>& protocol) {
        auto& record = registry_.UpsertFromROM(MakeConfigRom(kTestGuid), LinkPolicy{});
        record.protocol = protocol;
    }

    void InstallDeviceAtGeneration(Generation gen, const std::shared_ptr<IDeviceProtocol>& protocol) {
        auto& record = registry_.UpsertFromROM(MakeConfigRom(kTestGuid,
                                                             kFocusriteVendorId,
                                                             kSPro24DspModelId,
                                                             gen),
                                               LinkPolicy{});
        record.protocol = protocol;
    }

    [[nodiscard]] std::optional<DiceRestartSession> GetSession() const {
        return coordinator_.GetSession(kTestGuid);
    }

    [[nodiscard]] bool WaitForPendingClockReason(DiceRestartReason reason) const {
        constexpr auto timeout = std::chrono::seconds(2);
        constexpr auto poll = std::chrono::milliseconds(1);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            const auto session = GetSession();
            if (session.has_value() &&
                session->hasPendingClockRequest &&
                session->pendingReason == reason) {
                return true;
            }
            std::this_thread::sleep_for(poll);
        }

        return false;
    }

    [[nodiscard]] std::vector<std::string> LogSnapshot() const {
        return log_.Snapshot();
    }

    void ClearLog() { log_.Clear(); }

    NullFireWireBus bus_{};
    IRMClient irmClient_;
    HardwareInterface hardware_{};
    DeviceRegistry registry_{};
    SharedCallLog log_{};
    FakeDiceHostTransport hostTransport_;
    std::shared_ptr<FakeDiceProtocol> protocol_;
    DiceDuplexRestartCoordinator coordinator_;
};

TEST_F(DiceDuplexRestartCoordinatorTests, ColdStartTransitionsIdleToRunning) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kInitialStart);
    EXPECT_TRUE(session->deviceRunning);
    EXPECT_TRUE(session->hostTransmitStarted);
    EXPECT_TRUE(session->hostReceiveStarted);
    EXPECT_EQ(session->runtimeCaps.hostOutputPcmChannels, kDefaultRuntimeCaps.hostOutputPcmChannels);
    EXPECT_EQ(hostTransport_.reservePlaybackCalls, 1);
    EXPECT_EQ(hostTransport_.reserveCaptureCalls, 1);
    EXPECT_EQ(hostTransport_.startReceiveCalls, 1);
    EXPECT_EQ(hostTransport_.startTransmitCalls, 1);
    EXPECT_EQ(protocol_->prepareCalls, 1);
    EXPECT_EQ(protocol_->programRxCalls, 1);
    EXPECT_EQ(protocol_->programTxCalls, 1);
    EXPECT_EQ(protocol_->confirmCalls, 1);
}

TEST_F(DiceDuplexRestartCoordinatorTests, StopStreamingClearsRestartProgressAndStopsHostAndDevice) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.StopStreaming(kTestGuid), kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session->state, DiceRestartState::kIdle);
    EXPECT_FALSE(session->deviceRunning);
    EXPECT_FALSE(session->hostTransmitStarted);
    EXPECT_FALSE(session->hostReceiveStarted);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{"host.stop", "device.stop"}));
}

TEST_F(DiceDuplexRestartCoordinatorTests, IdleClockApplyUsesDeviceOnlyPathAndReturnsToIdle) {
    protocol_->applyCaps_ = AudioStreamRuntimeCaps{
        .hostInputPcmChannels = 10,
        .hostOutputPcmChannels = 10,
        .deviceToHostAm824Slots = 18,
        .hostToDeviceAm824Slots = 10,
        .sampleRateHz = 48000,
    };

    ASSERT_EQ(coordinator_.RequestClockConfig(kTestGuid,
                                              kSupportedClock,
                                              DiceRestartReason::kManualReconfigure),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session->state, DiceRestartState::kIdle);
    EXPECT_EQ(session->reason, DiceRestartReason::kManualReconfigure);
    EXPECT_EQ(session->runtimeCaps.hostInputPcmChannels, 10U);
    EXPECT_EQ(session->runtimeCaps.hostOutputPcmChannels, 10U);
    EXPECT_EQ(protocol_->applyClockCalls, 1);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(LogSnapshot(), (std::vector<std::string>{"device.apply_clock"}));
}

TEST_F(DiceDuplexRestartCoordinatorTests, RunningClockRequestPerformsFullStopAndRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();
    const int prepareBefore = protocol_->prepareCalls;

    ASSERT_EQ(coordinator_.RequestClockConfig(kTestGuid,
                                              kSupportedClock,
                                              DiceRestartReason::kManualReconfigure),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kManualReconfigure);
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

TEST_F(DiceDuplexRestartCoordinatorTests, BusResetRecoveryRestartsRunningSessionOnNewGeneration) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();
    InstallDeviceAtGeneration(Generation{2}, protocol_);

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kBusResetRebind),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kBusResetRebind);
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

TEST_F(DiceDuplexRestartCoordinatorTests, TimingLossRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(DiceDuplexRestartCoordinatorTests, CycleInconsistentRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(
        coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterCycleInconsistent),
        kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kRecoverAfterCycleInconsistent);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(DiceDuplexRestartCoordinatorTests, TxFaultRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterTxFault),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kRecoverAfterTxFault);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(DiceDuplexRestartCoordinatorTests, LockLossRecoveryRestartsRunningSession) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    ClearLog();

    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterLockLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kRecoverAfterLockLoss);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);

    const auto calls = LogSnapshot();
    ASSERT_GE(calls.size(), 8U);
    EXPECT_EQ(calls[0], "host.stop");
    EXPECT_EQ(calls[1], "device.stop");
    EXPECT_EQ(calls[2], "host.begin");
    EXPECT_EQ(calls[3], "device.prepare");
}

TEST_F(DiceDuplexRestartCoordinatorTests, LatestPendingClockRequestWinsDuringRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> firstPromise;
    std::future<IOReturn> firstFuture = firstPromise.get_future();
    std::thread firstThread([&] {
        firstPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DiceRestartReason::kManualReconfigure));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(2));

    std::promise<IOReturn> secondPromise;
    std::promise<IOReturn> thirdPromise;
    std::future<IOReturn> secondFuture = secondPromise.get_future();
    std::future<IOReturn> thirdFuture = thirdPromise.get_future();

    std::thread secondThread([&] {
        secondPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DiceRestartReason::kRecoverAfterTimingLoss));
    });
    ASSERT_TRUE(WaitForPendingClockReason(DiceRestartReason::kRecoverAfterTimingLoss));

    std::thread thirdThread([&] {
        thirdPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DiceRestartReason::kBusResetRebind));
    });
    ASSERT_TRUE(WaitForPendingClockReason(DiceRestartReason::kBusResetRebind));

    protocol_->SetHoldPrepare(false);

    EXPECT_EQ(firstFuture.get(), kIOReturnSuccess);
    EXPECT_EQ(secondFuture.get(), kIOReturnAborted);
    EXPECT_EQ(thirdFuture.get(), kIOReturnSuccess);

    firstThread.join();
    secondThread.join();
    thirdThread.join();

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_EQ(session->reason, DiceRestartReason::kBusResetRebind);
    ASSERT_TRUE(session->lastClockCompletion.has_value());
    EXPECT_EQ(session->lastClockCompletion->outcome, DiceClockRequestOutcome::kApplied);
    EXPECT_EQ(session->lastClockCompletion->reason, DiceRestartReason::kBusResetRebind);
    EXPECT_EQ(protocol_->prepareCalls, 3);
    EXPECT_EQ(hostTransport_.stopCalls, 2);
    EXPECT_EQ(protocol_->stopCalls, 2);
}

TEST_F(DiceDuplexRestartCoordinatorTests, StopStreamingAbortsClockRequestsDuringRestart) {
    ASSERT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnSuccess);
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> firstPromise;
    std::future<IOReturn> firstFuture = firstPromise.get_future();
    std::thread firstThread([&] {
        firstPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DiceRestartReason::kManualReconfigure));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(2));

    std::promise<IOReturn> secondPromise;
    std::future<IOReturn> secondFuture = secondPromise.get_future();
    std::thread secondThread([&] {
        secondPromise.set_value(coordinator_.RequestClockConfig(
            kTestGuid, kSupportedClock, DiceRestartReason::kBusResetRebind));
    });

    std::promise<IOReturn> stopPromise;
    std::future<IOReturn> stopFuture = stopPromise.get_future();
    std::thread stopThread([&] {
        stopPromise.set_value(coordinator_.StopStreaming(kTestGuid));
    });

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
    EXPECT_EQ(session->phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session->state, DiceRestartState::kIdle);
    ASSERT_TRUE(session->lastClockCompletion.has_value());
    EXPECT_EQ(session->lastClockCompletion->outcome, DiceClockRequestOutcome::kAbortedByStop);
    EXPECT_EQ(protocol_->prepareCalls, 2);
    EXPECT_EQ(hostTransport_.stopCalls, 3);
    EXPECT_EQ(protocol_->stopCalls, 3);
}

TEST_F(DiceDuplexRestartCoordinatorTests, GenerationChangeDuringPrepareInvalidatesRestartEpoch) {
    protocol_->SetHoldPrepare(true);

    std::promise<IOReturn> startPromise;
    std::future<IOReturn> startFuture = startPromise.get_future();
    std::thread startThread([&] {
        startPromise.set_value(coordinator_.StartStreaming(kTestGuid));
    });

    ASSERT_TRUE(protocol_->WaitUntilPrepareBlocked(1));
    InstallDeviceAtGeneration(Generation{2}, protocol_);
    protocol_->SetHoldPrepare(false);

    EXPECT_EQ(startFuture.get(), kIOReturnAborted);
    startThread.join();

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session->state, DiceRestartState::kIdle);
    EXPECT_EQ(session->terminalError, kIOReturnSuccess);
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->errorClass, DiceRestartErrorClass::kEpochInvalidated);
    EXPECT_EQ(session->lastInvalidation->cause, DiceRestartFailureCause::kPrepare);
    EXPECT_TRUE(session->lastInvalidation->retryable);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
    EXPECT_EQ(protocol_->stopCalls, 1);
}

TEST_F(DiceDuplexRestartCoordinatorTests, ProgramRxFailureRollsBackHostAndDeviceInOrder) {
    protocol_->programRxStatus = kIOReturnNoDevice;

    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnNoDevice);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kFailed);
    EXPECT_EQ(session->state, DiceRestartState::kFailed);
    EXPECT_EQ(session->terminalError, kIOReturnNoDevice);
    ASSERT_TRUE(session->lastFailure.has_value());
    EXPECT_EQ(session->lastFailure->errorClass, DiceRestartErrorClass::kStageFailure);
    EXPECT_EQ(session->lastFailure->cause, DiceRestartFailureCause::kProgramRx);
    EXPECT_TRUE(session->lastFailure->rollbackAttempted);
    EXPECT_FALSE(session->hostReceiveStarted);
    EXPECT_FALSE(session->deviceRunning);
    EXPECT_EQ(LogSnapshot(),
              (std::vector<std::string>{
                  "host.begin",
                  "device.prepare",
                  "host.reserve_playback",
                  "device.program_rx",
                  "host.stop",
                  "device.stop",
              }));
}

TEST_F(DiceDuplexRestartCoordinatorTests, UnsupportedClockConfigFailsBeforeHostAllocation) {
    const DiceDesiredClockConfig unsupportedClock{
        .sampleRateHz = 44100U,
        .clockSelect = kSupportedClock.clockSelect,
    };

    EXPECT_EQ(coordinator_.RequestClockConfig(kTestGuid,
                                              unsupportedClock,
                                              DiceRestartReason::kSampleRateChange),
              kIOReturnUnsupported);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->applyClockCalls, 0);
    EXPECT_FALSE(GetSession().has_value());
}

TEST_F(DiceDuplexRestartCoordinatorTests, RecoveryTriggerIsIgnoredWhenSessionIsIdleWithoutFootprint) {
    ASSERT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kIdle);
    EXPECT_EQ(session->state, DiceRestartState::kIdle);
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->errorClass, DiceRestartErrorClass::kEpochInvalidated);
    EXPECT_EQ(session->lastInvalidation->cause, DiceRestartFailureCause::kTimingLoss);
    EXPECT_EQ(hostTransport_.beginCalls, 0);
    EXPECT_EQ(hostTransport_.stopCalls, 0);
    EXPECT_EQ(protocol_->prepareCalls, 0);
}

TEST_F(DiceDuplexRestartCoordinatorTests, RetryableFailedSessionRestartsAndClearsLastFailure) {
    protocol_->programRxStatus = kIOReturnTimeout;
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnTimeout);

    auto failedSession = GetSession();
    ASSERT_TRUE(failedSession.has_value());
    ASSERT_TRUE(failedSession->lastFailure.has_value());
    EXPECT_TRUE(failedSession->lastFailure->retryable);

    protocol_->programRxStatus = kIOReturnSuccess;
    ClearLog();

    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kRunning);
    EXPECT_EQ(session->state, DiceRestartState::kRunning);
    EXPECT_FALSE(session->lastFailure.has_value());
    ASSERT_TRUE(session->lastInvalidation.has_value());
    EXPECT_EQ(session->lastInvalidation->cause, DiceRestartFailureCause::kTimingLoss);
}

TEST_F(DiceDuplexRestartCoordinatorTests, NonRetryableFailedSessionDoesNotRestartOnRecovery) {
    protocol_->programRxStatus = kIOReturnUnsupported;
    EXPECT_EQ(coordinator_.StartStreaming(kTestGuid), kIOReturnUnsupported);

    const auto failedSession = GetSession();
    ASSERT_TRUE(failedSession.has_value());
    ASSERT_TRUE(failedSession->lastFailure.has_value());
    EXPECT_FALSE(failedSession->lastFailure->retryable);

    ClearLog();
    protocol_->programRxStatus = kIOReturnSuccess;

    EXPECT_EQ(coordinator_.RecoverStreaming(kTestGuid, DiceRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);

    const auto session = GetSession();
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->phase, DiceRestartPhase::kFailed);
    EXPECT_EQ(session->state, DiceRestartState::kFailed);
    EXPECT_EQ(protocol_->prepareCalls, 1);
    EXPECT_EQ(hostTransport_.stopCalls, 1);
}

} // namespace
