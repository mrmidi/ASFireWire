# CMakeLists.txt for ASFW Host-Side Tests
# Supports modular structure and CTest discovery

cmake_minimum_required(VERSION 3.23)

project(ASFWHostTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tests
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ASFW_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(ASFW_ROOT_DIR "${ASFW_TESTS_DIR}/..")
set(ASFW_DRIVER_DIR "${ASFW_ROOT_DIR}/ASFWDriver")
set(ASFW_DRIVERKIT_DIR "${ASFW_ROOT_DIR}/docs")

# Common include directories
set(ASFW_COMMON_INCLUDES
    "${ASFW_TESTS_DIR}/mocks"
    "${ASFW_TESTS_DIR}/support"  # Include support folder for headers like TestDataUtils.hpp
    "${ASFW_ROOT_DIR}"
    "${ASFW_DRIVER_DIR}"
    "${ASFW_DRIVER_DIR}/Async"
    "${ASFW_DRIVER_DIR}/Core"
    "${ASFW_DRIVER_DIR}/Bus"
    "${ASFW_DRIVER_DIR}/Logging"
    "${ASFW_DRIVERKIT_DIR}"
    "${ASFW_DRIVER_DIR}/Hardware"
    "${ASFW_DRIVER_DIR}/Testing"
    "${ASFW_DRIVER_DIR}/Discovery"
    "${ASFW_ROOT_DIR}/AppleHeaders"
)

enable_testing()

# Find or fetch GoogleTest
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

# Define common test settings interface library
add_library(asfw_test_common_interface INTERFACE)
target_include_directories(asfw_test_common_interface INTERFACE ${ASFW_COMMON_INCLUDES})
target_compile_definitions(asfw_test_common_interface INTERFACE ASFW_HOST_TEST)
target_compile_features(asfw_test_common_interface INTERFACE cxx_std_23)

# Define shared static libraries used by tests
# 1. Shared Test support library (universal stubs ONLY)
add_library(asfw_test_support STATIC
    "support/LoggingStubs.cpp"
    "support/HardwareInterfaceStub.cpp"
)
target_link_libraries(asfw_test_support PUBLIC asfw_test_common_interface)

# 2. Packet SerDes library (compiles serdes files + response sender stub)
add_library(asfw_packet_serdes STATIC
    "${ASFW_DRIVER_DIR}/Async/Tx/PacketBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/ARPacketParser.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/PacketRouter.cpp"
    "${ASFW_DRIVER_DIR}/Debug/AsyncTraceCapture.cpp"
    "support/ResponseSenderStub.cpp"
)
target_link_libraries(asfw_packet_serdes PUBLIC asfw_test_support)

# 3. Config ROM host library
add_library(asfw_configrom_host STATIC
    "${ASFW_DRIVER_DIR}/ConfigROM/Local/ConfigROMBuilder.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Parse/ConfigROMParser.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMReader.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Store/ConfigROMStore.cpp"
)
target_link_libraries(asfw_configrom_host PUBLIC asfw_test_support)

# 4. Discovery host library
add_library(asfw_discovery_host STATIC
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMScanner.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMScanSession.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMScanSessionDetails.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMScanSessionIRM.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Parse/ConfigROMParser.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Remote/ROMReader.cpp"
    "${ASFW_DRIVER_DIR}/Discovery/SpeedPolicy.cpp"
    "${ASFW_DRIVER_DIR}/ConfigROM/Store/ConfigROMStore.cpp"
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
)
target_link_libraries(asfw_discovery_host PUBLIC asfw_test_support)

# Include modular test subdirectories
add_subdirectory(support)
add_subdirectory(core)
add_subdirectory(async)
add_subdirectory(discovery)
add_subdirectory(irm)
add_subdirectory(protocols)
add_subdirectory(audio)
add_subdirectory(devices)
add_subdirectory(common)
