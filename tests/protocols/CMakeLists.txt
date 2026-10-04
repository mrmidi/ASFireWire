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

# AVC Codec Tests (Core and Commands)
add_protocols_test(AvcCodecTests
    AvcCodecTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
)

# AV/C CCM codecs: SIGNAL SOURCE, INPUT SELECT, OUTPUT PRESET, CCM PROFILE (TA 2002010)
add_protocols_test(CcmCodecTests
    CcmCodecTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
)

# AV/C Music subunit commands (TA 2001007 §7) and the identifier descriptor (§5)
add_protocols_test(MusicCommandTests
    MusicCommandTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
)

# AV/C Audio subunit FUNCTION BLOCK (TA 1999008 §10) and CHANGE CONFIGURATION (§11.1)
add_protocols_test(AudioFunctionBlockTests
    AudioFunctionBlockTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
)

# Naming layer and the discovery log: every wire value by spec name, UNKNOWN for the unnamed
add_protocols_test(DiscoveryLogTests
    DiscoveryLogTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
)

# AVC Hardware Fixture Tests (Phase 88 / Duet)
add_protocols_test(AvcFixtureTests
    AvcFixtureTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
)

# AVC Differential Tests (Legacy vs New ASFW::AVC Codecs)

# AVC Simulated Unit Tests
add_protocols_test(AvcSimulatedUnitTests
    AvcSimulatedUnitTests.cpp
    "${ASFW_TESTS_DIR}/support/SimulatedAvcUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
)

# AVC Golden Wire Characterization Tests (Phase 2a)
add_protocols_test(AvcGoldenTests
    AvcGoldenTests.cpp
    "${ASFW_DRIVER_DIR}/UserClient/Handlers/AVCHandler.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/WireFormats/AVCMusicCapabilities.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/WireFormats/AVCDiscoveryDocument.cpp"
    "${ASFW_TESTS_DIR}/support/SimulatedAvcUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/AVC/AvcAudioConfig.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/AVC/AvcExtensionInventory.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Graph/AvcGraphBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BridgeCoInventory.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BeBoBProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
)

# AVCDiscovery lifecycle (attach, bus reset, resume, rescan)
add_protocols_test(AvcDiscoveryLifecycleTests
    AvcDiscoveryLifecycleTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCDiscovery.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderCue.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparation.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparationCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AudioProfileRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/MOTU/MotuV2Profile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MackieOnyx400FProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/GenericAvcProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/AVC/MAudioSpecialProfile.cpp"
    "${ASFW_DRIVER_DIR}/Audio/DriverKit/Config/DICE/DiceProfile.cpp"
    "${ASFW_TESTS_DIR}/support/SimulatedAvcUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/AVC/AvcAudioConfig.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/AVC/AvcExtensionInventory.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Graph/AvcGraphBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BridgeCoInventory.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BeBoBProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
)

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

# BridgeCo inventory: channel-section probe and facts from the snapshot
add_protocols_test(BridgeCoInventoryTests
    BridgeCoInventoryTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Commands/StreamFormatCommand.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Extensions/BridgeCoPlugInfo.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BridgeCoInventory.cpp"
)

# M-Audio bootloader cue codec and preparation FSM (pure host logic only).
add_protocols_test(BeBoBBootloaderPreparationTests
    BeBoBBootloaderPreparationTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderCue.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparation.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparationCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)
target_link_libraries(BeBoBBootloaderPreparationTests PRIVATE asfw_catalog_host)

# FCP transport lifecycle tests (write completion, cancellable timeout, teardown)
add_protocols_test(AvcAudioConfigTests
    AvcAudioConfigTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/AVC/AvcAudioConfig.cpp"
)

add_protocols_test(AVCExchangeLogWireTests
    AVCExchangeLogWireTests.cpp
)

