// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SessionTestSupport.hpp - One device, one host transport, one trace.
//
// Stage S2 of documentation/AUDIO_SESSION_REDESIGN.md replaces the duplex
// coordinator. These helpers record everything the session layer does, in
// order, into one WireTrace:
//
//   H ...   a host transport call (IRM reservation, DMA prepare/start/stop)
//   D ...   a device-control stage of a scripted (non-DICE) family
//   R/W/L   bus traffic of a real DICE protocol on SimulatedDiceDevice
//   ## ...  a call the test made into the session layer, and what it returned
//
// A scripted device stands in for the AV/C, BeBoB and MOTU protocols: the
// session layer sees only IDuplexDeviceControl, so the scripted stages and the
// recipe the device catalog resolves for the ROM are what shape its sequence.

#pragma once

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "Audio/Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "Audio/Protocols/Duplex/IDuplexDeviceControl.hpp"
#include "Audio/Protocols/IDeviceProtocol.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"

#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ASFW::Testing::Session {

using ::ASFW::Async::AsyncHandle;
using ::ASFW::Async::AsyncStatus;
using ::ASFW::Async::FWAddress;
using ::ASFW::Audio::AudioClockConfig;
using ::ASFW::Audio::AudioDuplexChannels;
using ::ASFW::Audio::AudioStreamRuntimeCaps;
using ::ASFW::Audio::DuplexClockApplyResult;
using ::ASFW::Audio::DuplexConfirmResult;
using ::ASFW::Audio::DuplexHealthResult;
using ::ASFW::Audio::DuplexPrepareResult;
using ::ASFW::Audio::DuplexStageResult;
using ::ASFW::Audio::IDeviceProtocol;
using ::ASFW::Audio::IDuplexDeviceControl;
using ::ASFW::Audio::IIsochDuplexHostTransport;
using ::ASFW::Discovery::CfgKey;
using ::ASFW::Discovery::ConfigROM;
using ::ASFW::Discovery::RomEntry;
using ::ASFW::FW::FwSpeed;
using ::ASFW::FW::Generation;
using ::ASFW::FW::LockOp;
using ::ASFW::FW::NodeId;
using ::ASFW::Testing::WireTrace;


inline std::string Status(IOReturn status) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<uint32_t>(status));
    return buf;
}

inline const char* SessionSpeedName(FwSpeed speed) {
    switch (speed) {
    case FwSpeed::S100: return "s100";
    case FwSpeed::S200: return "s200";
    case FwSpeed::S400: return "s400";
    case FwSpeed::S800: return "s800";
    }
    return "s?";
}

