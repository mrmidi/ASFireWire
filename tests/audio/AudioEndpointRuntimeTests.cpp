#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Config/AudioConstants.hpp"
#include "FakeTimerScheduler.hpp"

namespace {

ASFW::Audio::Devices::ResolvedAudioEndpointProfile MakeProfile() {
    ASFW::Audio::Devices::ResolvedAudioEndpointProfile profile{};
    profile.endpointId = ASFW::Audio::Devices::AudioEndpointId{41};
    profile.deviceInstanceId = ASFW::Discovery::DeviceInstanceId{17};
    profile.observedGuid = 0x1020304050607080ULL;
    profile.currentSampleRateHz = 48000;
    profile.runtimeCaps.sampleRateHz = 48000;
    profile.runtimeCaps.hostInputPcmChannels = 6;
    profile.runtimeCaps.hostOutputPcmChannels = 4;
    profile.runtimeCaps.deviceToHostAm824Slots = 6;
    profile.runtimeCaps.hostToDeviceAm824Slots = 4;
    profile.runtimeCaps.deviceToHostStreamCount = 1;
    profile.runtimeCaps.hostToDeviceStreamCount = 1;
    profile.runtimeCaps.deviceToHostStreams[0] = {.pcmChannels = 6,
                                                  .am824Slots = 6};
    profile.runtimeCaps.hostToDeviceStreams[0] = {.pcmChannels = 4,
                                                  .am824Slots = 4};
    return profile;
}

} // namespace

TEST(AudioEndpointRuntime, ResolvedEndpointDoesNotPublishBindingBeforeMemoryCopy) {
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    EXPECT_FALSE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_FALSE(snapshot.valid);

    ASFW::Audio::Runtime::AudioTelemetryEndpointSnapshot telemetry{};
    ASSERT_TRUE(runtime.CopyAudioTelemetrySnapshot(telemetry));
    EXPECT_EQ(telemetry.endpointId, 41U);
    EXPECT_EQ(telemetry.deviceInstanceId, 17U);
    EXPECT_EQ(telemetry.observedGuid, 0x1020304050607080ULL);
    EXPECT_EQ(telemetry.flags &
                  ASFW::Audio::Runtime::kAudioTelemetryBindingReady,
              0U);
}

TEST(AudioEndpointRuntime, TelemetryKeepsConfiguredUnboundEndpointVisible) {
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());

    ASFW::Audio::Runtime::AudioTelemetryEndpointSnapshot telemetry{};
    ASSERT_TRUE(runtime.CopyAudioTelemetrySnapshot(telemetry));
    EXPECT_EQ(telemetry.observedGuid, 0x1020304050607080ULL);
    EXPECT_EQ(telemetry.sampleRateHz, 48'000U);
    EXPECT_EQ(telemetry.outputChannels, 4U);
    EXPECT_EQ(telemetry.inputChannels, 6U);
    EXPECT_EQ(telemetry.flags &
                  ASFW::Audio::Runtime::kAudioTelemetryBindingReady,
              0U);
}

TEST(AudioEndpointRuntime, BadCopyArgsZeroOutputs) {
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());

    IOMemoryDescriptor* inputMemory = reinterpret_cast<IOMemoryDescriptor*>(0x1);
    IOMemoryDescriptor* controlMemory = reinterpret_cast<IOMemoryDescriptor*>(0x2);
    uint32_t outputFrames = 99;
    uint32_t outputChannels = 98;
    uint32_t inputFrames = 97;
    uint32_t inputChannels = 96;
    uint32_t sampleRateHz = 95;
    uint64_t generation = 94;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(nullptr,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    EXPECT_EQ(kr, kIOReturnBadArgument);
    EXPECT_EQ(inputMemory, nullptr);
    EXPECT_EQ(controlMemory, nullptr);
    EXPECT_EQ(outputFrames, 0u);
    EXPECT_EQ(outputChannels, 0u);
    EXPECT_EQ(inputFrames, 0u);
    EXPECT_EQ(inputChannels, 0u);
    EXPECT_EQ(sampleRateHz, 0u);
    EXPECT_EQ(generation, 0u);
}

