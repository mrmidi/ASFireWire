# tests/devices/CMakeLists.txt

macro(add_device_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# DICE / Focusrite protocol serialization tests
add_device_test(DiceFocusriteSerializationTests
    DiceFocusriteSerializationTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Focusrite/SaffireproCommon.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Focusrite/SPro24DspTypes.cpp"
)

# DICE duplex parity tests (requires support dir in includes for ReferencePhase0ParityFixture.inc)
add_executable(DICEDuplexBringupControllerTests
    DICEDuplexBringupControllerTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Core/DICETransaction.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Core/DICEDuplexBringupController.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)
target_include_directories(DICEDuplexBringupControllerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
target_link_libraries(DICEDuplexBringupControllerTests PRIVATE
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(DICEDuplexBringupControllerTests)

# Generic DICE / TCAT protocol split tests
add_device_test(DICETcatProtocolTests
    DICETcatProtocolTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Core/DICETransaction.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Core/DICEDuplexBringupController.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/TCAT/DICETcatProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Focusrite/SPro24DspTypes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/DICE/Focusrite/SaffireproCommon.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)

# DICE restart-session tests
add_device_test(DICERestartSessionTests
    DICERestartSessionTests.cpp
)

# DICE duplex restart coordinator FSM tests
add_device_test(AudioDuplexCoordinatorTests
    AudioDuplexCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Duplex/AudioDuplexCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Core/AudioRuntimeRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
)

# FW-74: resolver-owned duplex stream geometry, wire-format quirks, and host recipe.
add_device_test(DuplexStreamProfileTests
    DuplexStreamProfileTests.cpp
)

# BeBoBProtocol base-class: async ApplyClockConfig chain, timer settle, epoch cancel.
add_device_test(BeBoBProtocolTests
    BeBoBProtocolTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/BeBoB/BeBoBProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/AudioFunctionBlockCommand.cpp"
)

# FW-65: sync/async bridge (WaitForAsyncResult) characterization tests (header-only)
add_device_test(WaitForAsyncResultTests
    WaitForAsyncResultTests.cpp
)

# DICE recovery-policy classification kernel tests (FW-66, header-only policy)
add_device_test(DiceRecoveryPolicyTests
    DiceRecoveryPolicyTests.cpp
)

# DICE duplex operation gate (active/stop-intent) tests (FW-68, header-only gate)
add_device_test(DuplexOperationGateTests
    DuplexOperationGateTests.cpp
)

# DICE restart-journal FSM mutator tests (FW-69a, header-only journal)
add_device_test(RestartJournalTests
    RestartJournalTests.cpp
)

# DICE restart-session store tests (FW-69b, header-only lock-borrowing store)
add_device_test(RestartSessionStoreTests
    RestartSessionStoreTests.cpp
)

# DICE clock-request broker tests (FW-67, header-only lock-borrowing broker)
add_device_test(ClockRequestBrokerTests
    ClockRequestBrokerTests.cpp
)

# FW-130: Apogee Duet device spec (control caps + CIP quirks), header-only
add_device_test(ApogeeCapsTests
    ApogeeCapsTests.cpp
)

# FW-126: OXFW-common vendor framing template + Apogee code table
add_device_test(OxfwVendorFramingTests
    OxfwVendorFramingTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
)

# FW-129: Duet FCP dispatch and meter register reads
add_device_test(ApogeeTransportTests
    ApogeeTransportTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FW-128: Duet params serialization (pure, no transport)
add_device_test(ApogeeParamsSerdesTests
    ApogeeParamsSerdesTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
)

# FW-128: OXFW-common params cache/diff engine (header-only template)
add_device_test(OxfwParamsEngineTests
    OxfwParamsEngineTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FW-137: Oxford FW970/971 chip-common CSR reads
add_device_test(OxfordCsrTests
    OxfordCsrTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FW-139: OXFW two-tier stream-format discovery (reported list vs probed rates)
add_device_test(OxfwStreamFormatsTests
    OxfwStreamFormatsTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfwStreamFormats.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# FW-138: shared AV/C test rig (deferred bus + real FCPTransport + target responder)
add_device_test(AvcTestRigTests
    AvcTestRigTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# Apogee Duet vendor command encoding/decoding tests (previously orphaned)
add_device_test(ApogeeDuetVendorCmdTests
    ApogeeDuetVendorCmdTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# M-Audio bootloader cue: frozen bytes, write choke point, single-shot machine
add_device_test(BeBoBBootloaderCueTests
    BeBoBBootloaderCueTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/Bootloader/BeBoBBootloaderCue.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/Bootloader/BeBoBBootloaderPreparation.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/Bootloader/BeBoBBootloaderClient.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
)

# M-Audio 1814 / ProjectMix: pure custom duplex choreography before wiring it
# into the generic BeBoB coordinator or live transport.
add_device_test(MAudioDuplexChoreographyTests
    MAudioDuplexChoreographyTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Families/BeBoB/MAudio/MAudioDuplexChoreography.cpp"
)

# Apogee Duet runtime stream caps: aggregate vs per-stream wire view
add_device_test(ApogeeDuetStreamCapsTests
    ApogeeDuetStreamCapsTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)

# Apogee Duet clock transition tests
add_device_test(ApogeeDuetClockTransitionTests
    ApogeeDuetClockTransitionTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetDuplex.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/OxfordCsr.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeTransport.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/FCPTransport.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWDevice.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/FWUnit.cpp"
)
