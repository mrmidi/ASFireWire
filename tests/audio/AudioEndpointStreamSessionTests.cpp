// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Engine/AudioEndpointStreamSession.hpp"
#include "ASFWDriver/Audio/Duplex/IAudioDuplexStreamControl.hpp"
#include "ASFWDriver/Audio/Duplex/IsochDuplexHostTransport.hpp"
#include "ASFWDriver/Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "ASFWDriver/Audio/Ports/ITxPcmSource.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Isoch/IsochService.hpp"
#include "ASFWDriver/Midi/Transport/MidiTransportBlock.hpp"
#include "ASFWDriver/Audio/Wire/AM824/MpxMidiDemux.hpp"

#include "ASFWDriver/Audio/Runtime/PcmPublicationCache.hpp"

namespace {

using ASFW::Audio::AudioEndpointStreamSession;
using ASFW::Audio::IAudioDuplexStreamControl;
using ASFW::Audio::IIsochDuplexHostTransport;
using ASFW::Audio::Devices::AudioEndpointId;
using ASFW::Audio::Devices::ResolvedAudioEndpointProfile;
using ASFW::Audio::Ports::ITxPcmSource;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::DirectAudioBindingSnapshot;
using ASFW::Audio::Runtime::IDirectAudioBindingSource;
using ASFW::Audio::Runtime::PcmPublicationCache;
using ASFW::Encoding::MpxMidiGeometry;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::IsochService;
using ASFW::Midi::MidiTransportBlock;
using ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition;
using ASFW::Protocols::Audio::AMDTP::TxPresentationPlan;
using ASFW::Protocols::Audio::DICE::TxSlotPrepareResult;
using ASFW::Protocols::Audio::DICE::TxSlotFillResult;

class MockStreamControl final : public IAudioDuplexStreamControl {
public:
    uint32_t startCalls{0};
    uint32_t stopCalls{0};
    IOReturn startResult{kIOReturnSuccess};
    IOReturn stopResult{kIOReturnSuccess};

    [[nodiscard]] IOReturn StartStreaming(AudioEndpointId) noexcept override {
        ++startCalls;
        return startResult;
    }

    [[nodiscard]] IOReturn StopStreaming(AudioEndpointId) noexcept override {
        ++stopCalls;
        return stopResult;
    }
};

class MockHostTransport final : public IIsochDuplexHostTransport {
public:
    uint32_t clockAnchorCalls{0};

