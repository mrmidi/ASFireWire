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
    "${ASFW_ROOT_DIR}"
    "${ASFW_DRIVER_DIR}"
    "${ASFW_DRIVER_DIR}/Async"
    "${ASFW_DRIVER_DIR}/Core"
    "${ASFW_DRIVER_DIR}/Bus"
    "${ASFW_DRIVERKIT_DIR}"
    "${ASFW_DRIVER_DIR}/Hardware"
    "${ASFW_ROOT_DIR}/AppleHeaders"
    "${ASFW_TESTS_DIR}/mocks"
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

# Minimal library for packet serdes only
add_library(asfw_packet_serdes STATIC
    "${ASFW_DRIVER_DIR}/Async/Tx/PacketBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/ARPacketParser.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/PacketRouter.cpp"
    "${ASFW_TESTS_DIR}/LoggingStubs.cpp"
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

include(GoogleTest)
gtest_discover_tests(ASFWPacketTests)
gtest_discover_tests(TLabelMatchingTests)
gtest_discover_tests(CompletionCallbackPatternTests)
gtest_discover_tests(CompletionStrategyTests)
gtest_discover_tests(GapCountOptimizerTests)
gtest_discover_tests(TopologyGapExtractionTests)
gtest_discover_tests(PhyPacketsTests)
gtest_discover_tests(LabelAllocatorTests)