TEST(AudioEndpointRuntime, CopyDirectAudioMemoryAllocatesCompleteDuplexBinding) {
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    const kern_return_t kr = runtime.CopyDirectAudioMemory(&outputMemory,
                                                           &inputMemory,
                                                           &controlMemory,
                                                           &outputFrames,
                                                           &outputChannels,
                                                           &inputFrames,
                                                           &inputChannels,
                                                           &sampleRateHz,
                                                           &generation);

    ASSERT_EQ(kr, kIOReturnSuccess);
    ASSERT_NE(outputMemory, nullptr);
    ASSERT_NE(inputMemory, nullptr);
    ASSERT_NE(controlMemory, nullptr);
    EXPECT_EQ(outputFrames, ASFW::Audio::Config::kAudioOutputRingFrames);
    EXPECT_EQ(inputFrames, ASFW::Audio::Config::kAudioRingBufferFrames);
    EXPECT_EQ(outputChannels, 4u);
    EXPECT_EQ(inputChannels, 6u);
    EXPECT_EQ(sampleRateHz, 48000u);
    EXPECT_GT(generation, 0u);

    ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
    ASSERT_TRUE(runtime.CopyDirectAudioBinding(snapshot));
    EXPECT_TRUE(snapshot.IsValidDuplex());
    EXPECT_EQ(snapshot.outputFrames, outputFrames);
    EXPECT_EQ(snapshot.outputChannels, outputChannels);
    EXPECT_EQ(snapshot.inputFrames, inputFrames);
    EXPECT_EQ(snapshot.inputChannels, inputChannels);
    EXPECT_EQ(snapshot.sampleRateHz, sampleRateHz);
    EXPECT_EQ(snapshot.generation, generation);

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

TEST(AudioEndpointRuntime, PlaybackOnlyPresentationKeepsPhysicalReturnRingForDuplexTransport) {
    auto profile = MakeProfile();
    profile.runtimeCaps.hostInputPcmChannels = 0; // CoreAudio-visible topology.
    profile.runtimeCaps.hostOutputPcmChannels = 2;
    profile.runtimeCaps.deviceToHostAm824Slots = 2;
    profile.runtimeCaps.hostToDeviceAm824Slots = 2;
    profile.runtimeCaps.deviceToHostStreams[0] = {.pcmChannels = 2,
                                                  .am824Slots = 2};
    profile.runtimeCaps.hostToDeviceStreams[0] = {.pcmChannels = 2,
                                                  .am824Slots = 2};
    ASFW::Audio::AudioEndpointRuntime runtime(profile);

    IOMemoryDescriptor* outputMemory = nullptr;
    IOMemoryDescriptor* inputMemory = nullptr;
    IOMemoryDescriptor* controlMemory = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t sampleRateHz = 0;
    uint64_t generation = 0;

    ASSERT_EQ(runtime.CopyDirectAudioMemory(&outputMemory,
                                            &inputMemory,
                                            &controlMemory,
                                            &outputFrames,
                                            &outputChannels,
                                            &inputFrames,
                                            &inputChannels,
                                            &sampleRateHz,
                                            &generation),
              kIOReturnSuccess);
    EXPECT_EQ(outputChannels, 2U);
    EXPECT_EQ(inputChannels, 2U);

    outputMemory->release();
    inputMemory->release();
    controlMemory->release();
}

TEST(AudioEndpointRuntime, ConfigurationChangesReuseMaximumCapacityDescriptors) {
    auto profile = MakeProfile();
    profile.configurationCapabilityCount = 1;
    auto& adat = profile.configurationCapabilities[0];
    adat.configuration = {
        .sampleRate = 44100,
        .opticalInput = ASFW::Configuration::OpticalMode::Adat,
        .opticalOutput = ASFW::Configuration::OpticalMode::Adat,
    };
    adat.runtimeCaps = profile.runtimeCaps;
    adat.runtimeCaps.sampleRateHz = 44100;
    adat.runtimeCaps.hostInputPcmChannels = 16;
    adat.runtimeCaps.hostOutputPcmChannels = 12;
    adat.runtimeCaps.deviceToHostAm824Slots = 17;
    adat.runtimeCaps.hostToDeviceAm824Slots = 13;
    adat.runtimeCaps.deviceToHostStreams[0] = {.pcmChannels = 16, .am824Slots = 17};
    adat.runtimeCaps.hostToDeviceStreams[0] = {.pcmChannels = 12, .am824Slots = 13};
    ASFW::Audio::AudioEndpointRuntime runtime(profile);

    IOMemoryDescriptor* firstOutput = nullptr;
    IOMemoryDescriptor* firstInput = nullptr;
    IOMemoryDescriptor* firstControl = nullptr;
    uint32_t outputFrames = 0;
    uint32_t outputChannels = 0;
    uint32_t inputFrames = 0;
    uint32_t inputChannels = 0;
    uint32_t rate = 0;
    uint64_t firstGeneration = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(
                  &firstOutput, &firstInput, &firstControl, &outputFrames,
                  &outputChannels, &inputFrames, &inputChannels, &rate,
                  &firstGeneration),
              kIOReturnSuccess);
    EXPECT_EQ(outputChannels, 4U);
    EXPECT_EQ(inputChannels, 6U);

    ASSERT_TRUE(runtime.ApplyConfiguration(adat.runtimeCaps));

    IOMemoryDescriptor* secondOutput = nullptr;
    IOMemoryDescriptor* secondInput = nullptr;
    IOMemoryDescriptor* secondControl = nullptr;
    uint64_t secondGeneration = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(
                  &secondOutput, &secondInput, &secondControl, &outputFrames,
                  &outputChannels, &inputFrames, &inputChannels, &rate,
                  &secondGeneration),
              kIOReturnSuccess);
    EXPECT_EQ(secondOutput, firstOutput);
    EXPECT_EQ(secondInput, firstInput);
    EXPECT_EQ(secondControl, firstControl);
    EXPECT_EQ(outputChannels, 12U);
    EXPECT_EQ(inputChannels, 16U);
    EXPECT_EQ(rate, 44100U);
    EXPECT_GT(secondGeneration, firstGeneration);

    firstOutput->release();
    firstInput->release();
    firstControl->release();
    secondOutput->release();
    secondInput->release();
    secondControl->release();
}