// The IRM client needs a bus; the host transport below never uses it.
class NullFireWireBus final : public ::ASFW::Async::IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          ::ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 1};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           ::ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 2};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>,
                     uint32_t responseLength, FwSpeed,
                     ::ASFW::Async::InterfaceCompletionCallback callback) override {
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

class FakeBindingSource final : public ::ASFW::Audio::Runtime::IDirectAudioBindingSource {
public:
    bool CopyDirectAudioBinding(::ASFW::Audio::Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out.generation = 1;
        out.valid = true;
        out.inputBase = reinterpret_cast<float*>(0x1234);
        out.inputFrames = 512;
        out.inputChannels = 8;
        out.outputBase = reinterpret_cast<const float*>(0x5678);
        out.outputFrames = 512;
        out.outputChannels = 8;
        out.control = reinterpret_cast<::ASFW::Audio::Runtime::AudioTransportControlBlock*>(0x9abc);
        out.sampleRateHz = 48000;
        return true;
    }
};

// Operations a test can fail or hook, by name. Hooks run before the operation
// completes, so a test can change the world at an exact point in the sequence.
using Hooks = std::map<std::string, std::function<void()>>;
using Failures = std::map<std::string, IOReturn>;

// Host transport that records every call, with the values that reach the
// wire or the DMA programs. The IRM picks the lowest allowed free channel.
class TracingHostTransport final : public IIsochDuplexHostTransport {
public:
    TracingHostTransport(WireTrace& trace, Hooks& hooks, Failures& failures) noexcept
        : trace_(trace), hooks_(hooks), failures_(failures) {}

    kern_return_t BeginSplitDuplex(uint64_t) noexcept override {
        assigned_ = 0;
        return Finish("begin", "H begin");
    }

    kern_return_t ReservePlaybackResources(
        uint64_t, ::ASFW::IRM::IRMClient&, uint64_t allowed, uint32_t bandwidth,
        ::ASFW::Audio::Backends::IRMReservationResult& out) noexcept override {
        return Reserve("reserve_playback", "playback", allowed, bandwidth, out);
    }

    kern_return_t ReserveCaptureResources(
        uint64_t, ::ASFW::IRM::IRMClient&, uint64_t allowed, uint32_t bandwidth,
        ::ASFW::Audio::Backends::IRMReservationResult& out) noexcept override {
        return Reserve("reserve_capture", "capture", allowed, bandwidth, out);
    }

    kern_return_t PrepareReceive(uint8_t channel, ::ASFW::Driver::HardwareInterface&,
                                 ::ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                 const ::ASFW::Audio::DirectRxFormatDescriptor& format = {}) noexcept override {
        return Finish("prepare_receive", "H prepare rx ch=" + std::to_string(channel) + Format(format));
    }

    kern_return_t PrepareReceiveStream(uint32_t index, uint8_t channel,
                                       ::ASFW::Driver::HardwareInterface&,
                                       ::ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                       uint32_t offset,
                                       const ::ASFW::Audio::DirectRxFormatDescriptor& format = {}) noexcept override {
        return Finish("prepare_receive", "H prepare rx[" + std::to_string(index) + "] ch=" +
                                             std::to_string(channel) + " off=" + std::to_string(offset) +
                                             Format(format));
    }

    kern_return_t PrepareTransmit(uint8_t channel, ::ASFW::Driver::HardwareInterface&,
                                  uint8_t sourceId, FwSpeed speed) noexcept override {
        return Finish("prepare_transmit", "H prepare tx ch=" + std::to_string(channel) + " sid=" +
                                              std::to_string(sourceId) + " @" + SessionSpeedName(speed));
    }

    kern_return_t PrepareTransmitStream(uint32_t index, uint8_t channel,
                                        ::ASFW::Driver::HardwareInterface&, uint8_t sourceId,
                                        FwSpeed speed) noexcept override {
        return Finish("prepare_transmit", "H prepare tx[" + std::to_string(index) + "] ch=" +
                                              std::to_string(channel) + " sid=" +
                                              std::to_string(sourceId) + " @" + SessionSpeedName(speed));
    }

    kern_return_t StartPreparedReceive() noexcept override { return Finish("start_receive", "H start rx"); }
    kern_return_t StartPreparedTransmit() noexcept override { return Finish("start_transmit", "H start tx"); }
    kern_return_t StopPreparedReceive() noexcept override { return Finish("stop_receive", "H stop rx"); }
    kern_return_t StopPreparedTransmit() noexcept override { return Finish("stop_transmit", "H stop tx"); }
    kern_return_t StopAll() noexcept override { return Finish("stop_all", "H stop all"); }

private:
    static std::string Format(const ::ASFW::Audio::DirectRxFormatDescriptor& f) {
        return " fmt=" + std::to_string(static_cast<uint32_t>(f.wireFormat)) +
               " slots=" + std::to_string(f.am824Slots) +
               " pcm=" + std::to_string(f.streamChannels) +
               (f.trustConfiguredStride ? " trust-stride" : "") +
               (f.motuPcmChunks != 0 ? " motu=" + std::to_string(f.motuPcmChunks) : "");
    }

    kern_return_t Reserve(const char* op, const char* dir, uint64_t allowed, uint32_t bandwidth,
                          ::ASFW::Audio::Backends::IRMReservationResult& out) {
        RunHook(op);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "H reserve %s allowed=%016llx bw=%u", dir,
                      static_cast<unsigned long long>(allowed), bandwidth);
        std::string line = buf;
        out = {};
        out.status = FailureFor(op);
        if (out.status == kIOReturnSuccess) {
            out.channel = Pick(allowed);
            if (out.channel == ::ASFW::Audio::AudioStreamWireInfo::kInvalidIsoChannel) {
                out.status = kIOReturnNoResources;
                out.failure = ::ASFW::Audio::Backends::IsochReserveFailure::kChannelBusy;
            }
        }
        line += out.status == kIOReturnSuccess ? " -> ch=" + std::to_string(out.channel)
                                               : " !" + Status(out.status);
        trace_.Add(line);
        return out.status;
    }

    kern_return_t Finish(const char* op, std::string line) {
        RunHook(op);
        const IOReturn status = FailureFor(op);
        if (status != kIOReturnSuccess) {
            line += " !" + Status(status);
        }
        trace_.Add(line);
        return status;
    }

    void RunHook(const std::string& op) {
        if (const auto it = hooks_.find("host." + op); it != hooks_.end()) {
            auto hook = std::move(it->second);
            hooks_.erase(it);
            hook();
        }
    }

    IOReturn FailureFor(const std::string& op) {
        if (const auto it = failures_.find("host." + op); it != failures_.end()) {
            const IOReturn status = it->second;
            failures_.erase(it);
            return status;
        }
        return kIOReturnSuccess;
    }

    uint8_t Pick(uint64_t allowed) {
        const uint64_t free = allowed & ~assigned_;
        for (uint8_t ch = 0; ch < 64; ++ch) {
            if ((free & (uint64_t{1} << ch)) != 0) {
                assigned_ |= uint64_t{1} << ch;
                return ch;
            }
        }
        return ::ASFW::Audio::AudioStreamWireInfo::kInvalidIsoChannel;
    }

    WireTrace& trace_;
    Hooks& hooks_;
    Failures& failures_;
    uint64_t assigned_{0};
};