    kern_return_t BeginSplitDuplex(EndpointId) noexcept override { return kIOReturnSuccess; }
    kern_return_t ReservePlaybackResources(EndpointId, ::ASFW::IRM::IRMClient&, uint64_t, uint32_t, uint8_t& outChannel) noexcept override {
        outChannel = 0;
        return kIOReturnSuccess;
    }
    kern_return_t ReserveCaptureResources(EndpointId, ::ASFW::IRM::IRMClient&, uint64_t, uint32_t, uint8_t& outChannel) noexcept override {
        outChannel = 1;
        return kIOReturnSuccess;
    }
    kern_return_t PrepareReceive(uint8_t, HardwareInterface&, IDirectAudioBindingSource*,
                                 ASFW::Encoding::AudioWireFormat, uint32_t, uint32_t,
                                 bool, bool, const ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap&) noexcept override {
        return kIOReturnSuccess;
    }
    kern_return_t PrepareTransmit(uint8_t, HardwareInterface&, uint8_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareReceiveStream(uint32_t, uint8_t, HardwareInterface&, IDirectAudioBindingSource*, uint32_t, uint32_t,
                                       ASFW::Encoding::AudioWireFormat, uint32_t) noexcept override {
        return kIOReturnSuccess;
    }
    kern_return_t PrepareTransmitStream(uint32_t, uint8_t, HardwareInterface&, uint8_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedReceive() noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedReceiveAtCycle(uint32_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedTransmit() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopAll() noexcept override { return kIOReturnSuccess; }

    void NotifyClockAnchorReady(uint64_t) noexcept override {
        ++clockAnchorCalls;
    }
};

class FakeBindingSource final : public IDirectAudioBindingSource {
public:
    AudioTransportControlBlock controlBlock{};
    float dummyInput[512 * 8]{0};
    float dummyOutput[512 * 8]{0};

    FakeBindingSource() = default;

    bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept override {
        out.endpointId = AudioEndpointId{1};
        out.generation = 1;
        out.valid = true;
        out.inputBase = dummyInput;
        out.inputFrames = 512;
        out.inputChannels = 8;
        out.outputBase = dummyOutput;
        out.outputFrames = 512;
        out.outputChannels = 8;
        out.control = &controlBlock;
        out.sampleRateHz = 48000;
        return true;
    }
};

ResolvedAudioEndpointProfile MakeTestProfile() {
    ResolvedAudioEndpointProfile profile{};
    profile.endpointId = AudioEndpointId{1};
    profile.deviceInstanceId = ASFW::Discovery::DeviceInstanceId{1};
    profile.supportedRates = {48000};
    profile.supportedRateCount = 1;
    profile.currentSampleRateHz = 48000;
    profile.runtimeCaps.sampleRateHz = 48000;
    profile.runtimeCaps.hostToDeviceStreamCount = 1;
    profile.runtimeCaps.deviceToHostStreamCount = 1;
    // 6 audio channels + 1 MIDI slot = 7 AM824 slots, 1 MIDI port
    profile.runtimeCaps.hostToDeviceStreams[0] = {0, 6, 7, 1};
    profile.runtimeCaps.deviceToHostStreams[0] = {1, 6, 7, 1};
    return profile;
}

class AudioEndpointStreamSessionTest : public ::testing::Test {
protected:
    MockStreamControl streamControl_;
    MockHostTransport hostTransport_;
    FakeBindingSource bindingSource_;
    PcmPublicationCache pcmSource_;
    IsochService isoch_;
    HardwareInterface hardware_;
    ResolvedAudioEndpointProfile profile_{MakeTestProfile()};
    MidiTransportBlock midiBlock_{};

    void SetUp() override {
        midiBlock_.Arm(1);
        (void)pcmSource_.Configure(6, 8192);
        pcmSource_.BeginEpoch(1);
    }
};

TEST_F(AudioEndpointStreamSessionTest, InitialStateIsCleanAndUnleased) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    EXPECT_FALSE(session.HasLease());
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_FALSE(session.MidiLeaseActive());
    EXPECT_FALSE(session.IsStreaming());
}

TEST_F(AudioEndpointStreamSessionTest, StandaloneMidiLeaseStartsSession) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };

    EXPECT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.startCalls, 1U);
    EXPECT_TRUE(session.IsStreaming());
    EXPECT_TRUE(session.MidiLeaseActive());
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_TRUE(session.HasLease());
}

TEST_F(AudioEndpointStreamSessionTest, MidiOnlyFillComposesMidiIntoSilenceWithoutPcmSource) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };

    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);

    // Push MIDI byte 0x90 into port 0
    const uint8_t byteRun[] = {0x90};
    EXPECT_TRUE((midiBlock_.hostToDevice[0].TryWrite(byteRun, 0u)));

    // Prefill filled numSlots (1512) with NO-DATA packets.
    // Advance completionCursor to 1512 so packet 1512 (which maps to ring slot 0) can be acquired.
    const uint32_t nextPacket = session.TxSlotProvider().numSlots;
    session.TxSlotProvider().queueControl->completionCursor.store(nextPacket, std::memory_order_release);

    auto& txEngine = session.TxStreamEngine();
    TxPresentationPlan plan{
        .epoch = 1,
        .cycleOrdinal = nextPacket,
        .firstAudioFrame = 0,
        .frameCount = 8,
        .presentationBusTicks = 1'000'000,
        .disposition = AmdtpPacketDisposition::Data,
    };

    EXPECT_EQ(txEngine.PrepareTransmitSlot(nextPacket, plan, 8, 0x1000), TxSlotPrepareResult::Prepared);

    // Fill slot without PCM source (standalone MIDI mode)
    EXPECT_EQ(txEngine.FillTransmitSlot(nextPacket), TxSlotFillResult::Filled);
    EXPECT_TRUE(txEngine.CommitFill(nextPacket));

    // Verify late image has AM824 silence for channel 0 and MPX-MIDI for channel 6.
    // Slot 64 maps to ring index (64 % 64) = 0.
    auto& slotProvider = session.TxSlotProvider();
    const uint32_t stride = slotProvider.slotStrideBytes;
    const uint8_t* payload = slotProvider.payloadBase +
        ASFW::Isoch::TxPayloadImageOffset(0, 1, stride);

    const uint32_t* quadlets = reinterpret_cast<const uint32_t*>(payload + 8); // after CIP header
    const uint32_t pcmQuad = OSSwapBigToHostInt32(quadlets[0]);
    EXPECT_EQ(pcmQuad, 0x40000000U); // AM824 silence

    const uint32_t midiQuad = OSSwapBigToHostInt32(quadlets[6]);
    EXPECT_EQ(midiQuad & 0xFF000000U, 0x81000000U); // MPX-MIDI label (0x81 = 1 byte)
    EXPECT_EQ((midiQuad >> 16) & 0xFFU, 0x90U);     // 0x90 MIDI byte
}

