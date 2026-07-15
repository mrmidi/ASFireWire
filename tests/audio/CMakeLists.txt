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

# CIP Header Builder Tests
add_audio_test(CIPHeaderBuilderTests
    CIPHeaderBuilderTests.cpp
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
    "${ASFW_DRIVER_DIR}/Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
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



# Audio Profile Registry Tests
add_audio_test(AudioProfileRegistryTests
    AudioProfileRegistryTests.cpp
)

# DICE Profile Registry and Quirks Tests
add_audio_test(DiceProfileTests
    DiceProfileTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/ApogeeDuetProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/Isoch/Profiles/GenericDiceProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/Isoch/Profiles/MidasVeniceProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/Isoch/Profiles/PreSonusStudioLiveProfile.cpp"
)

# IT Descriptor Layout Tests
add_audio_test(ITDescriptorLayoutTests
    ITDescriptorLayoutTests.cpp
)
