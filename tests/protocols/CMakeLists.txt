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

# BridgeCo / BeBoB STATUS-only inventory codec tests
add_protocols_test(BridgeCoReadOnlyProbeTests
    BridgeCoReadOnlyProbeTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/BridgeCo/BridgeCoReadOnlyProbe.cpp"
)

# FCP transport lifecycle tests (write completion, cancellable timeout, teardown)
add_protocols_test(FCPTransportTests
    FCPTransportTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FCP response CSR range and transport-lifetime routing tests
add_protocols_test(FCPResponseRouterTests
    FCPResponseRouterTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# CMP device leases, MPR validation, and compare-swap contention tests
add_protocols_test(CMPConnectionTests
    CMPConnectionTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
)

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

# SBP2 Session Scheduler Tests (ISessionScheduler interface + virtual-clock fake)
add_protocols_test(SessionSchedulerTests
    SessionSchedulerTests.cpp
)

# SBP2 FetchAgent Tests (command-plane ORB submission/chaining/completion)
add_protocols_test(FetchAgentTests
    FetchAgentTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/FetchAgent.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2CommandORB.cpp"
)

# SBP2 LoginSession Tests (login/reconnect/logout state machine + status routing)
add_protocols_test(LoginSessionTests
    LoginSessionTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/LoginSession.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/FetchAgent.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2CommandORB.cpp"
)

# SBP2 SessionRegistry Tests (identity/lifecycle + per-record CommandExecutor)
add_protocols_test(SessionRegistryTests
    SessionRegistryTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/SessionRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/CommandExecutor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/LoginSession.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/FetchAgent.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2CommandORB.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2ManagementORB.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceManager.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/SpeedPolicy.cpp"
)

# SBP2Handler Tests (user-client boundary: session state + command ABI hardening)
add_protocols_test(SBP2HandlerTests
    SBP2HandlerTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/SessionRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/CommandExecutor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/LoginSession.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/FetchAgent.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2CommandORB.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/SBP2ManagementORB.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceManager.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/SpeedPolicy.cpp"
)
