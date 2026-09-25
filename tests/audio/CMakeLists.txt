# tests/audio/CMakeLists.txt

macro(add_audio_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# Isoch Receive Context Tests
add_executable(IsochReceiveContextTests
    IsochReceiveContextTests.cpp
    IsochRxDmaRingTests.cpp
    IsochRxTimingTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochReceiveContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochRxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/AudioClockPublisher.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/DirectInputWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/DescriptorRing.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)
target_link_libraries(IsochReceiveContextTests PRIVATE
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(IsochReceiveContextTests)

# Isoch DMA Memory Manager Tests
add_audio_test(IsochDMAMemoryManagerTests
    IsochDMAMemoryManagerTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
)

# AM824 Encoder Tests
add_audio_test(AM824EncoderTests
    AM824EncoderTests.cpp
)

# RawPcm24In32 Tests
add_audio_test(RawPcm24In32Tests
    RawPcm24In32Tests.cpp
)

# AM824 & RawPcm24In32 Payload Codec Tests
add_audio_test(Am824PayloadCodecTests
    Am824PayloadCodecTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
)

# MOTU Payload Codec and Device Timing Tests
add_audio_test(MotuPayloadCodecTests
    MotuPayloadCodecTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
)

# CIP Header Builder Tests
add_audio_test(CIPHeaderBuilderTests
    CIPHeaderBuilderTests.cpp
)

# MOTU Block Codec Tests
add_audio_test(MotuBlockCodecTests
    MotuBlockCodecTests.cpp
)

# MOTU v2 Register Plane Tests
add_audio_test(MotuV2RegisterTests
    MotuV2RegisterTests.cpp
)

# MOTU Device Profile / Factory Identity Tests
add_audio_test(MotuV2ProfileTests
    MotuV2ProfileTests.cpp
)

# MOTU host channel <-> physical port tables
add_audio_test(MotuPortLayoutTests
    MotuPortLayoutTests.cpp
)

# MOTU device->host payload extraction tests
add_audio_test(MotuPayloadReaderTests
    MotuPayloadReaderTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
)

# MOTU per-data-block presentation offset cache
add_audio_test(MotuEventOffsetCacheTests
    MotuEventOffsetCacheTests.cpp
)

# MOTU transmit-side SPH replay
add_audio_test(MotuTxTimingTests
    MotuTxTimingTests.cpp
)

# MOTU capture-side presentation timing (SPH -> replay offset)
add_audio_test(MotuRxTimingTests
    MotuRxTimingTests.cpp
)

# MOTU host->device payload placement tests
add_audio_test(MotuPayloadWriterTests
    MotuPayloadWriterTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/MOTU/MotuPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
)

# MOTU v2 Protocol Adapter Tests
# DeviceRegistry is linked because the protocol now takes a registry-issued route token
# (ProtocolRegisterIO validates the route on every transaction), so the test fixture has
# to build a real registry entry. Mirrors tests/devices/DICETcatProtocolTests.
add_audio_test(MotuV2ProtocolTests
    MotuV2ProtocolTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/MOTU/MotuV2Protocol.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)

# Blocking Cadence Tests
add_audio_test(BlockingCadenceTests
    BlockingCadenceTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
)

# Non-Blocking Cadence Tests
add_audio_test(NonBlockingCadenceTests
    NonBlockingCadenceTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
)


# Audio Driver config policy tests
add_audio_test(AudioDriverConfigPolicyTests
    AudioDriverConfigPolicyTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioDriverConfigPolicy.cpp"
)



# Direct ADK audio skeleton tests (AudioRuntime files)
add_audio_test(AudioDirectRuntimeTests
    AudioRuntime/AudioStreamMemoryTests.cpp
    AudioRuntime/AudioClientCursorTests.cpp
    AudioRuntime/AudioTransportControlBlockTests.cpp
    AudioRuntime/AudioGraphBindingTests.cpp
    AudioRuntime/DirectAudioEngineTests.cpp
    AudioRuntime/DirectAudioDebugSnapshotTests.cpp
    AudioRuntime/HostClockAnchorTests.cpp
    AudioRuntime/PlaybackRingRangeTests.cpp
)

# Audio telemetry wire v4 contract (FW-175); the golden fixture is shared with
# the Swift decoder tests.
add_audio_test(AudioTelemetryWireTests
    AudioRuntime/AudioTelemetryWireTests.cpp
)
target_compile_definitions(AudioTelemetryWireTests PRIVATE
    ASFW_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")




# RX packet decode outcome split. NOTE: the other files under AudioEngineDirect/
# (DirectTxProbeTests, TxAudioPacketProcessorTests, TxAudioPacketWriterTests) are
# not registered in any CMakeLists and therefore never run.
add_audio_test(RxAudioPacketProcessorTests
    AudioEngineDirect/RxAudioPacketProcessorTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp"
)

# Isoch TX Descriptor Slab Tests
add_audio_test(IsochTxDescriptorSlabTests
    IsochTxDescriptorSlabTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
)

# Isoch TX DMA Ring Tests
add_audio_test(IsochTxDmaRingTests
    IsochTxDmaRingTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

add_audio_test(TransmitBoundaryTests
    TransmitBoundaryTests.cpp
)

add_audio_test(IsochServiceTxPreparationTests
    IsochServiceTxPreparationTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/IsochService.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTransmitContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochReceiveContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochRxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/DescriptorRing.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/AudioClockPublisher.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/DirectInputWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

add_audio_test(AmdtpDirectTxTests
    AmdtpDirectTxTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
)



# The driver's real TX producer (ASFWAudioDriverZts.cpp) against an emulated
# IT transport. The IIG-generated headers resolve to the host shims beside
# ASFWAudioNub.h.
add_audio_test(AudioDriverTxProducerTests
    AudioDriverTxProducerTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/ASFWAudioDriverZts.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/MOTU/MotuPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/MAudioInternalTxTiming.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/MAudio/MAudioTxClockBridge.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/MAudio/MAudioPresentationObserver.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/MAudio/MAudioDuplexChoreography.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfile.cpp"
)
target_include_directories(AudioDriverTxProducerTests PRIVATE
    "${ASFW_TESTS_DIR}/mocks/net.mrmidi.ASFW.ASFWDriver"
)

add_audio_test(AmdtpRateGeometryTests
    AmdtpRateGeometryTests.cpp
)


add_audio_test(RxDrivenTimingTests
    RxDrivenTimingTests.cpp
)

# TX refill generation/range coverage (decides margin-race vs coverage bug)
add_audio_test(TxRefillCoverageTests
    TxRefillCoverageTests.cpp
)

# Endpoint runtime tests
add_audio_test(AudioEndpointRuntimeTests
    AudioEndpointRuntimeTests.cpp
)



# Device-vs-profile stream geometry resolution (header-only, pure).
add_audio_test(StreamGeometryResolverTests
    StreamGeometryResolverTests.cpp
)

# Profile aggregation through the production TxChannelCount()/RxChannelCount().
add_audio_test(StreamProfileAggregationTests
    StreamProfileAggregationTests.cpp
)

add_audio_test(DiceFixtureGeometryTests
    DiceFixtureGeometryTests.cpp
)

add_audio_test(ResolvedStreamConfigTests
    ResolvedStreamConfigTests.cpp
)

add_audio_test(NubGeometryRoundTripTests
    NubGeometryRoundTripTests.cpp
    ${CMAKE_SOURCE_DIR}/../ASFWDriver/Audio/DriverKit/Config/AudioDriverConfig.cpp
)

# DICE Profile Registry and Quirks Tests
add_audio_test(DiceProfileTests
    DiceProfileTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/DeviceProfiles/Audio/AudioDeviceCatalog.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/MOTU/MotuV2Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/ApogeeDuetProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/BeBoBProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx820iProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx400FProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/Phase88Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfile.cpp"
)

# Every DICE profile's answers, recorded (DICE_TCAT_ARCHITECTURE.md §4.2 stage C).
# Zero timestamps the RX path publishes, recorded (Epic 4, T1).
add_audio_test(ZtsCharacterizationTests
    ZtsCharacterizationTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/AudioClockPublisher.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/DirectInputWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

add_audio_test(DiceProfileEquivalenceTests
    DiceProfileEquivalenceTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/DeviceProfiles/Audio/AudioDeviceCatalog.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/MOTU/MotuV2Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/ApogeeDuetProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/BeBoBProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx820iProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx400FProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/Phase88Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfile.cpp"
)

# Timing / HAL geometry authority (FW-177). ProfileTimingPinTests pins every
# profile's declarations; TimingGeometryTests covers the pure resolver.
set(ASFW_TIMING_PROFILE_SOURCES
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/DeviceProfiles/Audio/AudioDeviceCatalog.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/MOTU/MotuV2Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/ApogeeDuetProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/BeBoBProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx820iProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx400FProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/Phase88Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfile.cpp"
)
add_audio_test(ProfileTimingPinTests
    ProfileTimingPinTests.cpp
    ${ASFW_TIMING_PROFILE_SOURCES}
)
target_include_directories(ProfileTimingPinTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_audio_test(TimingGeometryTests
    TimingGeometryTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
)

# IT Descriptor Layout Tests
add_audio_test(ITDescriptorLayoutTests
    ITDescriptorLayoutTests.cpp
)

# Channel Map Permutation Tests (Exit Criterion)
add_audio_test(ChannelMapPermutationTests
    ChannelMapPermutationTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/DirectInputWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/AudioClockPublisher.cpp"
)

add_audio_test(HardwareSampleTimelineTests
    HardwareSampleTimelineTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
)

add_audio_test(TxCycleAnchorTests
    TxCycleAnchorTests.cpp
)
