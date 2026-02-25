cmake_minimum_required(VERSION 3.23)

project(ASFWPacketTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASFW_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(ASFW_ROOT_DIR "${ASFW_TESTS_DIR}/..")
set(ASFW_DRIVER_DIR "${ASFW_ROOT_DIR}/ASFWDriver")
set(ASFW_DRIVERKIT_DIR "${ASFW_ROOT_DIR}/docs")

set(ASFW_COMMON_INCLUDES
    "${ASFW_TESTS_DIR}/mocks"
    "${ASFW_ROOT_DIR}"
    "${ASFW_DRIVER_DIR}"
    "${ASFW_DRIVER_DIR}/Async"
    "${ASFW_DRIVER_DIR}/Core"
    "${ASFW_DRIVER_DIR}/Bus"
    "${ASFW_DRIVERKIT_DIR}"
    "${ASFW_DRIVER_DIR}/Hardware"
    "${ASFW_DRIVER_DIR}/Testing"
    "${ASFW_DRIVER_DIR}/Discovery"
    "${ASFW_ROOT_DIR}/AppleHeaders"
)

enable_testing()

find_package(GTest CONFIG QUIET)

if (NOT GTest_FOUND)
  include(FetchContent)
  set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.0
  )
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# Minimal library for packet serdes only
add_library(asfw_packet_serdes STATIC
    "${ASFW_DRIVER_DIR}/Async/Tx/PacketBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/ARPacketParser.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/PacketRouter.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
    "${ASFW_TESTS_DIR}/ResponseSenderStub.cpp"
)

target_include_directories(asfw_packet_serdes PUBLIC ${ASFW_COMMON_INCLUDES})

target_compile_features(asfw_packet_serdes PUBLIC cxx_std_23)
target_compile_definitions(asfw_packet_serdes PUBLIC ASFW_HOST_TEST)

add_executable(ASFWPacketTests
    "${ASFW_TESTS_DIR}/PacketSerDesTests.cpp"
    "${ASFW_TESTS_DIR}/AsyncPacketSerDesLinuxCompatTests.cpp"
    "${ASFW_TESTS_DIR}/CompareAndSwapPacketTests.cpp"
)

target_link_libraries(ASFWPacketTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
)

target_compile_definitions(ASFWPacketTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ASFWPacketTests PRIVATE ${ASFW_COMMON_INCLUDES})

# TLabel Matching Tests - Critical bug fix verification
add_executable(TLabelMatchingTests
    "${ASFW_TESTS_DIR}/TLabelMatchingTests.cpp"
)

target_link_libraries(TLabelMatchingTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
)

