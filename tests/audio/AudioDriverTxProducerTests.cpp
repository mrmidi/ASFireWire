// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDriverTxProducerTests.cpp
//
// Runs the driver's real TX producer -- StartIO's arm step, the prefill, and the
// TxPreparationReady action handler from ASFWAudioDriverZts.cpp -- against an
// emulated IT transport. The IIG-generated classes are host shims
// (tests/mocks/net.mrmidi.ASFW.ASFWDriver), so the handler bodies compile
// unchanged.
//
// The emulator follows the consumer side of IsochTxDmaRing::Refill: each
// interrupt retires a batch of packets, publishes their OUTPUT_LAST completion
// stamps and a cycle-timer/host-time pair, requests a refill, checks that the
// descriptors it loads 48 packets ahead are committed, and stops on a producer
// fault exactly as the ring does.
//
// Before this existed the producer glue had no host coverage at all: the
// FW-255 bring-up died ~97 ms after IT start (the first steady-state M-Audio
// NO-DATA slot was rejected by CommitPacket) and every host test stayed green.

#include "Audio/DriverKit/ASFWAudioDriverPrivate.hpp"
#include "Audio/DriverKit/Config/AVC/MAudioSpecialProfile.hpp"
#include "Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.hpp"
#include "Audio/DriverKit/Config/ResolvedStreamConfig.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Isoch/Core/IsochTxQueue.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include "ASFWAudioDevice.h"

#include "../support/MAudioSpecialHappyPathFixture.inc"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
using ASFW::Isoch::IsochTxPacketMeta;
using ASFW::Isoch::IsochTxQueueControl;
using ASFW::Isoch::IsochTxQueueStatus;
using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
namespace Capture = MAudioSpecialHappyPathFixture;

constexpr uint32_t kCyclesPerSecond = 8000;
// IsochTxDmaRing keeps this many descriptors loaded ahead of the hardware.
constexpr uint32_t kDescriptorsAhead = 48;
// The live ring interrupts every few packets; FireBug-era logs show a
// completion delta high-water of 6-7.
constexpr uint32_t kPacketsPerInterrupt = 8;

struct WirePacket final {
    uint64_t index{0};
    std::vector<uint8_t> bytes;

    [[nodiscard]] uint32_t Quadlet(size_t offset) const {
        return (uint32_t{bytes[offset]} << 24) | (uint32_t{bytes[offset + 1]} << 16) |
               (uint32_t{bytes[offset + 2]} << 8) | uint32_t{bytes[offset + 3]};
    }
    [[nodiscard]] uint8_t Dbc() const { return bytes[3]; }
    [[nodiscard]] uint16_t Syt() const { return static_cast<uint16_t>(Quadlet(4) & 0xFFFF); }
    [[nodiscard]] bool IsData() const { return Syt() != 0xFFFF; }
};

struct TransportFault final {
    uint64_t packetIndex{0};
    const char* reason{""};
};

// Owns everything StartIO would own for the primary playback stream, and
// plays the transport's part of the shared-queue contract.
class TxProducerRig final {
public:
    TxProducerRig() {
        device_ = new ASFWAudioDevice();
        device_->zeroTimestampPeriod = Geometry::kHalZeroTimestampPeriodFrames;
        ivars_.audioDevice = OSSharedPtr<ASFWAudioDevice>(device_, OSNoRetain);
        ivars_.device.audioNub = &nub_;
        ivars_.runtime.directAudioGraph.control = control_.get();
        driver_.ivars = &ivars_;
    }

    ~TxProducerRig() { driver_.ivars = nullptr; }

    TxProducerRig(const TxProducerRig&) = delete;
    TxProducerRig& operator=(const TxProducerRig&) = delete;

