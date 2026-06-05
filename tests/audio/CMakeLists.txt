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
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochReceiveContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochRxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/AudioClockPublisher.cpp"
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/DirectInputWriter.cpp"
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/Rx/RxAudioPacketProcessor.cpp"
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

# TX Verifier Decode Tests
add_audio_test(TxVerifierDecodeTests
    TxVerifierDecodeTests.cpp
)

# Blocking Cadence Tests
add_audio_test(BlockingCadenceTests
    BlockingCadenceTests.cpp
)

# Non-Blocking Cadence Tests
add_audio_test(NonBlockingCadenceTests
    NonBlockingCadenceTests.cpp
)

# Blocking DBC Tests
add_audio_test(BlockingDbcTests
    BlockingDbcTests.cpp
)

# Audio Ring Buffer Tests
add_audio_test(AudioRingBufferTests
    AudioRingBufferTests.cpp
)

# Audio Driver config policy tests
add_audio_test(AudioDriverConfigPolicyTests
    AudioDriverConfigPolicyTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioDriverConfigPolicy.cpp"
)

# Packet Assembler Tests
add_audio_test(PacketAssemblerTests
    PacketAssemblerTests.cpp
)

# Direct ADK audio skeleton tests (AudioRuntime files)
add_audio_test(AudioDirectRuntimeTests
    AudioRuntime/AudioStreamMemoryTests.cpp
    AudioRuntime/AudioClientCursorTests.cpp
    AudioRuntime/AudioTransportControlBlockTests.cpp
    AudioRuntime/AudioGraphBindingTests.cpp
    AudioRuntime/DirectAudioEngineTests.cpp
    AudioRuntime/DirectAudioDebugSnapshotTests.cpp
    AudioRuntime/AudioSampleRingTests.cpp
    AudioRuntime/TimingCursorPolicyTests.cpp
    AudioRuntime/ZtsAuthorityTests.cpp
    AudioRuntime/ZtsTimelineCalculatorTests.cpp
)

# Direct TX sidecar tests (AudioEngineDirect files)
add_audio_test(AudioEngineDirectTxTests
    AudioEngineDirect/DirectTxProbeTests.cpp
    AudioEngineDirect/TxAudioPacketProcessorTests.cpp
    AudioEngineDirect/TxAudioPacketWriterTests.cpp
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/Tx/DirectTxProbe.cpp"
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/Tx/TxAudioPacketProcessor.cpp"
    "${ASFW_DRIVER_DIR}/AudioEngine/Direct/Tx/TxAudioPacketWriter.cpp"
)

# Isoch TX Descriptor Slab Tests
add_audio_test(IsochTxDescriptorSlabTests
    IsochTxDescriptorSlabTests.cpp
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
)

# SYT generator tests
add_audio_test(SYTGeneratorTests
    SYTGeneratorTests.cpp
    "${ASFW_DRIVER_DIR}/AudioWire/AMDTP/SYTGenerator.cpp"
)

# Output Cursor Discipline Tests
add_audio_test(OutputCursorDisciplineTests
    OutputCursorDisciplineTests.cpp
)

# Endpoint runtime tests
add_audio_test(AudioEndpointRuntimeTests
    AudioEndpointRuntimeTests.cpp
)

# External sync bridge tests
add_audio_test(ExternalSyncBridgeTests
    ExternalSyncBridgeTests.cpp
)

# External sync discipline (48k) tests
add_audio_test(ExternalSyncDiscipline48kTests
    ExternalSyncDiscipline48kTests.cpp
)

# SimITEngine Tests
add_audio_test(SimITEngineTests
    SimITEngineTests.cpp
)

# Audio Profile Registry Tests
add_audio_test(AudioProfileRegistryTests
    AudioProfileRegistryTests.cpp
)

# IT Descriptor Layout Tests
add_audio_test(ITDescriptorLayoutTests
    ITDescriptorLayoutTests.cpp
)
