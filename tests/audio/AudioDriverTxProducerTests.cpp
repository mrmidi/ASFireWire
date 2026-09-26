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
#include "Audio/DriverKit/Config/DICE/DiceProfile.hpp"
#include "Audio/DriverKit/Config/ResolvedStreamConfig.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Isoch/Core/IsochTxQueue.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include "ASFWAudioDevice.h"

#include "../support/MAudioSpecialHappyPathFixture.inc"
#include "WireTrace.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
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

// A sample that names itself: tag = 1 + (frame mod 32768) * 16 + channel,
// stored as tag / (2^23 - 1). PcmSlotCodec scales by exactly 2^23 - 1, so the
// 24-bit slot decodes back to the tag, and 0 means nothing was written.
constexpr uint32_t kTagFrameModulo = 32768;
constexpr float kScale24 = 8388607.0f;

float SampleTag(uint64_t frame, uint32_t channel) {
    const uint32_t tag = 1 + static_cast<uint32_t>(frame % kTagFrameModulo) * 16 + channel;
    return static_cast<float>(tag) / kScale24;
}

// Owns everything StartIO would own for the primary playback stream, and
// plays the transport's part of the shared-queue contract.
class TxProducerRig final {
public:
    TxProducerRig() {
        device_ = new ASFWAudioDevice();
        device_->zeroTimestampPeriod = ASFW::IsochTransport::HalBufferProfileForRate(48000).zeroTimestampPeriodFrames;
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
    // One playback stream as the nub publishes it after resolution.
    void SetResolvedPlayback(const ASFW::Isoch::Audio::ParsedWireStream& stream) {
        ivars_.device.playbackStreams[0] = stream;
        ivars_.device.playbackStreamCount = 1;
    }

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

        // As StartIO frames stream 0: the device's resolved geometry when the
        // nub carried it, otherwise the profile's own.
        ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
        if (!ASFW::Isoch::Audio::BuildResolvedTxStreamConfig(
                profile, ivars_.device.playbackStreams, ivars_.device.playbackStreamCount, 0,
                txConfig)) {
            return false;
        }
        txConfig.sampleRate = sampleRateHz;
        sampleRate_ = sampleRateHz;
        pcmChannels_ = txConfig.pcmChannels;
        dbs_ = txConfig.dbs;

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

    // CoreAudio's side of playback. Each IO cycle CoreAudio fills its output
    // ring for [sampleTime, sampleTime + ioFrames) and calls WriteEnd; this
    // plays that through the driver's real WriteEnd step
    // (HandleOutputWriteEnd). Sample time runs on the device frame clock: at
    // bus packet p the device is at frame p * rate / 8000, and CoreAudio
    // writes up to leadFrames ahead of it, checking at every packet time. No IO runs for packets in
    // [stallFrom, stallTo); afterwards CoreAudio resumes at the current time,
    // as the HAL does after an overload, so the frames in between are never
    // written.
    //
    // Every sample carries its own identity (see SampleTag), so the wire
    // shows which frame landed in which packet.
    struct HostOutputPlan final {
        uint32_t ioFrames{512};
        uint32_t leadFrames{560};
        uint64_t stallFrom{0};
        uint64_t stallTo{0};
        // True where the HAL clock comes from published zero timestamps
        // (M-Audio: TX completions); IO waits for the first one.
        bool clockFromZts{false};
    };

    void EnableHostOutput(const HostOutputPlan& plan) {
        hostPlan_ = plan;
        clockFromZts_ = plan.clockFromZts;
        ringFrames_ = ASFW::IsochTransport::HalBufferProfileForRate(sampleRate_)
                          .zeroTimestampPeriodFrames;
        outputRing_.assign(static_cast<size_t>(ringFrames_) * pcmChannels_, 0.0f);
        auto& memory = ivars_.runtime.directAudioGraph.memory;
        memory.outputBase = outputRing_.data();
        memory.outputFrameCapacity = ringFrames_;
        memory.outputChannels = pcmChannels_;
    }

    [[nodiscard]] uint32_t SampleRate() const { return sampleRate_; }
    [[nodiscard]] uint32_t PcmChannels() const { return pcmChannels_; }
    [[nodiscard]] uint32_t Dbs() const { return dbs_; }
    [[nodiscard]] uint64_t WriteEnds() const { return writeEnds_; }

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
    [[nodiscard]] ASFWAudioDriver_IVars& Ivars() { return ivars_; }
    [[nodiscard]] ASFW::Audio::Runtime::AudioTransportControlBlock& MutableControl() {
        return *control_;
    }

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
            // CoreAudio wakes on its own clock, not on our interrupt: give it
            // every packet time, after that packet has left.
            DriveHostWrites(packet + 1);
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

    // CoreAudio's sample time "now", as the HAL derives it from the latest
    // zero timestamp. Where the rig publishes none (the DICE cases feed RX
    // replay directly and run no RX consumer), the device frame clock stands
    // in: packet * rate / 8000, the frame the RX timeline would project.
    [[nodiscard]] bool NowSampleTime(uint64_t atPacket, uint64_t& now) const {
        const auto& published = device_->published;
        if (!published.empty()) {
            const auto& anchor = published.back();
            const uint64_t host = HostTicksFor(atPacket);
            if (host < anchor.hostTime) {
                return false;
            }
            now = anchor.sampleTime + (host - anchor.hostTime) * sampleRate_ / 1'000'000'000ULL;
            return true;
        }
        if (clockFromZts_) {
            return false;
        }
        now = atPacket * sampleRate_ / kCyclesPerSecond;
        return true;
    }

    void DriveHostWrites(uint64_t atPacket) {
        if (!hostPlan_) {
            return;
        }
        const auto& plan = *hostPlan_;
        if (atPacket >= plan.stallFrom && atPacket < plan.stallTo) {
            stalled_ = true;
            return;
        }
        uint64_t now = 0;
        if (!NowSampleTime(atPacket, now)) {
            return;  // the HAL has no clock yet, so CoreAudio does no IO
        }
        if (stalled_) {
            stalled_ = false;
            if (nextWrite_ < now) {
                nextWrite_ = (now / plan.ioFrames + 1) * plan.ioFrames;
            }
        }
        while (nextWrite_ + plan.ioFrames <= now + plan.leadFrames) {
            for (uint64_t f = nextWrite_; f < nextWrite_ + plan.ioFrames; ++f) {
                float* frame = outputRing_.data() + (f % ringFrames_) * pcmChannels_;
                for (uint32_t ch = 0; ch < pcmChannels_; ++ch) {
                    frame[ch] = SampleTag(f, ch);
                }
            }
            EXPECT_TRUE(ASFW::Audio::DriverKit::HandleOutputWriteEnd(
                ivars_, *control_, nextWrite_, HostTicksFor(atPacket), plan.ioFrames));
            ++writeEnds_;
            nextWrite_ += plan.ioFrames;
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

    uint32_t sampleRate_{0};
    uint32_t pcmChannels_{0};
    uint32_t dbs_{0};

    std::optional<HostOutputPlan> hostPlan_;
    std::vector<float> outputRing_;
    uint32_t ringFrames_{0};
    uint64_t nextWrite_{0};
    uint64_t writeEnds_{0};
    bool stalled_{false};
    bool clockFromZts_{false};

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

// 0.5 s of bus time: two ring laps past the 1696-packet prefill, and well
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
                  ASFW::IsochTransport::HalBufferProfileForRate(48000).zeroTimestampPeriodFrames);
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

// Epic 4: the one HAL publish point refuses an anchor projected in a timeline
// epoch that has since ended, and passes one from the live epoch.
TEST(AudioDriverTxProducerTests, HalPublishRefusesAnAnchorFromAnEndedEpoch) {
    using ASFW::Audio::Runtime::HardwareTimelineDiscontinuity;
    using ASFW::Audio::Runtime::HardwareTimelineSource;
    using ASFW::Audio::Runtime::ZtsMirrorPublishResult;
    TxProducerRig rig;
    auto& control = rig.MutableControl();
    const uint64_t first = control.hardwareTimeline.BeginEpoch(
        HardwareTimelineSource::Receive, HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    ASSERT_NE(first, 0U);
    ASSERT_TRUE(control.PublishHostClockAnchor(12288, 1000, 5333333, first).accepted);
    EXPECT_EQ(ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(rig.Ivars(), "test", false),
              ZtsMirrorPublishResult::Published);
    ASSERT_EQ(rig.Device().published.size(), 1U);

    const uint64_t second = control.hardwareTimeline.BeginEpoch(
        HardwareTimelineSource::Receive, HardwareTimelineDiscontinuity::PresentationLoss, 48000,
        24576);
    ASSERT_NE(second, first);
    ASSERT_TRUE(control.PublishHostClockAnchor(24576, 2000, 5333333, first).accepted);
    EXPECT_EQ(ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(rig.Ivars(), "test", false),
              ZtsMirrorPublishResult::StaleEpoch);
    EXPECT_EQ(rig.Device().published.size(), 1U);

    ASSERT_TRUE(control.PublishHostClockAnchor(24576, 2100, 5333333, second).accepted);
    EXPECT_EQ(ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(rig.Ivars(), "test", false),
              ZtsMirrorPublishResult::Published);
    EXPECT_EQ(rig.Device().published.size(), 2U);
}

TEST(AudioDriverTxProducerTests, SaffireReplaysRxTimingOnceReplayEstablishes) {
    // The registry's Saffire entry: the default spec, named.
    const ASFW::Isoch::Audio::DICE::DiceProfile profile{{.name = "Focusrite Saffire (DICE)"}};
    TxProducerRig rig;
    // The DICE profile states no geometry; the device reports one playback
    // stream of 8 PCM + 1 MIDI (DBS 9), and it crosses the nub resolved.
    rig.SetResolvedPlayback({.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1});
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
    // Packets prepared before replay establishes carry no DATA, so the first
    // DATA packet follows the prefilled shared store (prepare-time content; late
    // binding, FW-209, removes this start-up gap). After that TX carries the
    // device's 3-of-4 DATA cadence.
    size_t firstData = wire.size();
    for (size_t i = 0; i < wire.size(); ++i) {
        if (wire[i].IsData()) {
            firstData = i;
            break;
        }
    }
    ASSERT_LT(firstData, wire.size());
    EXPECT_LE(firstData, Geometry::kTxSharedSlotPackets);
    EXPECT_GE(dataPackets, (wire.size() - firstData) * 3 / 4 - 1);
}

// ---------------------------------------------------------------------------
// TX ownership goldens (milestone 6, T1; documentation/TX_OWNERSHIP.md)
//
// Records, before milestone 6 changes anything, which CoreAudio frames reach
// which packets on the wire. Each run of packets becomes one line:
//
//   NODATA          no data blocks (SYT 0xFFFF)
//   PCM             every block carries a written frame, consecutive
//   SILENT          DATA packets whose blocks were never written
//   MIXED           written and unwritten blocks in one packet, or a torn one
//
// `offset` is the run's first frame minus the device frame at the packet's
// transmit time (packet * rate / 8000): where output frames land relative to
// transmission. NODATA packets inside a PCM or SILENT run are counted there.

struct PacketView final {
    enum class Kind { NoData, Pcm, Silent, Mixed } kind{Kind::NoData};
    uint32_t blocks{0};
    int64_t firstFrame{-1};
    int64_t lastFrame{-1};
    uint32_t silentBlocks{0};
    uint8_t label{0};
};

PacketView ViewPacket(const WirePacket& packet, uint32_t dbs, uint32_t pcm) {
    PacketView v{};
    if (packet.bytes.size() <= 8 || !packet.IsData()) {
        return v;
    }
    v.blocks = static_cast<uint32_t>((packet.bytes.size() - 8) / (dbs * 4));
    v.label = packet.bytes[8];
    bool torn = false;
    int64_t expected = -1;
    for (uint32_t b = 0; b < v.blocks; ++b) {
        int64_t blockFrame = -1;
        uint32_t written = 0;
        for (uint32_t slot = 0; slot < pcm; ++slot) {
            const uint32_t q = packet.Quadlet(8 + (static_cast<size_t>(b) * dbs + slot) * 4);
            int32_t value = static_cast<int32_t>(q & 0x00FFFFFFu);
            if (value & 0x00800000) {
                value -= 0x01000000;
            }
            if (value <= 0) {
                continue;
            }
            ++written;
            const int64_t frame = (value - 1) / 16;
            if (blockFrame < 0) {
                blockFrame = frame;
            } else if (frame != blockFrame) {
                torn = true;
            }
        }
        if (written == 0) {
            ++v.silentBlocks;
            continue;
        }
        if (written != pcm) {
            torn = true;
        }
        if (v.firstFrame < 0) {
            v.firstFrame = blockFrame;
        } else if (blockFrame != expected) {
            torn = true;
        }
        v.lastFrame = blockFrame;
        expected = (blockFrame + 1) % kTagFrameModulo;
    }
    if (v.silentBlocks == v.blocks) {
        v.kind = PacketView::Kind::Silent;
    } else if (v.silentBlocks != 0 || torn) {
        v.kind = PacketView::Kind::Mixed;
    } else {
        v.kind = PacketView::Kind::Pcm;
    }
    return v;
}

ASFW::Testing::WireTrace DescribeTxWire(const std::vector<WirePacket>& wire,
                                        const TxProducerRig& rig) {
    using Kind = PacketView::Kind;
    ASFW::Testing::WireTrace trace;
    struct Run {
        Kind kind{Kind::NoData};
        uint64_t firstPacket{0}, lastPacket{0};
        int64_t firstFrame{-1}, lastFrame{-1};
        uint64_t data{0}, noData{0};
        uint8_t label{0};
        bool open{false};
    } run;
    char line[192];
    const auto flush = [&] {
        if (!run.open) {
            return;
        }
        const auto deviceFrame = static_cast<int64_t>(run.firstPacket * rig.SampleRate() /
                                                      kCyclesPerSecond);
        switch (run.kind) {
        case Kind::NoData:
            std::snprintf(line, sizeof line, "pkts %llu-%llu NODATA x%llu",
                          (unsigned long long)run.firstPacket, (unsigned long long)run.lastPacket,
                          (unsigned long long)run.noData);
            break;
        case Kind::Pcm:
            std::snprintf(line, sizeof line,
                          "pkts %llu-%llu PCM frames %lld-%lld data=%llu nodata=%llu label=0x%02x offset=%+lld",
                          (unsigned long long)run.firstPacket, (unsigned long long)run.lastPacket,
                          (long long)run.firstFrame, (long long)run.lastFrame,
                          (unsigned long long)run.data, (unsigned long long)run.noData, run.label,
                          (long long)(run.firstFrame - deviceFrame));
            break;
        case Kind::Silent:
            std::snprintf(line, sizeof line, "pkts %llu-%llu SILENT data=%llu nodata=%llu label=0x%02x",
                          (unsigned long long)run.firstPacket, (unsigned long long)run.lastPacket,
                          (unsigned long long)run.data, (unsigned long long)run.noData, run.label);
            break;
        case Kind::Mixed:
            std::snprintf(line, sizeof line, "pkts %llu-%llu MIXED frames %lld-%lld label=0x%02x",
                          (unsigned long long)run.firstPacket, (unsigned long long)run.lastPacket,
                          (long long)run.firstFrame, (long long)run.lastFrame, run.label);
            break;
        }
        trace.Add(line);
        run.open = false;
    };
    for (const auto& packet : wire) {
        const PacketView v = ViewPacket(packet, rig.Dbs(), rig.PcmChannels());
        if (v.kind == Kind::NoData) {
            if (run.open && run.kind != Kind::Mixed) {
                ++run.noData;
                run.lastPacket = packet.index;
                continue;
            }
            flush();
            run = Run{Kind::NoData, packet.index, packet.index, -1, -1, 0, 1, 0, true};
            continue;
        }
        const bool continues =
            run.open && run.kind == v.kind && run.label == v.label &&
            (v.kind == Kind::Silent ||
             (v.kind == Kind::Pcm && v.firstFrame == (run.lastFrame + 1) % kTagFrameModulo));
        if (continues) {
            ++run.data;
            run.lastPacket = packet.index;
            if (v.kind == Kind::Pcm) {
                run.lastFrame = v.lastFrame;
            }
            continue;
        }
        flush();
        run = Run{v.kind, packet.index, packet.index, v.firstFrame, v.lastFrame, 1, 0, v.label,
                  true};
    }
    flush();
    return trace;
}

struct TxGoldenCase final {
    const char* golden;
    TxProducerRig::HostOutputPlan plan;
};

void ExpectTxGolden(const TxProducerRig& rig, const TxGoldenCase& c) {
    ASFW::Testing::WireTrace trace;
    char line[160];
    std::snprintf(line, sizeof line,
                  "case rate=%u io=%u lead=%u stall=%llu-%llu clock=%s writeEnds=%llu",
                  rig.SampleRate(), c.plan.ioFrames, c.plan.leadFrames,
                  (unsigned long long)c.plan.stallFrom, (unsigned long long)c.plan.stallTo,
                  c.plan.clockFromZts ? "zts" : "device-frame", (unsigned long long)rig.WriteEnds());
    trace.Add(line);
    for (const auto& l : DescribeTxWire(rig.Wire(), rig).Lines()) {
        trace.Add(l);
    }
    ASFW::Testing::ExpectMatchesGolden(trace, c.golden);
}

void RecordSaffireTxGolden(const TxGoldenCase& c) {
    const ASFW::Isoch::Audio::DICE::DiceProfile profile{{.name = "Focusrite Saffire (DICE)"}};
    TxProducerRig rig;
    rig.SetResolvedPlayback({.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1});
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::FocusriteSPro24Dsp, 48000));
    rig.FeedBlockingRx(8);
    rig.EnableHostOutput(c.plan);
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();
    ASSERT_GT(rig.WriteEnds(), 0U);
    ExpectTxGolden(rig, c);
}

void RecordMAudioTxGolden(const TxGoldenCase& c) {
    ASFW::Isoch::Audio::AVC::Profiles::MAudioSpecialProfile profile(false);
    TxProducerRig rig;
    ASSERT_TRUE(rig.Start(profile, ProfileBuilderId::MAudioFireWire1814, 48000));
    rig.EnableHostOutput(c.plan);
    ASSERT_TRUE(rig.RunPackets(kSteadyStatePackets)) << rig.DescribeFault();
    ASSERT_GT(rig.WriteEnds(), 0U);
    ExpectTxGolden(rig, c);
}

// CoreAudio writes 512-frame buffers well ahead of the device.
TEST(TxOwnershipGolden, SaffireClean48k) {
    RecordSaffireTxGolden({"tx/saffire/clean-48k-io512.txt", {.ioFrames = 512, .leadFrames = 560}});
}

// A small buffer: 64 frames, written 48 safety frames ahead.
TEST(TxOwnershipGolden, SaffireSmallBuffer48k) {
    RecordSaffireTxGolden({"tx/saffire/clean-48k-io64.txt", {.ioFrames = 64, .leadFrames = 112}});
}

// CoreAudio writes only frames the device has already reached.
TEST(TxOwnershipGolden, SaffireLateWriter48k) {
    RecordSaffireTxGolden({"tx/saffire/late-writer-48k-io64.txt", {.ioFrames = 64, .leadFrames = 0}});
}

// 50 ms without IO (packets 2000-2400), then CoreAudio resumes at the
// current time.
TEST(TxOwnershipGolden, SaffireStall48k) {
    RecordSaffireTxGolden({"tx/saffire/stall-48k-io512.txt",
                           {.ioFrames = 512, .leadFrames = 560, .stallFrom = 2000, .stallTo = 2400}});
}

TEST(TxOwnershipGolden, MAudio1814Clean48k) {
    RecordMAudioTxGolden({"tx/maudio-1814/clean-48k-io512.txt",
                          {.ioFrames = 512, .leadFrames = 560, .clockFromZts = true}});
}

} // namespace