    // Mirrors StartIO: select the clock domain, arm the producer on freshly
    // mapped memory, prefill one lap, then hand the queue to the transport.
    [[nodiscard]] bool Start(const ASFW::Isoch::Audio::IAudioStreamProfile& profile,
                             ProfileBuilderId builder,
                             uint32_t sampleRateHz) {
        ivars_.device.profileBuilderId = static_cast<uint32_t>(builder);
        ivars_.device.currentSampleRate = sampleRateHz;
        // Mirrors BuildAudioGraph: the profile and the timing geometry are
        // resolved once before StartIO ever runs (FW-183).
        ivars_.device.profile = &profile;
        const auto timing = ASFW::Audio::DriverKit::ResolveProfileTimingGeometry(
            profile, sampleRateHz, ivars_.device.streamModeRaw);
        if (!timing) {
            return false;
        }
        ivars_.device.timing = *timing;
        control_->ResetForStart();
        if (!ASFW::Audio::DriverKit::SelectTxClockDomain(ivars_, profile)) {
            return false;
        }

        ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
        if (!profile.BuildDefaultTxStreamConfig(txConfig)) {
            return false;
        }
        txConfig.sampleRate = sampleRateHz;

        numSlots_ = Geometry::kTxSharedSlotPackets;
        stride_ = ASFW::Isoch::Audio::TxPacketBytesForStreamConfig(txConfig);
        payload_.assign(static_cast<size_t>(numSlots_) * stride_, 0);
        metadata_ = std::make_unique<IsochTxPacketMeta[]>(numSlots_);
        queue_ = std::make_unique<IsochTxQueueControl>();

        const auto armed = ASFW::Audio::DriverKit::ArmPrimaryTxProducer(
            ivars_, profile, txConfig,
            {.payloadBase = payload_.data(),
             .metadataRing = metadata_.get(),
             .queueControl = queue_.get(),
             .numSlots = numSlots_,
             .slotStrideBytes = stride_});
        if (armed.failedStage != nullptr) {
            return false;
        }

        ASFW::Audio::DriverKit::PrefillTxRingBeforeStart(ivars_);
        if (queue_->committedEnd.load() != numSlots_) {
            return false;
        }

        // StartAudioStreaming: the transport publishes its geometry and arms
        // the consumer cursors.
        queue_->ResetConsumerForArm();
        queue_->abiVersion = ASFW::Isoch::kTxQueueAbiVersion;
        queue_->numSlots = numSlots_;
        queue_->slotStrideBytes = stride_;
        queue_->maxPacketBytes = stride_;
        queue_->interruptInterval = Geometry::kTxPacketsPerGroup;
        queue_->statusWord.store(IsochTxQueueStatus::kRunning);
        ivars_.runtime.txActive.store(true);
        return true;
    }

    // DICE-family TX replays the device's own timing: the RX consumer
    // publishes one replay entry per received packet and TX consumes them in
    // order. When enabled, the rig plays the RX side for a blocking 48 kHz
    // device -- the only mode the reference Saffire capture shows (host
    // packets are 8 or 296 bytes, device packets up to 552 = 8 x DBS 17).
    void FeedBlockingRx(uint32_t dataBlocksPerPacket) { rxBlocksPerPacket_ = dataBlocksPerPacket; }

    // Retires `packets` bus cycles of transmit, one interrupt per batch.
    // Returns false once the transport has stopped on a fault.
    bool RunPackets(uint64_t packets) {
        const uint64_t end = completed_ + packets;
        while (!fault_ && completed_ < end) {
            const uint64_t batch = std::min<uint64_t>(kPacketsPerInterrupt, end - completed_);
            Interrupt(static_cast<uint32_t>(batch));
        }
        return !fault_;
    }

    [[nodiscard]] const std::vector<WirePacket>& Wire() const { return wire_; }
    [[nodiscard]] const std::optional<TransportFault>& Fault() const { return fault_; }
    [[nodiscard]] const ASFWAudioDevice& Device() const { return *device_; }
    [[nodiscard]] const ASFW::Audio::Runtime::AudioTransportControlBlock& Control() const {
        return *control_;
    }
    [[nodiscard]] uint32_t Stride() const { return stride_; }

