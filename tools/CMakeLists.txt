# Host measurement tools (Epic FW-169).
#
#   cmake -S tools -B build/tools && cmake --build build/tools
#
# The pure analysis cores build everywhere and are unit-tested from
# tests/tools. The CoreAudio front ends build on macOS only.
cmake_minimum_required(VERSION 3.23)
project(ASFWMeasurementTools LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # POSIX/BSD calls in the macOS front ends

# Stamp binaries with the commit they were built from so evidence files can
# name it. Falls back to "unknown" outside a git checkout.
find_package(Git QUIET)
set(ASFW_TOOLS_GIT_SHA "unknown")
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        RESULT_VARIABLE _rc)
    if (_rc EQUAL 0 AND _sha)
        set(ASFW_TOOLS_GIT_SHA "${_sha}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain --untracked-files=no -- .
            OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (_dirty)
            set(ASFW_TOOLS_GIT_SHA "${ASFW_TOOLS_GIT_SHA}-dirty")
        endif()
    endif()
endif()

if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(ASFW_TOOLS_WARNINGS -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(rtl)
