# tests/common/CMakeLists.txt

macro(add_common_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# CSR Contract Verifier Tests
add_common_test(CSRContractVerifierTests
    CSRContractVerifierTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/CSR/CSRContractVerifier.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/CSRResponder.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapService.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/SpeedMapService.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalIRMResourceController.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalCSRAccessor.cpp"
)

# CSR Responder Contract Tests
add_executable(CSRResponderContractTests
    CSRResponderContractTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/CSR/CSRResponder.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapService.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/SpeedMapService.cpp"
)
target_link_libraries(CSRResponderContractTests PRIVATE
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(CSRResponderContractTests)

# CSR Responder Tests
add_common_test(CSRResponderTests
    CSRResponderTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/CSR/CSRResponder.cpp"
)

# Capability Mode Tests
add_common_test(CapabilityModeTests
    CapabilityModeTests.cpp
)

# Local Request Dispatch Tests
add_executable(LocalRequestDispatchTests
    LocalRequestDispatchTests.cpp
    "${ASFW_DRIVER_DIR}/Async/Rx/LocalRequestDispatch.cpp"
)
target_link_libraries(LocalRequestDispatchTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(LocalRequestDispatchTests)

# FireWire IOReturn Tests
add_common_test(FireWireIOReturnTests
    FireWireIOReturnTests.cpp
)

# Transaction Storage Tests
add_common_test(TransactionStorageTests
    TransactionStorageTests.cpp
    "${ASFW_DRIVER_DIR}/UserClient/Storage/TransactionStorage.cpp"
)

# Address Space Manager Tests
add_common_test(AddressSpaceManagerTests
    AddressSpaceManagerTests.cpp
)

# Diagnostics Service & ABI Invariants Tests
add_executable(DiagnosticsServiceTests
    DiagnosticsServiceTests.cpp
)
target_link_libraries(DiagnosticsServiceTests PRIVATE
    asfw_test_support
    GTest::gtest_main
)
target_include_directories(DiagnosticsServiceTests PRIVATE "${ASFW_DRIVER_DIR}/Shared")
gtest_discover_tests(DiagnosticsServiceTests)

# Device Protocol Factory Routing Tests
add_common_test(DeviceProtocolFactoryTests
    DeviceProtocolFactoryTests.cpp
)

# Speed Map Service Tests
add_common_test(SpeedMapServiceTests
    SpeedMapServiceTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/CSR/SpeedMapService.cpp"
)

# Root runtime lifecycle state-machine tests
add_common_test(ControllerStateMachineTests
    ControllerStateMachineTests.cpp
    "${ASFW_DRIVER_DIR}/Controller/ControllerStateMachine.cpp"
)
