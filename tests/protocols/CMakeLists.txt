# tests/protocols/CMakeLists.txt

macro(add_protocols_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

macro(add_protocols_mock_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
        GTest::gmock
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# FCP Packet Parsing Tests
add_executable(FCPPacketParsingTests
    FCPPacketParsingTests.cpp
)
target_link_libraries(FCPPacketParsingTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(FCPPacketParsingTests)

# Music Subunit Identifier Parser Tests
add_executable(MusicSubunitIdentifierParserTests
    MusicSubunitIdentifierParserTests.cpp
    "../support/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"
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
)
target_link_libraries(MusicSubunitIdentifierParserTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(MusicSubunitIdentifierParserTests)

# AVC Info Block Tests
add_protocols_test(AVCInfoBlockTests
    AVCInfoBlockTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
)

# ClusterInfo Parsing Tests
add_protocols_test(ClusterInfoParsingTests
    ClusterInfoParsingTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
)

# Extended Stream Format Command Tests
add_protocols_mock_test(ExtendedStreamFormatCommandTests
    ExtendedStreamFormatCommandTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/ExtendedStreamFormatCommand.cpp"
)

# AVC Stream Format Command Tests
add_protocols_mock_test(AVCStreamFormatCommandTests
    AVCStreamFormatCommandTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
)

# Audio Function Block Command Tests
add_protocols_mock_test(AudioFunctionBlockCommandTests
    AudioFunctionBlockCommandTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
)

# Music Subunit Tests
add_executable(MusicSubunitTests
    MusicSubunitTests.cpp
    "../support/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Music/MusicSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Audio/AudioSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Camera/CameraSubunit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
)
target_link_libraries(MusicSubunitTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(MusicSubunitTests)

# Stream Format Parser Tests
add_protocols_test(StreamFormatParserTests
    StreamFormatParserTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/StreamFormats/StreamFormatParser.cpp"
)

# Music Subunit Capabilities Tests
add_protocols_test(MusicSubunitCapabilitiesTests
    MusicSubunitCapabilitiesTests.cpp
)

# AVCHandler Tests
add_executable(AVCHandlerTests
    AVCHandlerTests.cpp
    "../support/AsyncSubsystemStub.cpp"
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
)
target_link_libraries(AVCHandlerTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(AVCHandlerTests)

# AVCCapabilitiesSerializerTests
add_executable(AVCCapabilitiesSerializerTests
    AVCCapabilitiesSerializerTests.cpp
    "../support/AsyncSubsystemStub.cpp"
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
)
target_link_libraries(AVCCapabilitiesSerializerTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(AVCCapabilitiesSerializerTests)

# AVC Unit Plug Info Command Tests
add_protocols_mock_test(AVCUnitPlugInfoCommandTests
    AVCUnitPlugInfoCommandTests.cpp
)

# SBP2 ORB Tests
add_protocols_test(SBP2ORBTests
    SBP2ORBTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2CommandORB.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2ManagementORB.cpp"
)
