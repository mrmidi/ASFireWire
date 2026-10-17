# Platform-neutral analysis cores: timeline audit, admission, detector,
# aggregation, evidence JSON, self-test (rtl_core), the tone glitch detector
# (tone_core) and its per-callback IO record (tone_io). Unit-tested on any host.
add_library(asfw_rtl_core STATIC rtl_core.c tone_core.c tone_io.c)
target_include_directories(asfw_rtl_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(asfw_rtl_core PRIVATE ${ASFW_TOOLS_WARNINGS})
if (NOT MSVC)
    target_link_libraries(asfw_rtl_core PUBLIC m)
endif()

if (APPLE)
    # The one CoreAudio IO implementation: linked by the command line here and
    # compiled into the ASFW app by project.yml.
    add_library(asfw_rtl_session STATIC rtl_session.c)
    target_compile_options(asfw_rtl_session PRIVATE ${ASFW_TOOLS_WARNINGS})
    target_link_libraries(asfw_rtl_session PUBLIC asfw_rtl_core
        "-framework CoreAudio" "-framework CoreFoundation")

    add_executable(rtl_loopback rtl_loopback.c)
    target_compile_options(rtl_loopback PRIVATE ${ASFW_TOOLS_WARNINGS})
    target_compile_definitions(rtl_loopback PRIVATE RTL_GIT_SHA="${ASFW_TOOLS_GIT_SHA}")
    target_link_libraries(rtl_loopback PRIVATE asfw_rtl_session)
endif()
