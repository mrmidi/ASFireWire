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
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Focusrite/SaffireproCommon.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Focusrite/SPro24DspTypes.cpp"
)

# DICE duplex parity tests (requires support dir in includes for ReferencePhase0ParityFixture.inc)
add_executable(DICEDuplexBringupControllerTests
    DICEDuplexBringupControllerTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Core/DICETransaction.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Core/DICEDuplexBringupController.cpp"
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
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Core/DICETransaction.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Core/DICEDuplexBringupController.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/TCAT/DICETcatProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Focusrite/SPro24DspProtocol.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Focusrite/SPro24DspTypes.cpp"
    "${ASFW_DRIVER_DIR}/Protocols/Audio/DICE/Focusrite/SaffireproCommon.cpp"
)

# DICE restart-session tests
add_device_test(DICERestartSessionTests
    DICERestartSessionTests.cpp
)

# DICE duplex restart coordinator FSM tests
add_device_test(DiceDuplexRestartCoordinatorTests
    DiceDuplexRestartCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/Audio/Backends/DiceDuplexRestartCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Core/AudioRuntimeRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/DeviceRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMClient.cpp"
)

# Apogee protocol boolean control mapping tests
add_device_test(ApogeeBooleanControlMappingTests
    ApogeeBooleanControlMappingTests.cpp
)

# Apogee Duet vendor command encoding/decoding tests (previously orphaned)
add_device_test(ApogeeDuetVendorCmdTests
    ApogeeDuetVendorCmdTests.cpp
    "${ASFW_DRIVER_DIR}/Protocols/Audio/Oxford/Apogee/ApogeeDuetProtocol.cpp"
)