TEST(AudioEndpointRuntime, AllocationLimitsComputedAcrossFormations) {
    ASFW::Audio::Devices::ResolvedAudioEndpointProfile profile{};
    profile.endpointId = ASFW::Audio::Devices::AudioEndpointId{50};
    profile.deviceInstanceId = ASFW::Discovery::DeviceInstanceId{20};
    profile.observedGuid = 0x1122334455667788ULL;
    profile.currentSampleRateHz = 48000;
    profile.runtimeCaps.sampleRateHz = 48000;
    profile.runtimeCaps.hostInputPcmChannels = 8;
    profile.runtimeCaps.hostOutputPcmChannels = 8;
    profile.runtimeCaps.deviceToHostStreamCount = 1;
    profile.runtimeCaps.hostToDeviceStreamCount = 1;
    profile.runtimeCaps.deviceToHostStreams[0] = {.pcmChannels = 8, .am824Slots = 8};
    profile.runtimeCaps.hostToDeviceStreams[0] = {.pcmChannels = 8, .am824Slots = 8};

    profile.supportedRates[0] = 48000;
    profile.supportedRates[1] = 96000;
    profile.supportedRateCount = 2;

    auto& cap48 = profile.configurationCapabilities[0].runtimeCaps;
    cap48.sampleRateHz = 48000;
    cap48.hostOutputPcmChannels = 8;
    cap48.hostInputPcmChannels = 8;
    cap48.hostToDeviceStreamCount = 1;
    cap48.deviceToHostStreamCount = 1;
    cap48.hostToDeviceStreams[0] = {.pcmChannels = 8, .am824Slots = 8};
    cap48.deviceToHostStreams[0] = {.pcmChannels = 8, .am824Slots = 8};

    auto& cap96 = profile.configurationCapabilities[1].runtimeCaps;
    cap96.sampleRateHz = 96000;
    cap96.hostOutputPcmChannels = 6;
    cap96.hostInputPcmChannels = 4;
    cap96.hostToDeviceStreamCount = 1;
    cap96.deviceToHostStreamCount = 1;
    cap96.hostToDeviceStreams[0] = {.pcmChannels = 6, .am824Slots = 6};
    cap96.deviceToHostStreams[0] = {.pcmChannels = 4, .am824Slots = 4};
    profile.configurationCapabilityCount = 2;

    ASFW::Audio::AudioEndpointRuntime runtime(profile);
    const auto& limits = runtime.AllocationLimits();

    constexpr uint64_t expectedMaxOut = 24'576ULL * 6ULL * sizeof(float); // 589,824
    constexpr uint64_t expectedMaxIn = 12'288ULL * 8ULL * sizeof(float);  // 393,216
    EXPECT_EQ(limits.allocatedOutputBytes, expectedMaxOut);
    EXPECT_EQ(limits.allocatedInputBytes, expectedMaxIn);
    EXPECT_EQ(limits.maxOutputChannels, 8U);
    EXPECT_EQ(limits.maxInputChannels, 8U);
    EXPECT_EQ(limits.maxAllocatedFrames, 24'576U);
}

TEST(AudioEndpointRuntime, RateTransitionPreservesBackingMemoryWhileUpdatingActiveFrames) {
    ASFW::Audio::Devices::ResolvedAudioEndpointProfile profile = MakeProfile();
    profile.supportedRates[0] = 48000;
    profile.supportedRates[1] = 96000;
    profile.supportedRateCount = 2;

    auto& cap48 = profile.configurationCapabilities[0].runtimeCaps;
    cap48.sampleRateHz = 48000;
    cap48.hostOutputPcmChannels = 4;
    cap48.hostInputPcmChannels = 6;
    cap48.hostToDeviceStreamCount = 1;
    cap48.deviceToHostStreamCount = 1;
    cap48.hostToDeviceStreams[0] = {.pcmChannels = 4, .am824Slots = 4};
    cap48.deviceToHostStreams[0] = {.pcmChannels = 6, .am824Slots = 6};

    auto& cap96 = profile.configurationCapabilities[1].runtimeCaps;
    cap96.sampleRateHz = 96000;
    cap96.hostOutputPcmChannels = 4;
    cap96.hostInputPcmChannels = 6;
    cap96.hostToDeviceStreamCount = 1;
    cap96.deviceToHostStreamCount = 1;
    cap96.hostToDeviceStreams[0] = {.pcmChannels = 4, .am824Slots = 4};
    cap96.deviceToHostStreams[0] = {.pcmChannels = 6, .am824Slots = 6};
    profile.configurationCapabilityCount = 2;

    ASFW::Audio::AudioEndpointRuntime runtime(profile);

    IOMemoryDescriptor* firstOut = nullptr;
    IOMemoryDescriptor* firstIn = nullptr;
    IOMemoryDescriptor* firstCtl = nullptr;
    uint32_t outFrames = 0, outCh = 0, inFrames = 0, inCh = 0, rate = 0;
    uint64_t gen1 = 0;

    // 1. Initial 48k memory copy
    ASSERT_EQ(runtime.CopyDirectAudioMemory(
                  &firstOut, &firstIn, &firstCtl, &outFrames,
                  &outCh, &inFrames, &inCh, &rate, &gen1),
              kIOReturnSuccess);
    EXPECT_EQ(rate, 48'000U);
    EXPECT_EQ(outFrames, 12'288U);
    EXPECT_EQ(inFrames, 12'288U);

    uint64_t outLen = 0, inLen = 0;
    ASSERT_EQ(firstOut->GetLength(&outLen), kIOReturnSuccess);
    ASSERT_EQ(firstIn->GetLength(&inLen), kIOReturnSuccess);
    EXPECT_GE(outLen, 24'576ULL * 4 * sizeof(float));
    EXPECT_GE(inLen, 24'576ULL * 6 * sizeof(float));

    // 2. Transition to 96k
    ASSERT_TRUE(runtime.ApplyConfiguration(cap96));

    IOMemoryDescriptor* secondOut = nullptr;
    IOMemoryDescriptor* secondIn = nullptr;
    IOMemoryDescriptor* secondCtl = nullptr;
    uint64_t gen2 = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(
                  &secondOut, &secondIn, &secondCtl, &outFrames,
                  &outCh, &inFrames, &inCh, &rate, &gen2),
              kIOReturnSuccess);
    EXPECT_EQ(secondOut, firstOut);
    EXPECT_EQ(secondIn, firstIn);
    EXPECT_EQ(secondCtl, firstCtl);
    EXPECT_EQ(rate, 96'000U);
    EXPECT_EQ(outFrames, 24'576U);
    EXPECT_EQ(inFrames, 24'576U);
    EXPECT_GT(gen2, gen1);

    // 3. Transition back to 48k
    ASSERT_TRUE(runtime.ApplyConfiguration(cap48));

    IOMemoryDescriptor* thirdOut = nullptr;
    IOMemoryDescriptor* thirdIn = nullptr;
    IOMemoryDescriptor* thirdCtl = nullptr;
    uint64_t gen3 = 0;
    ASSERT_EQ(runtime.CopyDirectAudioMemory(
                  &thirdOut, &thirdIn, &thirdCtl, &outFrames,
                  &outCh, &inFrames, &inCh, &rate, &gen3),
              kIOReturnSuccess);
    EXPECT_EQ(thirdOut, firstOut);
    EXPECT_EQ(thirdIn, firstIn);
    EXPECT_EQ(thirdCtl, firstCtl);
    EXPECT_EQ(rate, 48'000U);
    EXPECT_EQ(outFrames, 12'288U);
    EXPECT_EQ(inFrames, 12'288U);
    EXPECT_GT(gen3, gen2);

    firstOut->release();
    firstIn->release();
    firstCtl->release();
    secondOut->release();
    secondIn->release();
    secondCtl->release();
    thirdOut->release();
    thirdIn->release();
    thirdCtl->release();
}

TEST(AudioEndpointRuntime, RejectsConfigurationExceedingAllocationLimits) {
    ASFW::Audio::Devices::ResolvedAudioEndpointProfile profile{};
    profile.endpointId = ASFW::Audio::Devices::AudioEndpointId{52};
    profile.deviceInstanceId = ASFW::Discovery::DeviceInstanceId{22};
    profile.unitInstanceId = ASFW::Discovery::UnitInstanceId{22, 0};
    profile.runtimeCaps.sampleRateHz = 48000;
    profile.runtimeCaps.hostOutputPcmChannels = 4;
    profile.runtimeCaps.hostInputPcmChannels = 4;
    profile.runtimeCaps.deviceToHostStreams[0] = {.pcmChannels = 4, .am824Slots = 4};
    profile.runtimeCaps.hostToDeviceStreams[0] = {.pcmChannels = 4, .am824Slots = 4};

    ASFW::Audio::AudioEndpointRuntime runtime(profile);

    // 192 kHz requires 49,152 frames, which exceeds kAllocatedFrameRingFrames (24,576)
    ASFW::Audio::AudioStreamRuntimeCaps cap192 = profile.runtimeCaps;
    cap192.sampleRateHz = 192000;
    EXPECT_FALSE(runtime.ApplyConfiguration(cap192));
}

TEST(AudioEndpointRuntime, ReaderIsolatedAcrossSessionRearm) {
    (void)ASFW::Timing::initializeHostTimebase();
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());
    auto session = std::make_shared<ASFW::Audio::Runtime::TxLatencySession>();
    runtime.RegisterTxLatencySession(session);

    uint32_t sid1 = 0;
    ASSERT_TRUE(runtime.StartTxLatencySession(10, 1, 0x1234, 100, &sid1));
    EXPECT_GT(sid1, 0U);

    // Stop session 1
    EXPECT_TRUE(runtime.StopTxLatencySession(sid1));

    // Page session 1 before rearm
    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    EXPECT_TRUE(runtime.CopyTxLatencyResults(0, 32, sid1, page));
    EXPECT_EQ(page.header.sessionId, sid1);
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(ASFW::Audio::Runtime::TxLatencySessionState::Frozen));

    // Arm session 2
    uint32_t sid2 = 0;
    ASSERT_TRUE(runtime.StartTxLatencySession(10, 1, 0x5678, 100, &sid2));
    EXPECT_NE(sid1, sid2);

    // Outstanding reader pages session 1 AFTER session 2 was armed and is capturing!
    ASFW::UserClient::Wire::TxLatencyResultsPageWire retainedPage{};
    EXPECT_TRUE(runtime.CopyTxLatencyResults(0, 32, sid1, retainedPage));
    EXPECT_EQ(retainedPage.header.sessionId, sid1);
    EXPECT_EQ(retainedPage.header.sessionState, static_cast<uint32_t>(ASFW::Audio::Runtime::TxLatencySessionState::Frozen));

    // Reader querying active session gets session 2
    ASFW::UserClient::Wire::TxLatencyResultsPageWire activePage{};
    EXPECT_TRUE(runtime.CopyTxLatencyResults(0, 32, sid2, activePage));
    EXPECT_EQ(activePage.header.sessionId, sid2);
    EXPECT_EQ(activePage.header.sessionState, static_cast<uint32_t>(ASFW::Audio::Runtime::TxLatencySessionState::Capturing));

    runtime.UnregisterTxLatencySession(session);
}

TEST(AudioEndpointRuntime, IndependentDeadlineTimerFiresWithoutInterrupts) {
    (void)ASFW::Timing::initializeHostTimebase();
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());
    ASFW::Testing::FakeTimerScheduler scheduler;
    runtime.SetTimerScheduler(&scheduler);

    auto session = std::make_shared<ASFW::Audio::Runtime::TxLatencySession>();
    runtime.RegisterTxLatencySession(session);

    uint32_t sid = 0;
    // 1 second duration
    ASSERT_TRUE(runtime.StartTxLatencySession(1, 1, 0x1234, 100, &sid));
    EXPECT_EQ(session->State(), ASFW::Audio::Runtime::TxLatencySessionState::Capturing);
    EXPECT_EQ(scheduler.PendingCount(), 1U);

    // Advance clock past the deadline (1s + 50ms buffer = 1.05s)
    scheduler.Advance(1'100'000'000ULL);

    // Session must be Frozen with DeadlineExpired without any audio completions arriving!
    EXPECT_EQ(session->State(), ASFW::Audio::Runtime::TxLatencySessionState::Frozen);

    ASFW::UserClient::Wire::TxLatencyResultsPageWire page{};
    EXPECT_TRUE(runtime.CopyTxLatencyResults(0, 32, sid, page));
    EXPECT_EQ(page.header.sessionId, sid);
    EXPECT_EQ(page.header.sessionState, static_cast<uint32_t>(ASFW::Audio::Runtime::TxLatencySessionState::Frozen));
    EXPECT_EQ(page.header.terminationReason, static_cast<uint32_t>(ASFW::Audio::Runtime::TxLatencyTerminationReason::DeadlineExpired));

    runtime.UnregisterTxLatencySession(session);
}

TEST(AudioEndpointRuntime, TargetedStopRejectsMismatchedSession) {
    (void)ASFW::Timing::initializeHostTimebase();
    ASFW::Audio::AudioEndpointRuntime runtime(MakeProfile());
    auto session = std::make_shared<ASFW::Audio::Runtime::TxLatencySession>();
    runtime.RegisterTxLatencySession(session);

    uint32_t sid = 0;
    ASSERT_TRUE(runtime.StartTxLatencySession(10, 1, 0x1234, 100, &sid));
    EXPECT_EQ(session->State(), ASFW::Audio::Runtime::TxLatencySessionState::Capturing);

    // Stopping with mismatched sessionId must fail and leave session capturing
    EXPECT_FALSE(runtime.StopTxLatencySession(sid + 999));
    EXPECT_EQ(session->State(), ASFW::Audio::Runtime::TxLatencySessionState::Capturing);

    // Stopping with matching sessionId must succeed and freeze session
    EXPECT_TRUE(runtime.StopTxLatencySession(sid));
    EXPECT_EQ(session->State(), ASFW::Audio::Runtime::TxLatencySessionState::Frozen);

    runtime.UnregisterTxLatencySession(session);
}
