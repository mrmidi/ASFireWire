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
)

# DICE restart-session tests
add_device_test(DICERestartSessionTests
    DICERestartSessionTests.cpp
)

# DICE duplex restart coordinator FSM tests
add_device_test(DiceDuplexRestartCoordinatorTests
    DiceDuplexRestartCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Backends/DiceDuplexRestartCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Core/AudioRuntimeRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
)

# FW-74: resolver-owned duplex stream geometry, wire-format quirks, and host recipe.
add_device_test(DuplexStreamProfileTests
    DuplexStreamProfileTests.cpp
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

# Apogee protocol boolean control mapping tests
add_device_test(ApogeeBooleanControlMappingTests
    ApogeeBooleanControlMappingTests.cpp
)

# Apogee Duet vendor command encoding/decoding tests (previously orphaned)
add_device_test(ApogeeDuetVendorCmdTests
    ApogeeDuetVendorCmdTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/AVC/CMP/CMPClient.cpp"
)