// A non-DICE family as the session layer sees it: every stage completes at
// once, records itself, and can be failed or hooked by name ("device.<stage>").
class ScriptedDeviceControl final : public IDeviceProtocol, public IDuplexDeviceControl {
public:
    ScriptedDeviceControl(WireTrace& trace, Hooks& hooks, Failures& failures,
                          ::ASFW::IRM::IRMClient& irm, AudioStreamRuntimeCaps caps) noexcept
        : trace_(trace), hooks_(hooks), failures_(failures), irm_(irm), caps_(caps) {}

    IOReturn Initialize() override { return kIOReturnSuccess; }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return "Scripted"; }
    IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& out) const override {
        out = caps_;
        return true;
    }

    void EnsureRuntimeStreamGeometry(VoidCallback callback) override {
        callback(Stage("geometry", "D geometry"));
    }

    void PrepareDuplex(const AudioDuplexChannels& channels, const AudioClockConfig& clock,
                       PrepareCallback callback) override {
        channels_ = channels;
        const IOReturn status = Stage("prepare", "D prepare rate=" + std::to_string(clock.sampleRateHz) +
                                                     Channels(channels));
        if (status == kIOReturnSuccess) {
            clock_ = clock;
            caps_.sampleRateHz = clock.sampleRateHz;
        }
        callback(status, status == kIOReturnSuccess
                             ? DuplexPrepareResult{.generation = Generation{1}, .channels = channels,
                                                   .appliedClock = clock_, .runtimeCaps = caps_}
                             : DuplexPrepareResult{});
    }

    void SetAssignedChannels(const AudioDuplexChannels& channels) noexcept override {
        channels_ = channels;
        trace_.Add("D assign" + Channels(channels));
    }

    void ProgramRx(StageCallback callback) override {
        const IOReturn status = Stage("program_rx", "D program rx");
        callback(status, StageResult(status));
    }

    void ProgramTxAndEnableDuplex(StageCallback callback) override {
        const IOReturn status = Stage("program_tx", "D program tx+enable");
        callback(status, StageResult(status));
    }

    void ConfirmDuplexStart(ConfirmCallback callback) override {
        const IOReturn status = Stage("confirm", "D confirm");
        callback(status, status == kIOReturnSuccess
                             ? DuplexConfirmResult{.generation = Generation{1}, .channels = channels_,
                                                   .appliedClock = clock_, .runtimeCaps = caps_,
                                                   .notification = 0x20, .status = 0x201}
                             : DuplexConfirmResult{});
    }

    void ApplyClockConfig(const AudioClockConfig& clock, ClockApplyCallback callback) override {
        const IOReturn status = Stage("apply_clock", "D apply clock rate=" + std::to_string(clock.sampleRateHz));
        if (status == kIOReturnSuccess) {
            clock_ = clock;
            caps_.sampleRateHz = clock.sampleRateHz;
        }
        callback(status, status == kIOReturnSuccess
                             ? DuplexClockApplyResult{.generation = Generation{1},
                                                      .appliedClock = clock_, .runtimeCaps = caps_}
                             : DuplexClockApplyResult{});
    }

    void ReadDuplexHealth(HealthCallback callback) override {
        const IOReturn status = Stage("health", "D health");
        // Locked at the current clock: the pre-stream clock gate passes.
        const uint32_t rateIndex = clock_.sampleRateHz == 44100U ? 1U : 2U;
        const uint32_t statusValue = 0x1U | (rateIndex << 8);
        callback(status, DuplexHealthResult{.generation = Generation{1}, .appliedClock = clock_,
                                            .runtimeCaps = caps_, .sourceLocked = true,
                                            .clockReferenceHealthy = true,
                                            .nominalRateHz = clock_.sampleRateHz,
                                            .status = statusValue});
    }

    void DisconnectPlayback(VoidCallback callback) override {
        callback(Stage("disconnect_playback", "D disconnect playback"));
    }
    void DisconnectCapture(VoidCallback callback) override {
        callback(Stage("disconnect_capture", "D disconnect capture"));
    }
    void BreakBothConnections(VoidCallback callback) override {
        callback(Stage("break", "D break connections"));
    }
    void SetTeardownCancelToken(const std::atomic<bool>*) noexcept override {}
    IOReturn StopDuplex() override { return Stage("stop", "D stop"); }
    ::ASFW::IRM::IRMClient* GetIRMClient() const override { return &irm_; }