    [[nodiscard]] std::string DescribeFault() const {
        if (!fault_) {
            return "no transport fault";
        }
        std::string text = "transport stopped at packet " +
                           std::to_string(fault_->packetIndex) + ": " + fault_->reason;
        ASFW::Audio::Runtime::TxProducerFaultRecord record{};
        if (control_->txProducerFault.TryRead(record)) {
            text += std::string(" producer stage=") +
                    ASFW::Audio::Runtime::TxProducerFaultStageName(record.stage) +
                    " reason=" + ASFW::Audio::Runtime::TxProducerFaultReasonName(record.reason) +
                    " packet=" + std::to_string(record.packetIndex);
        }
        return text;
    }

private:
    void Interrupt(uint32_t delta) {
        // Load descriptors ahead of the hardware first, as Refill does: a slot
        // the producer has not committed is an IT FATAL.
        for (uint32_t i = 0; i < delta; ++i) {
            const uint64_t fill = completed_ + kDescriptorsAhead + i;
            if (fill < loadedEnd_) {
                continue;
            }
            const auto& meta = metadata_[fill % numSlots_];
            if (meta.commitGeneration.load() !=
                ASFW::Isoch::ExpectedTxCommitGeneration(fill, numSlots_)) {
                fault_ = TransportFault{fill, "slot-not-committed"};
                return;
            }
            loadedEnd_ = fill + 1;
        }

        if (rxBlocksPerPacket_ != 0) {
            ReceiveCycles(delta);
        }

        for (uint32_t i = 0; i < delta; ++i) {
            const uint64_t packet = completed_ + i;
            const auto& meta = metadata_[packet % numSlots_];
            const uint8_t* bytes = payload_.data() + (packet % numSlots_) * stride_;
            wire_.push_back({packet, std::vector<uint8_t>(bytes, bytes + meta.payloadLength)});
            queue_->PushCompletionStamp(packet, CycleTimerFor(packet, 0));
        }
        completed_ += delta;
        queue_->completionCursor.store(completed_);
        queue_->clockPair.Publish(
            {.hostTimeMid = HostTicksFor(completed_),
             .cycleTimer32 = CycleTimerFor(completed_, 0)});

        const uint64_t requested = queue_->refillRequestGeneration.load();
        if (requested == queue_->refillHandledGeneration.load()) {
            queue_->refillRequestGeneration.store(requested + 1);
            driver_.TxPreparationReady_Impl(nullptr, requested + 1);
        }

        if (queue_->statusWord.load() == IsochTxQueueStatus::kProducerFault) {
            fault_ = TransportFault{completed_, "producer-fault-status"};
        }
    }

    // What DirectAudioReceiveConsumer publishes per received packet. IEC
    // 61883-6 blocking at 48 kHz with SYT_INTERVAL 8: a packet's first frame
    // is 8n frames x 512 ticks after the stream origin, which lands DATA in
    // cycle phases 0/1/2 at offsets 0/1024/2048 and leaves phase 3 NO-DATA.
    void ReceiveCycles(uint32_t cycles) {
        auto& replay = control_->rxSequenceReplay;
        const uint32_t rxDelay = control_->rxTransferDelayTicks.load();
        for (uint32_t i = 0; i < cycles; ++i) {
            const uint64_t cycle = rxCycles_++;
            const uint32_t phase = static_cast<uint32_t>(cycle % 4);
            ASFW::Audio::Runtime::RxSequenceEntry entry{};
            entry.sourceCycleTimer = CycleTimerFor(cycle, 0);
            entry.dbc = rxDbc_;
            entry.flags = ASFW::Audio::Runtime::RxSequenceFlags::kValidCip;
            entry.firstAudioFrame = rxFrames_;
            if (phase != 3) {
                const uint64_t presentation = (kStartCycle + cycle) * 3072ULL + phase * 1024ULL + rxDelay;
                const auto syt = static_cast<uint16_t>((((presentation / 3072) & 0xF) << 12) |
                                                       (presentation % 3072));
                entry.dataBlocks = static_cast<uint16_t>(rxBlocksPerPacket_);
                entry.sytOffset = ASFW::Audio::Runtime::ComputeReplaySytOffset(
                    syt, entry.sourceCycleTimer, rxDelay);
                entry.flags |= ASFW::Audio::Runtime::RxSequenceFlags::kValidSyt;
                rxFrames_ += rxBlocksPerPacket_;
                rxDbc_ = static_cast<uint8_t>(rxDbc_ + rxBlocksPerPacket_);
            }
            replay.Publish(entry);
            if (!replay.IsEstablished()) {
                (void)replay.MarkEstablished();
            }
        }
    }