target_compile_definitions(TLabelMatchingTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(TLabelMatchingTests PRIVATE ${ASFW_COMMON_INCLUDES})

# Completion Callback Pattern Tests - Documents the Apple-style fix
add_executable(CompletionCallbackPatternTests
    "${ASFW_TESTS_DIR}/CompletionCallbackPatternTests.cpp"
)

target_link_libraries(CompletionCallbackPatternTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(CompletionCallbackPatternTests
    PRIVATE
        ASFW_HOST_TEST
)

# Completion Strategy Tests - Explicit two-path completion model (gotAck vs gotPacket)
add_executable(CompletionStrategyTests
    "${ASFW_TESTS_DIR}/CompletionStrategyTests.cpp"
)

target_link_libraries(CompletionStrategyTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(CompletionStrategyTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(CompletionStrategyTests PRIVATE ${ASFW_COMMON_INCLUDES})

# Completion Refactor Plan Tests - ACK/AR race coverage scaffolding
add_executable(CompletionRefactorPlanTests
    "${ASFW_TESTS_DIR}/CompletionRefactorPlanTests.cpp"
    "${ASFW_DRIVER_DIR}/Async/Core/Transaction.cpp"
    "${ASFW_DRIVER_DIR}/Async/Core/TransactionManager.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/PayloadHandle.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(CompletionRefactorPlanTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(CompletionRefactorPlanTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(CompletionRefactorPlanTests PRIVATE
    "${ASFW_TESTS_DIR}/mocks"
    ${ASFW_COMMON_INCLUDES}
)

gtest_discover_tests(CompletionRefactorPlanTests)

# Gap Count Optimizer Tests - Bus reset storm fix
add_executable(GapCountOptimizerTests
    "${ASFW_TESTS_DIR}/GapCountOptimizerTests.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GapCountOptimizer.cpp"
)

target_link_libraries(GapCountOptimizerTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(GapCountOptimizerTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(GapCountOptimizerTests PRIVATE ${ASFW_COMMON_INCLUDES})

# Topology Gap Extraction Tests - Real-world FireBug data validation
add_executable(TopologyGapExtractionTests
    "${ASFW_TESTS_DIR}/TopologyGapExtractionTests.cpp"
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(TopologyGapExtractionTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(TopologyGapExtractionTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(TopologyGapExtractionTests PRIVATE ${ASFW_COMMON_INCLUDES})

# PHY Packets Tests - Encoding/decoding validation, gap=0 bug regression
add_executable(PhyPacketsTests
    "${ASFW_TESTS_DIR}/PhyPacketsTests.cpp"
)

target_link_libraries(PhyPacketsTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(PhyPacketsTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(PhyPacketsTests PRIVATE ${ASFW_COMMON_INCLUDES})

# =============================================================================
# DMA Memory Tests - FakeDMAMemory unit tests
# =============================================================================
add_executable(DMAMemoryTests
    "${ASFW_TESTS_DIR}/DMAMemoryTests.cpp"
)

target_link_libraries(DMAMemoryTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(DMAMemoryTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(DMAMemoryTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(DMAMemoryTests)

# =============================================================================
# Descriptor Ring DMA Tests - Ring logic with fake DMA
# =============================================================================
add_executable(DescriptorRingDMATests
    "${ASFW_TESTS_DIR}/DescriptorRingDMATests.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/DescriptorRing.cpp"
)

target_link_libraries(DescriptorRingDMATests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(DescriptorRingDMATests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(DescriptorRingDMATests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(DescriptorRingDMATests)

# =============================================================================
# Buffer Ring DMA Tests - BufferRing logic with fake DMA
# =============================================================================
add_executable(BufferRingDMATests
    "${ASFW_TESTS_DIR}/BufferRingDMATests.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(BufferRingDMATests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(BufferRingDMATests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(BufferRingDMATests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(BufferRingDMATests)

# Label Allocator / tLabel lifecycle tests
add_executable(LabelAllocatorTests
    "${ASFW_TESTS_DIR}/LabelAllocatorTests.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(LabelAllocatorTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(LabelAllocatorTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(LabelAllocatorTests PRIVATE ${ASFW_COMMON_INCLUDES})

# FCP Packet Parsing Tests - Destination offset extraction and FCP response routing
add_executable(FCPPacketParsingTests
    "${ASFW_TESTS_DIR}/FCPPacketParsingTests.cpp"
)

target_link_libraries(FCPPacketParsingTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
)

target_compile_definitions(FCPPacketParsingTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(FCPPacketParsingTests PRIVATE ${ASFW_COMMON_INCLUDES})

# Packet Field Extraction Tests - Comprehensive tests for ALL packet field extraction
# (Created after sourceID byte-swap bug was discovered)
add_executable(PacketFieldExtractionTests
    "${ASFW_TESTS_DIR}/PacketFieldExtractionTests.cpp"
)

target_link_libraries(PacketFieldExtractionTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
)

target_compile_definitions(PacketFieldExtractionTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(PacketFieldExtractionTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(PacketFieldExtractionTests)

# Music Subunit Identifier Parser Tests
add_executable(MusicSubunitIdentifierParserTests
    "${ASFW_TESTS_DIR}/MusicSubunitIdentifierParserTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"

    "${ASFW_TESTS_DIR}/AsyncSubsystemStub.cpp"
    # Dependencies required by MusicSubunit.cpp (even if not used in test)
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Audio/AudioSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Camera/CameraSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp" # Stub for IOUserClient/DriverKit
)

target_link_libraries(MusicSubunitIdentifierParserTests
    PRIVATE
        asfw_packet_serdes # For FCPTransport dependencies if any
        GTest::gtest_main
)

target_compile_definitions(MusicSubunitIdentifierParserTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(MusicSubunitIdentifierParserTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(MusicSubunitIdentifierParserTests)

# AVC Info Block Tests - Phase 3: Recursive info block parsing
add_executable(AVCInfoBlockTests
    "${ASFW_TESTS_DIR}/AVCInfoBlockTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AVCInfoBlockTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AVCInfoBlockTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AVCInfoBlockTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AVCInfoBlockTests)

# ClusterInfo Parsing Tests - Tests for descriptor ClusterInfo/MusicPlugInfo parsing
add_executable(ClusterInfoParsingTests
    "${ASFW_TESTS_DIR}/ClusterInfoParsingTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(ClusterInfoParsingTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(ClusterInfoParsingTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ClusterInfoParsingTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(ClusterInfoParsingTests)

# ResponseSender Header Format Tests - OHCI AT format bug fix verification
add_executable(ResponseSenderHeaderFormatTests
    "${ASFW_TESTS_DIR}/ResponseSenderHeaderFormatTests.cpp"
)

target_link_libraries(ResponseSenderHeaderFormatTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(ResponseSenderHeaderFormatTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ResponseSenderHeaderFormatTests PRIVATE ${ASFW_COMMON_INCLUDES})


# Extended Stream Format Command Tests - TDD Phase 1
add_executable(ExtendedStreamFormatCommandTests
    "${ASFW_TESTS_DIR}/ExtendedStreamFormatCommandTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/ExtendedStreamFormatCommand.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(ExtendedStreamFormatCommandTests
    PRIVATE
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(ExtendedStreamFormatCommandTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ExtendedStreamFormatCommandTests PRIVATE ${ASFW_COMMON_INCLUDES})


# AVC Stream Format Command Tests - Format offset bug fix verification
# Uses real Apogee Duet response data from FWA/discovery.txt
add_executable(AVCStreamFormatCommandTests
    "${ASFW_TESTS_DIR}/AVCStreamFormatCommandTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AVCStreamFormatCommandTests
    PRIVATE
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(AVCStreamFormatCommandTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AVCStreamFormatCommandTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AVCStreamFormatCommandTests)


# Audio Function Block Command Tests - TDD Phase 1
add_executable(AudioFunctionBlockCommandTests
    "${ASFW_TESTS_DIR}/AudioFunctionBlockCommandTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AudioFunctionBlockCommandTests
    PRIVATE
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(AudioFunctionBlockCommandTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AudioFunctionBlockCommandTests PRIVATE ${ASFW_COMMON_INCLUDES})


# Music Subunit Tests - TDD Phase 1 (Integration)
add_executable(MusicSubunitTests
    "${ASFW_TESTS_DIR}/MusicSubunitTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"

    # We need FCPTransport stub or similar if we link AVCUnit
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_TESTS_DIR}/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Audio/AudioSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Camera/CameraSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
)

target_link_libraries(MusicSubunitTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(MusicSubunitTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(MusicSubunitTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(MusicSubunitTests)

# Stream Format Parser Tests - Legacy format support
add_executable(StreamFormatParserTests
    "${ASFW_TESTS_DIR}/StreamFormatParserTests.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(StreamFormatParserTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(StreamFormatParserTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(StreamFormatParserTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(StreamFormatParserTests)

# AsyncSubsystem Accessor Tests - Coverage improvement for simple getters
add_executable(AsyncSubsystemAccessorTests
    "${ASFW_TESTS_DIR}/AsyncSubsystemAccessorTests.cpp"
    "${ASFW_TESTS_DIR}/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AsyncSubsystemAccessorTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AsyncSubsystemAccessorTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AsyncSubsystemAccessorTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AsyncSubsystemAccessorTests)

# MusicSubunit Capabilities Tests - Coverage for inline helper methods
add_executable(MusicSubunitCapabilitiesTests
    "${ASFW_TESTS_DIR}/MusicSubunitCapabilitiesTests.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(MusicSubunitCapabilitiesTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(MusicSubunitCapabilitiesTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(MusicSubunitCapabilitiesTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(MusicSubunitCapabilitiesTests)



# AVCHandler Tests - Driver-UI Communication
add_executable(AVCHandlerTests
    "${ASFW_TESTS_DIR}/AVCHandlerTests.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/Handlers/AVCHandler.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Audio/AudioSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Camera/CameraSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_TESTS_DIR}/AsyncSubsystemStub.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AVCHandlerTests
    PRIVATE
        asfw_packet_serdes
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(AVCHandlerTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AVCHandlerTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AVCHandlerTests)

# AVCCapabilitiesSerializerTests
add_executable(AVCCapabilitiesSerializerTests
    "${ASFW_TESTS_DIR}/AVCCapabilitiesSerializerTests.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/Handlers/AVCHandler.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Audio/AudioSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Camera/CameraSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_TESTS_DIR}/AsyncSubsystemStub.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)
target_link_libraries(AVCCapabilitiesSerializerTests PRIVATE
    GTest::gtest_main
    GTest::gmock
    asfw_packet_serdes
)
target_compile_definitions(AVCCapabilitiesSerializerTests
    PRIVATE
        ASFW_HOST_TEST
)
target_include_directories(AVCCapabilitiesSerializerTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AVCCapabilitiesSerializerTests)

# AVC Unit Plug Info Command Tests
add_executable(AVCUnitPlugInfoCommandTests
    "${ASFW_TESTS_DIR}/AVCUnitPlugInfoCommandTests.cpp"
)

target_link_libraries(AVCUnitPlugInfoCommandTests
    PRIVATE
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(AVCUnitPlugInfoCommandTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AVCUnitPlugInfoCommandTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AVCUnitPlugInfoCommandTests)

# Isoch Receive Context Tests
add_executable(IsochReceiveContextTests
    "${ASFW_TESTS_DIR}/IsochReceiveContextTests.cpp"
    "${ASFW_TESTS_DIR}/IsochRxDmaRingTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochReceiveContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochRxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Receive/IsochAudioRxPipeline.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/DescriptorRing.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(IsochReceiveContextTests
    PRIVATE
        GTest::gtest_main
        GTest::gmock
)

target_compile_definitions(IsochReceiveContextTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(IsochReceiveContextTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(IsochReceiveContextTests)

# Isoch DMA Memory Manager Tests
add_executable(IsochDMAMemoryManagerTests
    "${ASFW_TESTS_DIR}/IsochDMAMemoryManagerTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Memory/IsochDMAMemoryManager.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/DMAMemoryManager.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
)

target_link_libraries(IsochDMAMemoryManagerTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(IsochDMAMemoryManagerTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(IsochDMAMemoryManagerTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(IsochDMAMemoryManagerTests)

# =============================================================================
# Phase 1.5: AM824/AMDTP Encoding Tests
# =============================================================================

# AM824 Encoder Tests
add_executable(AM824EncoderTests
    "${ASFW_TESTS_DIR}/AM824EncoderTests.cpp"
)

target_link_libraries(AM824EncoderTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AM824EncoderTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AM824EncoderTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AM824EncoderTests)

# CIP Header Builder Tests
add_executable(CIPHeaderBuilderTests
    "${ASFW_TESTS_DIR}/CIPHeaderBuilderTests.cpp"
)

target_link_libraries(CIPHeaderBuilderTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(CIPHeaderBuilderTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(CIPHeaderBuilderTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(CIPHeaderBuilderTests)

# TX Verifier Decode Tests (dev-only verifier helpers)
add_executable(TxVerifierDecodeTests
    "${ASFW_TESTS_DIR}/TxVerifierDecodeTests.cpp"
)

target_link_libraries(TxVerifierDecodeTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(TxVerifierDecodeTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(TxVerifierDecodeTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(TxVerifierDecodeTests)

# Blocking Cadence Tests (48 kHz)
add_executable(BlockingCadenceTests
    "${ASFW_TESTS_DIR}/BlockingCadenceTests.cpp"
)

target_link_libraries(BlockingCadenceTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(BlockingCadenceTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(BlockingCadenceTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(BlockingCadenceTests)

# Non-Blocking Cadence Tests (48 kHz)
add_executable(NonBlockingCadenceTests
    "${ASFW_TESTS_DIR}/NonBlockingCadenceTests.cpp"
)

target_link_libraries(NonBlockingCadenceTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(NonBlockingCadenceTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(NonBlockingCadenceTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(NonBlockingCadenceTests)

# Blocking DBC Generator Tests
add_executable(BlockingDbcTests
    "${ASFW_TESTS_DIR}/BlockingDbcTests.cpp"
)

target_link_libraries(BlockingDbcTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(BlockingDbcTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(BlockingDbcTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(BlockingDbcTests)

# Audio Ring Buffer Tests
add_executable(AudioRingBufferTests
    "${ASFW_TESTS_DIR}/AudioRingBufferTests.cpp"
)

target_link_libraries(AudioRingBufferTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AudioRingBufferTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AudioRingBufferTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AudioRingBufferTests)

# Audio Driver config policy tests (pure bring-up/config behavior)
add_executable(AudioDriverConfigPolicyTests
    "${ASFW_TESTS_DIR}/AudioDriverConfigPolicyTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Audio/AudioDriverConfigPolicy.cpp"
)

target_link_libraries(AudioDriverConfigPolicyTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AudioDriverConfigPolicyTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AudioDriverConfigPolicyTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AudioDriverConfigPolicyTests)

# Packet Assembler Integration Tests
add_executable(PacketAssemblerTests
    "${ASFW_TESTS_DIR}/PacketAssemblerTests.cpp"
)

target_link_libraries(PacketAssemblerTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(PacketAssemblerTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(PacketAssemblerTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(PacketAssemblerTests)

# Audio IO path tests (host-side queue/ring behavior without ADK runtime)
add_executable(AudioIOPathTests
    "${ASFW_TESTS_DIR}/AudioIOPathTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Audio/AudioIOPath.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(AudioIOPathTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(AudioIOPathTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(AudioIOPathTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(AudioIOPathTests)

# Isoch Transmit Context Tests
add_executable(IsochTransmitContextTests
    "${ASFW_TESTS_DIR}/IsochTransmitContextTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTransmitContext.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochAudioTxPipeline.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDmaRing.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxVerifier.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxRecoveryController.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Encoding/SYTGenerator.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
    "${ASFW_TESTS_DIR}/HardwareInterfaceStub.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(IsochTransmitContextTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(IsochTransmitContextTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(IsochTransmitContextTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(IsochTransmitContextTests)

# Isoch TX Descriptor Slab Addressing Tests
add_executable(IsochTxDescriptorSlabTests
    "${ASFW_TESTS_DIR}/IsochTxDescriptorSlabTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Transmit/IsochTxDescriptorSlab.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(IsochTxDescriptorSlabTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(IsochTxDescriptorSlabTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(IsochTxDescriptorSlabTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(IsochTxDescriptorSlabTests)

# SYT generator tests
add_executable(SYTGeneratorTests
    "${ASFW_TESTS_DIR}/SYTGeneratorTests.cpp"
    "${ASFW_DRIVER_DIR}/Isoch/Encoding/SYTGenerator.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_link_libraries(SYTGeneratorTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(SYTGeneratorTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(SYTGeneratorTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(SYTGeneratorTests)

# External sync bridge state machine tests
add_executable(ExternalSyncBridgeTests
    "${ASFW_TESTS_DIR}/ExternalSyncBridgeTests.cpp"
)

target_link_libraries(ExternalSyncBridgeTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(ExternalSyncBridgeTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ExternalSyncBridgeTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(ExternalSyncBridgeTests)

# External sync discipline (48k) tests
add_executable(ExternalSyncDiscipline48kTests
    "${ASFW_TESTS_DIR}/ExternalSyncDiscipline48kTests.cpp"
)

target_link_libraries(ExternalSyncDiscipline48kTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(ExternalSyncDiscipline48kTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ExternalSyncDiscipline48kTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(ExternalSyncDiscipline48kTests)

# SimITEngine Tests (Hardware-grade offline testing)
add_executable(SimITEngineTests
    "${ASFW_TESTS_DIR}/SimITEngineTests.cpp"
)

target_link_libraries(SimITEngineTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(SimITEngineTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(SimITEngineTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(SimITEngineTests)

# Device protocol factory routing tests
add_executable(DeviceProtocolFactoryTests
    "${ASFW_TESTS_DIR}/DeviceProtocolFactoryTests.cpp"
)

target_link_libraries(DeviceProtocolFactoryTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(DeviceProtocolFactoryTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(DeviceProtocolFactoryTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(DeviceProtocolFactoryTests)

# Apogee protocol boolean control mapping tests
add_executable(ApogeeBooleanControlMappingTests
    "${ASFW_TESTS_DIR}/ApogeeBooleanControlMappingTests.cpp"
)

target_link_libraries(ApogeeBooleanControlMappingTests
    PRIVATE
        GTest::gtest_main
)

target_compile_definitions(ApogeeBooleanControlMappingTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ApogeeBooleanControlMappingTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(ApogeeBooleanControlMappingTests)

# =============================================================================
# Config ROM Tests (IEEE 1212 / TA 1999027)
# =============================================================================

add_library(asfw_configrom_host STATIC
    "${ASFW_DRIVER_DIR}/ConfigROM/ConfigROMBuilder.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/ROMReader.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/ConfigROMStore.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_include_directories(asfw_configrom_host PUBLIC ${ASFW_COMMON_INCLUDES})
target_compile_features(asfw_configrom_host PUBLIC cxx_std_23)
target_compile_definitions(asfw_configrom_host PUBLIC ASFW_HOST_TEST)

add_executable(ASFWConfigROMTests
    "${ASFW_TESTS_DIR}/ConfigROMBuilderTests.cpp"
    "${ASFW_TESTS_DIR}/ConfigROMBIBParseTests.cpp"
    "${ASFW_TESTS_DIR}/TextDescriptorLeafParseTests.cpp"
    "${ASFW_TESTS_DIR}/ROMReaderHeaderFirstTests.cpp"
    "${ASFW_TESTS_DIR}/BusOptionsFieldsTests.cpp"
)

target_link_libraries(ASFWConfigROMTests
    PRIVATE
        asfw_configrom_host
        GTest::gtest_main
)

target_compile_definitions(ASFWConfigROMTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ASFWConfigROMTests PRIVATE ${ASFW_COMMON_INCLUDES})

# =============================================================================
# Discovery Subsystem Tests
# =============================================================================

add_library(asfw_discovery_host STATIC
    "${ASFW_DRIVER_DIR}/ConfigROM/ROMScanner.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/ROMReader.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/SpeedPolicy.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/ConfigROMStore.cpp"
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
)

target_include_directories(asfw_discovery_host PUBLIC ${ASFW_COMMON_INCLUDES})
target_compile_features(asfw_discovery_host PUBLIC cxx_std_23)
target_compile_definitions(asfw_discovery_host PUBLIC ASFW_HOST_TEST)

add_executable(ROMScannerCompletionTests
    "${ASFW_TESTS_DIR}/ROMScannerCompletionTests.cpp"
)

target_link_libraries(ROMScannerCompletionTests
    PRIVATE
        asfw_discovery_host
        GTest::gtest_main
)

target_compile_definitions(ROMScannerCompletionTests
    PRIVATE
        ASFW_HOST_TEST
)

target_include_directories(ROMScannerCompletionTests PRIVATE ${ASFW_COMMON_INCLUDES})

gtest_discover_tests(ROMScannerCompletionTests)