TEST_F(AudioEndpointStreamSessionTest, DynamicAudioLeaseAcquisitionAndRelease) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };

    // 1. Standalone MIDI starts stream
    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.startCalls, 1U);
    EXPECT_TRUE(session.IsStreaming());

    // 2. CoreAudio opens: dynamic audio lease acquisition does NOT restart hardware
    EXPECT_EQ(session.AcquireAudioLease(&pcmSource_), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.startCalls, 1U); // No second start!
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_TRUE(session.MidiLeaseActive());
    EXPECT_TRUE(session.IsStreaming());

    // 3. CoreAudio closes: releasing audio lease does NOT stop hardware
    EXPECT_EQ(session.ReleaseAudioLease(), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.stopCalls, 0U); // Transport still active!
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_TRUE(session.MidiLeaseActive());
    EXPECT_TRUE(session.IsStreaming());

    // 4. MIDI stops: last lease release stops hardware
    EXPECT_EQ(session.ReleaseMidiLease(), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.stopCalls, 1U);
    EXPECT_FALSE(session.IsStreaming());
    EXPECT_FALSE(session.HasLease());
}

TEST_F(AudioEndpointStreamSessionTest, AudioFirstThenMidiThenReleaseAudio) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };

    // 1. Audio lease starts streaming
    EXPECT_EQ(session.AcquireAudioLease(&pcmSource_), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.startCalls, 1U);
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_TRUE(session.IsStreaming());

    // 2. MIDI lease added dynamically
    EXPECT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.startCalls, 1U);
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_TRUE(session.MidiLeaseActive());

    // 3. Audio lease released while MIDI remains active
    EXPECT_EQ(session.ReleaseAudioLease(), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.stopCalls, 0U);
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_TRUE(session.MidiLeaseActive());
    EXPECT_TRUE(session.IsStreaming());

    // 4. MIDI lease released stops streaming
    EXPECT_EQ(session.ReleaseMidiLease(), kIOReturnSuccess);
    EXPECT_EQ(streamControl_.stopCalls, 1U);
    EXPECT_FALSE(session.IsStreaming());
}

TEST_F(AudioEndpointStreamSessionTest, FailedStartRollsBackCleanly) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    streamControl_.startResult = kIOReturnNotReady;

    // Audio lease start failure rolls back
    EXPECT_EQ(session.AcquireAudioLease(&pcmSource_), kIOReturnNotReady);
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_FALSE(session.HasLease());
    EXPECT_FALSE(session.IsStreaming());

    // MIDI lease start failure rolls back
    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    EXPECT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnNotReady);
    EXPECT_FALSE(session.MidiLeaseActive());
    EXPECT_FALSE(session.HasLease());
    EXPECT_FALSE(session.IsStreaming());
}

TEST_F(AudioEndpointStreamSessionTest, MidiOnlyStartupInitializesTimeline) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    // Control block starts with epoch == 0
    EXPECT_EQ(bindingSource_.controlBlock.hardwareTimeline.Epoch(), 0ULL);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    EXPECT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_TRUE(session.IsStreaming());

    // Hardware timeline was initialized by session start
    const uint64_t epoch = bindingSource_.controlBlock.hardwareTimeline.Epoch();
    EXPECT_NE(epoch, 0ULL);
    EXPECT_GT(bindingSource_.controlBlock.rxTransferDelayTicks.load(), 0U);
    EXPECT_GT(bindingSource_.controlBlock.txTransferDelayTicks.load(), 0U);
}

TEST_F(AudioEndpointStreamSessionTest, StopStreamingFailurePreservesTxResources) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    EXPECT_EQ(session.AcquireAudioLease(&pcmSource_), kIOReturnSuccess);
    EXPECT_TRUE(session.IsStreaming());
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_NE(session.TxSlotProvider().payloadBase, nullptr);

    // StopStreaming fails with timeout
    streamControl_.stopResult = kIOReturnTimeout;

    EXPECT_EQ(session.ReleaseAudioLease(), kIOReturnTimeout);
    // Failure preserved: session is still streaming, lease active, resources not freed
    EXPECT_TRUE(session.IsStreaming());
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_NE(session.TxSlotProvider().payloadBase, nullptr);

    // After coordinator recovers, release succeeds and frees resources
    streamControl_.stopResult = kIOReturnSuccess;
    EXPECT_EQ(session.ReleaseAudioLease(), kIOReturnSuccess);
    EXPECT_FALSE(session.IsStreaming());
    EXPECT_FALSE(session.AudioLeaseActive());
    EXPECT_EQ(session.TxSlotProvider().payloadBase, nullptr);
}