    // OHCI cycle timer: seconds[31:25] cycle[24:12] offset[11:0].
    [[nodiscard]] static uint32_t CycleTimerFor(uint64_t packet, uint32_t offset) {
        const uint64_t cycle = kStartCycle + packet;
        return static_cast<uint32_t>((((cycle / kCyclesPerSecond) & 0x7F) << 25) |
                                     ((cycle % kCyclesPerSecond) << 12) | offset);
    }

    // The host mach clock is 1:1 nanoseconds in the host mocks; one bus
    // cycle is 125 us.
    [[nodiscard]] static uint64_t HostTicksFor(uint64_t packet) {
        return kHostEpochNs + packet * 125'000ULL;
    }

    static constexpr uint64_t kStartCycle = 1208; // IT start seen on hardware.
    static constexpr uint64_t kHostEpochNs = 1'000'000'000ULL;

    ASFWAudioDriver driver_{};
    ASFWAudioDriver_IVars ivars_{};
    ASFWAudioNub nub_{};
    ASFWAudioDevice* device_{nullptr};
    std::unique_ptr<ASFW::Audio::Runtime::AudioTransportControlBlock> control_ =
        std::make_unique<ASFW::Audio::Runtime::AudioTransportControlBlock>();

    uint32_t numSlots_{0};
    uint32_t stride_{0};
    std::vector<uint8_t> payload_;
    std::unique_ptr<IsochTxPacketMeta[]> metadata_;
    std::unique_ptr<IsochTxQueueControl> queue_;

    uint32_t rxBlocksPerPacket_{0};
    uint64_t rxCycles_{0};
    uint64_t rxFrames_{0};
    uint8_t rxDbc_{0};

    uint64_t completed_{0};
    uint64_t loadedEnd_{0};
    std::vector<WirePacket> wire_;
    std::optional<TransportFault> fault_;
};

const Capture::Event* FirstCapturedHostPacket() {
    for (const auto& event : Capture::kEvents) {
        if (event.kind == Capture::EventKind::IsochPacket && event.dst == 0) {
            return &event;
        }
    }
    return nullptr;
}

// 0.5 s of bus time: several ring laps past the 912-packet prefill, and well
// past the ~780-cycle point where the FW-255 failure stopped IT.
constexpr uint64_t kSteadyStatePackets = 4000;

TEST(AudioDriverTxProducerTests, MAudio1814SurvivesSteadyStateAndMatchesCapture) {
    const auto* captured = FirstCapturedHostPacket();
    ASSERT_NE(captured, nullptr);

    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    TxProducerRig rig;
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::MAudioFireWire1814, 48000));
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();

    // The working capture's channel summary: every host packet, all 139087 of
    // them, is exactly the size of the first one (Smallest == Largest == 232).
    const auto& wire = rig.Wire();
    ASSERT_EQ(wire.size(), kSteadyStatePackets);
    for (const auto& packet : wire) {
        ASSERT_EQ(packet.bytes.size(), captured->payloadSize) << "packet " << packet.index;
    }

    // The first packet on the wire is byte-for-byte the captured one.
    EXPECT_EQ(wire.front().bytes,
              std::vector<uint8_t>(captured->payload, captured->payload + captured->payloadSize));
}

