# Host measurement tools (Epic FW-169): the platform-neutral analysis cores of
# tools/rtl and tools/halprobe, plus the Python analysers.
#
# The cores are C11. They are compiled here directly (rather than through
# tools/CMakeLists.txt) so they inherit this suite's sanitizer flags.
enable_language(C)

set(ASFW_TOOLS_DIR "${ASFW_ROOT_DIR}/tools")

add_library(asfw_rtl_core_host STATIC "${ASFW_TOOLS_DIR}/rtl/rtl_core.c")
set_target_properties(asfw_rtl_core_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(asfw_rtl_core_host PUBLIC "${ASFW_TOOLS_DIR}/rtl")
target_compile_options(asfw_rtl_core_host PRIVATE -Wall -Wextra)
if (NOT MSVC)
    target_link_libraries(asfw_rtl_core_host PUBLIC m)
endif()

add_executable(RtlCoreTests RtlCoreTests.cpp)
target_link_libraries(RtlCoreTests PRIVATE
    asfw_rtl_core_host
    asfw_test_common_interface
    GTest::gtest_main
)
gtest_discover_tests(RtlCoreTests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME AnalyzeRtlPyTests
        COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_analyze_rtl
        WORKING_DIRECTORY "${ASFW_TOOLS_DIR}/rtl")
endif()

add_library(asfw_hal_core_host STATIC "${ASFW_TOOLS_DIR}/halprobe/hal_core.c")
set_target_properties(asfw_hal_core_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(asfw_hal_core_host PUBLIC "${ASFW_TOOLS_DIR}/halprobe")
target_compile_options(asfw_hal_core_host PRIVATE -Wall -Wextra)
target_link_libraries(asfw_hal_core_host PUBLIC asfw_rtl_core_host)

add_executable(HalCoreTests HalCoreTests.cpp)
target_link_libraries(HalCoreTests PRIVATE
    asfw_hal_core_host
    asfw_test_common_interface
    GTest::gtest_main
)
gtest_discover_tests(HalCoreTests)
