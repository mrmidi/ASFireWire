# tests/irm/CMakeLists.txt

macro(add_irm_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# Broadcast Channel CSR Tests
add_irm_test(BroadcastChannelCSRTests
    BroadcastChannelCSRTests.cpp
)

# Local IRM Resource CSR Handler Tests
add_irm_test(LocalIRMResourceCSRHandlerTests
    LocalIRMResourceCSRHandlerTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalIRMResourceCSRHandler.cpp"
)

# Local IRM Resource Controller Tests
add_irm_test(LocalIRMResourceControllerTests
    LocalIRMResourceControllerTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalIRMResourceController.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalCSRAccessor.cpp"
)

# IRM Fallback Coordinator Tests
add_irm_test(IRMFallbackCoordinatorTests
    IRMFallbackCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/IRM/IRMFallbackCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalCSRAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Bus/Timing/PostResetTimingCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Scheduling/Scheduler.cpp"
    "../support/BusResetCoordinatorStub.cpp"
)