TEST(AudioDriverTxProducerTests, MAudio1814KeepsCadenceDbcAndSytOnceDataFlows) {
    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    TxProducerRig rig;
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::MAudioFireWire1814, 48000));
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();

    const auto& wire = rig.Wire();
    // Blocking 48 kHz with SYT interval 8: every packet carries eight blocks
    // (cadence NO-DATA included), so DBC advances by 8 on every packet.
    for (size_t i = 1; i < wire.size(); ++i) {
        ASSERT_EQ(static_cast<uint8_t>(wire[i].Dbc() - wire[i - 1].Dbc()), 8U)
            << "packet " << wire[i].index;
    }

    size_t firstData = wire.size();
    for (size_t i = 0; i < wire.size(); ++i) {
        if (wire[i].IsData()) {
            firstData = i;
            break;
        }
    }
    ASSERT_LT(firstData, wire.size()) << "no DATA packet in " << wire.size() << " cycles";

    // From the first DATA packet on, the vendor cadence is three DATA packets
    // then one NO-DATA, anchored at cycle zero of the cadence.
    for (size_t i = firstData; i < wire.size(); ++i) {
        const bool expectData = (wire[i].index % 4) != 3;
        ASSERT_EQ(wire[i].IsData(), expectData) << "packet " << wire[i].index;
    }
}

TEST(AudioDriverTxProducerTests, MAudio1814PublishesTheHalClockFromTxCompletions) {
    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    TxProducerRig rig;
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::MAudioFireWire1814, 48000));
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();

    // StartIO waits on this: M-Audio's HAL clock comes from qualified TX
    // completion stamps, not from RX. Without it StartIO times out.
    const auto& published = rig.Device().published;
    ASSERT_FALSE(published.empty());
    for (size_t i = 1; i < published.size(); ++i) {
        EXPECT_EQ(published[i].sampleTime - published[i - 1].sampleTime,
                  Geometry::kHalZeroTimestampPeriodFrames);
        EXPECT_GT(published[i].hostTime, published[i - 1].hostTime);
    }
}

// The reference Saffire Pro 24 DSP capture (tools/pydice/ref-full.txt, the
// same trace ReferencePhase0ParityFixture is exported from) is blocking
// 48 kHz: host channel 0 packets are 8 bytes (header only) or 296 bytes
// (8 blocks x DBS 9) over 45550 packets with 0 silent cycles. Its first host
// packet is 296 bytes with SYT=NO_INFO, so block count -- not SYT -- is what
// tells the two sizes apart.
constexpr uint32_t kSaffireDbs = 9;
constexpr size_t kSaffireHeaderOnlyBytes = 8;
constexpr size_t kSaffireFullBytes = 8 + 8 * kSaffireDbs * 4;

uint8_t BlocksIn(const WirePacket& packet, uint32_t dbs) {
    return static_cast<uint8_t>((packet.bytes.size() - 8) / (dbs * 4));
}

TEST(AudioDriverTxProducerTests, SaffireReplaysRxTimingOnceReplayEstablishes) {
    ASFW::Isoch::Audio::DICE::Profiles::FocusriteSaffireProfile profile;
    TxProducerRig rig;
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::FocusriteSPro24Dsp, 48000));
    rig.FeedBlockingRx(8);
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();

    const auto& wire = rig.Wire();
    ASSERT_EQ(wire.size(), kSteadyStatePackets);
    size_t dataPackets = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
        const auto& packet = wire[i];
        ASSERT_TRUE(packet.bytes.size() == kSaffireHeaderOnlyBytes ||
                    packet.bytes.size() == kSaffireFullBytes)
            << "packet " << packet.index << " is " << packet.bytes.size() << " bytes";
        if (packet.IsData()) {
            ASSERT_EQ(packet.bytes.size(), kSaffireFullBytes) << "packet " << packet.index;
            ++dataPackets;
        }
        // IEC 61883-1: DBC advances by the blocks the previous packet carried.
        if (i > 0) {
            ASSERT_EQ(static_cast<uint8_t>(packet.Dbc() - wire[i - 1].Dbc()),
                      BlocksIn(wire[i - 1], kSaffireDbs))
                << "packet " << packet.index;
        }
    }
    // Replay establishes a few hundred cycles in; after that TX carries the
    // device's 3-of-4 DATA cadence.
    EXPECT_GT(dataPackets, kSteadyStatePackets / 2);
}

} // namespace