add_protocols_test(FCPTransportTests
    FCPTransportTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FCP response CSR range and transport-lifetime routing tests
add_protocols_test(FCPResponseRouterTests
    FCPResponseRouterTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# Bounded M-Audio special-firmware clock command and allowlist compatibility
add_protocols_test(MAudioSpecialCommandTests
    MAudioSpecialCommandTests.cpp
)

# CMP device leases, MPR validation, and compare-swap contention tests
add_protocols_test(CMPConnectionTests
    CMPConnectionTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)

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

# Audio Subunit Descriptor Tests
add_protocols_test(AudioSubunitDescriptorTests
    AudioSubunitDescriptorTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
)

# Music Subunit Descriptor Tests
add_protocols_test(MusicSubunitDescriptorTests
    MusicSubunitDescriptorTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
)

# AVC Graph Builder Tests
add_protocols_test(AvcGraphBuilderTests
    AvcGraphBuilderTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Graph/AvcGraphBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
)

# Stream Format Parser Tests

# Music Subunit Capabilities Tests

# AVCHandler Tests
add_executable(AVCHandlerTests
    AVCHandlerTests.cpp
    "../support/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/Handlers/AVCHandler.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/WireFormats/AVCMusicCapabilities.cpp"
    "${ASFW_DRIVER_DIR}/UserClient/WireFormats/AVCDiscoveryDocument.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AVCUnit.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Graph/AvcGraphBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/DescriptorAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/MusicSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AudioSubunitDescriptor.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Descriptors/AVCInfoBlock.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
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

add_protocols_test(PCRCodecTests
    PCRCodecTests.cpp
)

add_protocols_test(DVCaptureSinkTests
    DVCaptureSinkTests.cpp
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

# SCSI HBA login-up readiness gate (childless-target HW finding, 2026-07-31)
add_protocols_test(TargetReadinessGateTests
    TargetReadinessGateTests.cpp
    "${ASFW_DRIVER_DIR}/SCSIController/TargetReadinessGate.cpp"
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

# SCSI HBA target-ID and per-task decisions (header-only)
add_protocols_test(HbaTaskPolicyTests
    HbaTaskPolicyTests.cpp
)

# SCSI HBA target-0 create/destroy edge handling
add_protocols_test(TargetLifecycleTests
    TargetLifecycleTests.cpp
    "${ASFW_DRIVER_DIR}/SCSIController/TargetLifecycle.cpp"
)

# SCSI HBA <-> SBP-2 bridge, end to end over the real session stack
add_protocols_test(SBP2TargetBridgeTests
    SBP2TargetBridgeTests.cpp
    "${ASFW_DRIVER_DIR}/SCSIController/SBP2TargetBridge.cpp"
    "${ASFW_DRIVER_DIR}/SCSIController/SBP2BridgeHub.cpp"
    "${ASFW_DRIVER_DIR}/SCSIController/TargetReadinessGate.cpp"
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

# SBP-2 nub publisher over the real DeviceManager, ASFWDriver replaced by a fake IOService
add_protocols_test(SBP2NubPublisherTests
    SBP2NubPublisherTests.cpp
    "${ASFW_DRIVER_DIR}/SCSIController/SBP2NubPublisher.cpp"
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

# The per-device allowlist that bounds what may reach firmware which hangs on
# AV/C it does not implement. Header-only match rule plus the tables;
# enforcement lives in FCPTransport::SubmitCommand.
add_protocols_test(AVCCommandFilterTests
    AVCCommandFilterTests.cpp
)

# M-Audio special firmware stream formation geometry
add_protocols_test(MAudioSpecialFormationTests
    MAudioSpecialFormationTests.cpp
)

# Linux M-Audio post-start directional rate reassertion timing policy.
add_protocols_test(MAudioSpecialStartPolicyTests
    MAudioSpecialStartPolicyTests.cpp
)

add_protocols_test(DriverKitSessionSchedulerTeardownTests
    DriverKitSessionSchedulerTeardownTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/SBP2/Session/DriverKitSessionScheduler.cpp"
)

add_protocols_test(CompletionQueueTeardownTests CompletionQueueTeardownTests.cpp)

add_protocols_test(AvcFeatureControlTests
    AvcFeatureControlTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp"
)
