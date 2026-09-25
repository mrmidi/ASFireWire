# Platform-neutral analysis core: timeline audit, admission, detector,
# aggregation, evidence JSON, self-test. Unit-tested on any host.
add_library(asfw_rtl_core STATIC rtl_core.c)
target_include_directories(asfw_rtl_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(asfw_rtl_core PRIVATE ${ASFW_TOOLS_WARNINGS})
if (NOT MSVC)
    target_link_libraries(asfw_rtl_core PUBLIC m)
endif()

if (APPLE)
    add_executable(rtl_loopback rtl_loopback.c)
    target_compile_options(rtl_loopback PRIVATE ${ASFW_TOOLS_WARNINGS})
    target_compile_definitions(rtl_loopback PRIVATE RTL_GIT_SHA="${ASFW_TOOLS_GIT_SHA}")
    target_link_libraries(rtl_loopback PRIVATE asfw_rtl_core
        "-framework CoreAudio" "-framework CoreFoundation")
endif()