TEST_F(AudioEndpointStreamSessionTest, MAudioClockArmingOnSessionStart) {
    auto mAudioProfile = MakeTestProfile();
    mAudioProfile.profileBuilder =
        ASFW::DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814;

    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, mAudioProfile);

    EXPECT_FALSE(session.MAudioInternalTimingArmed());

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    EXPECT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_TRUE(session.IsStreaming());
    EXPECT_TRUE(session.MAudioInternalTimingArmed());

    EXPECT_EQ(session.ReleaseMidiLease(), kIOReturnSuccess);
    EXPECT_FALSE(session.IsStreaming());
    EXPECT_FALSE(session.MAudioInternalTimingArmed());
}

TEST_F(AudioEndpointStreamSessionTest, MidiOnlyContentFillBounding) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    // Standalone MIDI session: pcmSource_ is null
    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);

    auto* queue = session.TxSlotProvider().queueControl;
    ASSERT_NE(queue, nullptr);

    // Initial state: 0 completions, 0 finalized, committedEnd at 0
    queue->completionCursor.store(0, std::memory_order_release);
    queue->finalizedEnd.store(0, std::memory_order_release);
    queue->committedEnd.store(0, std::memory_order_release);

    session.OnTxPreparation(1);

    // Descriptors were prepared out to PreparedTargetPackets (~1008 packets)
    const uint64_t committed = queue->committedEnd.load(std::memory_order_acquire);
    EXPECT_GT(committed, 100U);

    // But content fill was bounded to frozen + 16 packets (2 completion groups = 2 ms)
    // rather than running all the way to committedEnd
    EXPECT_EQ(session.TxFillCursor(), 16U);
    EXPECT_EQ(session.TxCommitCursor(), 16U);
}

TEST_F(AudioEndpointStreamSessionTest, DestructorClearsCallbackOnStopFailure) {
    // Configure stop to fail (transport timeout)
    streamControl_.stopResult = kIOReturnTimeout;

    {
        AudioEndpointStreamSession session(
            AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
            hostTransport_, streamControl_, profile_);

        // Start a MIDI session so streaming_ becomes true
        MpxMidiGeometry geom{
            .dbs = 7,
            .midiSlotIndex = 6,
            .portCount = 1,
            .dbcAligned = true,
        };
        ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
        ASSERT_TRUE(session.IsStreaming());

        // Verify callback is registered
        EXPECT_TRUE(isoch_.HasTxPreparationCallback());
    }
    // Session destroyed. Even though StopSessionLocked failed (kIOReturnTimeout),
    // the destructor must have unconditionally cleared the TX preparation callback
    // to prevent a dangling `this` capture.
    EXPECT_FALSE(isoch_.HasTxPreparationCallback());
    // Since StopTransmit() succeeded, shutdown was confirmed and resources were freed.
    EXPECT_FALSE(isoch_.HasTxIsochResources(0));
}

TEST_F(AudioEndpointStreamSessionTest, DestructorRetainsTxResourcesWhenForcedStopFails) {
    streamControl_.stopResult = kIOReturnTimeout;
    isoch_.SetStopTransmitResultForTesting(kIOReturnTimeout);

    {
        AudioEndpointStreamSession session(
            AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
            hostTransport_, streamControl_, profile_);

        MpxMidiGeometry geom{
            .dbs = 7,
            .midiSlotIndex = 6,
            .portCount = 1,
            .dbcAligned = true,
        };
        ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
        ASSERT_TRUE(session.IsStreaming());
        EXPECT_TRUE(isoch_.HasTxIsochResources(0));
    }
    // Session destroyed. Both StopSessionLocked and StopTransmit timed out, and hardware
    // is NOT gone. The destructor must retain TX resources (not free or unmap them) to
    // protect in-flight DMA operations from page faults or kernel panics.
    EXPECT_TRUE(isoch_.HasTxIsochResources(0));
    EXPECT_FALSE(isoch_.HasTxPreparationCallback());

    // Clean up for fixture tear-down
    (void)isoch_.FreeTxIsochResources();
}