private:
    static std::string Channels(const AudioDuplexChannels& c) {
        std::string out = " cap=" + std::to_string(c.captureStreamCount) + "[";
        for (uint32_t i = 0; i < c.captureStreamCount; ++i) {
            out += (i ? "," : "") + std::to_string(c.captureIsoChannels[i]);
        }
        out += "] play=" + std::to_string(c.playbackStreamCount) + "[";
        for (uint32_t i = 0; i < c.playbackStreamCount; ++i) {
            out += (i ? "," : "") + std::to_string(c.playbackIsoChannels[i]);
        }
        return out + "]";
    }

    DuplexStageResult StageResult(IOReturn status) const {
        return status == kIOReturnSuccess
                   ? DuplexStageResult{.generation = Generation{1}, .channels = channels_,
                                       .runtimeCaps = caps_}
                   : DuplexStageResult{};
    }

    IOReturn Stage(const std::string& op, std::string line) {
        if (const auto it = hooks_.find("device." + op); it != hooks_.end()) {
            auto hook = std::move(it->second);
            hooks_.erase(it);
            hook();
        }
        IOReturn status = kIOReturnSuccess;
        if (const auto it = failures_.find("device." + op); it != failures_.end()) {
            status = it->second;
            failures_.erase(it);
            line += " !" + Status(status);
        }
        trace_.Add(line);
        return status;
    }

    WireTrace& trace_;
    Hooks& hooks_;
    Failures& failures_;
    ::ASFW::IRM::IRMClient& irm_;
    AudioStreamRuntimeCaps caps_;
    AudioDuplexChannels channels_{};
    AudioClockConfig clock_{.sampleRateHz = 48000U};
};

// Which device the session drives: its ROM identity (the catalog resolves the
// recipe from it) and whether a real DICE protocol runs on the simulator.
struct SessionShape {
    const char* key;
    uint32_t vendorId;
    uint32_t modelId;
    uint32_t unitSpecifier;
    uint32_t unitVersion;
    const ::ASFW::Testing::DICE::DiceDeviceImage* diceImage{nullptr};  // null: scripted device
    bool spro24Dsp{false};
};

inline constexpr uint32_t kDiceUnitVersion = 0x000001;
inline constexpr uint32_t kAvcUnitSpecifier = 0x00A02D;
inline constexpr uint32_t kAvcUnitVersion = 0x010001;

// The GUID carries the vendor's OUI in its top 24 bits, as a real one does;
// the catalog cross-checks it.
inline constexpr uint64_t SessionGuid(const SessionShape& shape) noexcept {
    return (uint64_t{shape.vendorId} << 40) | 0x0004713ULL;
}

inline ConfigROM MakeSessionRom(const SessionShape& shape, Generation gen) {
    ConfigROM rom{};
    rom.gen = gen;
    rom.firstSeen = gen;
    rom.lastValidated = gen;
    rom.nodeId = 2;
    rom.bib.guid = SessionGuid(shape);
    rom.bib.maxRec = 8;
    rom.rootDirMinimal = {
        RomEntry{.key = CfgKey::VendorId, .value = shape.vendorId},
        RomEntry{.key = CfgKey::ModelId, .value = shape.modelId},
    };
    ::ASFW::Discovery::UnitDirectory unit{};
    unit.offsetQuadlets = 5;
    unit.unitSpecId = shape.unitSpecifier;
    unit.unitSwVersion = shape.unitVersion;
    rom.unitDirectories.push_back(unit);
    return rom;
}

} // namespace ASFW::Testing::Session
