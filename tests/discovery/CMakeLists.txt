# tests/discovery/CMakeLists.txt

macro(add_discovery_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# ASFW Config ROM Tests (links to configrom host library)
add_executable(ASFWConfigROMTests
    ConfigROMBuilderTests.cpp
    ConfigROMBIBParseTests.cpp
    ConfigROMStoreConcurrencyTests.cpp
    TextDescriptorLeafParseTests.cpp
    ROMReaderHeaderFirstTests.cpp
    BusOptionsFieldsTests.cpp
)
target_link_libraries(ASFWConfigROMTests PRIVATE
    asfw_configrom_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ASFWConfigROMTests)

# ROMScanner Completion Tests
add_executable(ROMScannerCompletionTests
    ROMScannerCompletionTests.cpp
)
target_link_libraries(ROMScannerCompletionTests PRIVATE
    asfw_discovery_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ROMScannerCompletionTests)

# ROMScanner MultiNode FSM Tests
add_executable(ROMScannerMultiNodeFSMTests
    ROMScannerMultiNodeFSMTests.cpp
)
target_link_libraries(ROMScannerMultiNodeFSMTests PRIVATE
    asfw_discovery_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ROMScannerMultiNodeFSMTests)

# ROMScanner IRM Verify Tests
add_executable(ROMScannerIRMVerifyTests
    ROMScannerIRMVerifyTests.cpp
)
target_link_libraries(ROMScannerIRMVerifyTests PRIVATE
    asfw_discovery_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ROMScannerIRMVerifyTests)

# ROMScanner Abort Tests
add_executable(ROMScannerAbortTests
    ROMScannerAbortTests.cpp
)
target_link_libraries(ROMScannerAbortTests PRIVATE
    asfw_discovery_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ROMScannerAbortTests)

# ROMScanner Details Discovery Tests
add_executable(ROMScannerDetailsDiscoveryTests
    ROMScannerDetailsDiscoveryTests.cpp
)
target_link_libraries(ROMScannerDetailsDiscoveryTests PRIVATE
    asfw_discovery_host
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ROMScannerDetailsDiscoveryTests)

# Discovery Convergence Tests
add_discovery_test(DiscoveryConvergenceTests
    DiscoveryConvergenceTests.cpp
)

# ROMScan Node State Machine Tests
add_discovery_test(ROMScanNodeStateMachineTests
    ROMScanNodeStateMachineTests.cpp
)

# ROM Reader Mock Tests (moved from unit/)
add_executable(ROMReaderMockTests ROMReaderMockTests.cpp)
target_link_libraries(ROMReaderMockTests PRIVATE
    asfw_configrom_host
    asfw_test_support
    GTest::gtest_main
    GTest::gmock
)
gtest_discover_tests(ROMReaderMockTests)