TEST_F(AudioEndpointStreamSessionTest, DestructorFreesTxResourcesWhenHardwareGoneEvenIfStopTransmitFails) {
    streamControl_.stopResult = kIOReturnTimeout;
    isoch_.SetStopTransmitResultForTesting(kIOReturnTimeout);

    {
        AudioEndpointStreamSession session(
            AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
            hostTransport_, streamControl_, profile_);

        MpxMidiGeometry geom{
            .dbs = 7,
            .midiSlotIndex = 6,
            .portCount = 1,
            .dbcAligned = true,
        };
        ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
        ASSERT_TRUE(session.IsStreaming());
        EXPECT_TRUE(isoch_.HasTxIsochResources(0));

        // Simulate device unplug / provider revoked
        hardware_.LatchProviderRevokedAndDrain();
    }
    // Hardware is gone: revoked provider cannot perform DMA. Resources must be freed.
    EXPECT_FALSE(isoch_.HasTxIsochResources(0));
    EXPECT_FALSE(isoch_.HasTxPreparationCallback());
}

TEST_F(AudioEndpointStreamSessionTest, AudioJoinAdoptsExistingMidiEpoch) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    // Start MIDI first — this initializes the hardware timeline
    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    ASSERT_TRUE(session.IsStreaming());

    // Capture the epoch set by MIDI's StartSessionLocked
    const uint64_t midiEpoch = bindingSource_.controlBlock.hardwareTimeline.Epoch();
    ASSERT_NE(midiEpoch, 0U);

    // Now join audio — this should NOT call BeginEpoch again because
    // the session is already streaming (streaming_ == true, so StartSessionLocked
    // is not called again). The epoch must remain unchanged.
    ASSERT_EQ(session.AcquireAudioLease(&pcmSource_), kIOReturnSuccess);
    EXPECT_TRUE(session.AudioLeaseActive());
    EXPECT_TRUE(session.MidiLeaseActive());

    const uint64_t postAudioEpoch = bindingSource_.controlBlock.hardwareTimeline.Epoch();
    EXPECT_EQ(postAudioEpoch, midiEpoch);
}

TEST_F(AudioEndpointStreamSessionTest, SessionStopResetsTimelineAndStreamingFlag) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_TRUE(session.IsStreaming());
    EXPECT_TRUE(bindingSource_.controlBlock.isSessionStreaming.load());
    EXPECT_NE(bindingSource_.controlBlock.hardwareTimeline.Epoch(), 0U);

    // Release lease -> session stops completely
    EXPECT_EQ(session.ReleaseMidiLease(), kIOReturnSuccess);
    EXPECT_FALSE(session.IsStreaming());
    EXPECT_FALSE(bindingSource_.controlBlock.isSessionStreaming.load());
    EXPECT_EQ(bindingSource_.controlBlock.hardwareTimeline.Epoch(), 0U);
}

TEST_F(AudioEndpointStreamSessionTest, TxTransportFaultUpdatesStatusAndHaltsPump) {
    AudioEndpointStreamSession session(
        AudioEndpointId{1}, bindingSource_, isoch_, hardware_,
        hostTransport_, streamControl_, profile_);

    MpxMidiGeometry geom{
        .dbs = 7,
        .midiSlotIndex = 6,
        .portCount = 1,
        .dbcAligned = true,
    };
    ASSERT_EQ(session.AcquireMidiLease(&midiBlock_, 1, geom, 48000, 8), kIOReturnSuccess);
    EXPECT_TRUE(session.IsStreaming());

    auto* queue = session.TxSlotProvider().queueControl;
    ASSERT_NE(queue, nullptr);

    // Simulate transport entering producer fault (e.g. payload seal mismatch)
    queue->statusWord.store(ASFW::Isoch::IsochTxQueueStatus::kProducerFault, std::memory_order_release);

    // Preparation callback arrives on fault path
    session.OnTxPreparation(1);

    // Audio transport control block must reflect the fault
    EXPECT_EQ(bindingSource_.controlBlock.txTransportStatus.load(),
              static_cast<uint32_t>(ASFW::Isoch::IsochTxQueueStatus::kProducerFault));
    // Pump must not have advanced fill or commit cursors
    EXPECT_EQ(session.TxFillCursor(), 0U);
    EXPECT_EQ(session.TxCommitCursor(), 0U);
}

} // namespace
