# Platform-neutral maths and evidence record for the HAL probe. Reuses the RTL
# core for the declared round-trip composition and statistics.
add_library(asfw_hal_core STATIC hal_core.c)
target_include_directories(asfw_hal_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(asfw_hal_core PRIVATE ${ASFW_TOOLS_WARNINGS})
target_link_libraries(asfw_hal_core PUBLIC asfw_rtl_core)

if (APPLE)
    add_executable(hal_geometry hal_geometry.c)
    target_compile_options(hal_geometry PRIVATE ${ASFW_TOOLS_WARNINGS} -Wno-four-char-constants -Wno-multichar)
    target_compile_definitions(hal_geometry PRIVATE HAL_GIT_SHA="${ASFW_TOOLS_GIT_SHA}")
    target_link_libraries(hal_geometry PRIVATE asfw_hal_core
        "-framework CoreAudio" "-framework CoreFoundation")
endif()
