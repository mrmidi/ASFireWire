# tests/core/CMakeLists.txt

macro(add_core_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# Gap Count Optimizer Tests
add_core_test(GapCountOptimizerTests
    GapCountOptimizerTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/GapCountOptimizer.cpp"
)

# Topology Gap Extraction Tests
add_core_test(TopologyGapExtractionTests
    TopologyGapExtractionTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
)

# Topology Manager Tests
add_core_test(TopologyManagerTests
    TopologyManagerTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
)

# SelfID Topology Normalizer Tests
add_core_test(SelfIDTopologyNormalizerTests
    SelfIDTopologyNormalizerTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GapCountOptimizer.cpp"
)

# SelfID Stream Parser Tests
add_core_test(SelfIDStreamParserTests
    SelfIDStreamParserTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
)

# SelfID Sequence Tests
add_core_test(SelfIDSequenceTests
    SelfIDSequenceTests.cpp
)

# SelfID Capture Tests
add_core_test(SelfIDCaptureTests
    SelfIDCaptureTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/SelfIDCapture.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

# Bus Manager Gap Optimization Tests
add_core_test(BusManagerGapOptimizationTests
    BusManagerGapOptimizationTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GapCountOptimizer.cpp"
    "${ASFW_DRIVER_DIR}/Controller/ControllerConfig.cpp"
)

# Bus Manager Election Tests
add_core_test(BusManagerElectionTests
    BusManagerElectionTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/BusManagerElection.cpp"
    "${ASFW_DRIVER_DIR}/Bus/BusManager/BusManagerElectionDriver.cpp"
    "../support/BusResetCoordinatorStub.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalIRMResourceController.cpp"
    "${ASFW_DRIVER_DIR}/Bus/IRM/LocalCSRAccessor.cpp"
    "${ASFW_DRIVER_DIR}/Bus/Timing/PostResetTimingCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Controller/ControllerConfig.cpp"
    "${ASFW_DRIVER_DIR}/Scheduling/Scheduler.cpp"
)

# Bus Reset Coordinator Tests
add_core_test(BusResetCoordinatorTests
    BusResetCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusResetCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/BusResetCoordinatorFSM.cpp"
    "${ASFW_DRIVER_DIR}/Bus/BusResetCoordinatorActions.cpp"
    "${ASFW_DRIVER_DIR}/Bus/Timing/PostResetTimingCoordinator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/BusResetCoordinatorDiscoveryDelay.cpp"
    "${ASFW_DRIVER_DIR}/Bus/BusManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GapCountOptimizer.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDCapture.cpp"
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapService.cpp"
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapBuilder.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
    "${ASFW_DRIVER_DIR}/Hardware/InterruptManager.cpp"
    "../support/BusResetCoordinatorDepsStubs.cpp"
)

# Role Policy Tests
add_core_test(RolePolicyTests
    RolePolicyTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/Role/RolePolicy.cpp"
)

# Role Coordinator Tests
add_core_test(RoleCoordinatorTests
    RoleCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/Role/RolePolicy.cpp"
    "${ASFW_DRIVER_DIR}/Bus/Role/RoleCoordinator.cpp"
)

# Cycle Observer Tests
add_core_test(CycleObserverTests
    CycleObserverTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/Role/CycleObserver.cpp"
)

# PHY Packets Tests
add_core_test(PhyPacketsTests
    PhyPacketsTests.cpp
)

# Timing Utils Tests
add_core_test(TimingUtilsTests
    TimingUtilsTests.cpp
)

# Post-Reset Timing Tests
add_core_test(PostResetTimingTests
    PostResetTimingTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/Timing/PostResetTimingCoordinator.cpp"
)

# Interrupt Mask Shadow Tests
add_core_test(InterruptMaskShadowTests
    InterruptMaskShadowTests.cpp
    "${ASFW_DRIVER_DIR}/Hardware/InterruptManager.cpp"
)
target_link_libraries(InterruptMaskShadowTests PRIVATE GTest::gmock)

# Topology Map Builder Tests
add_core_test(TopologyMapBuilderTests
    TopologyMapBuilderTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/CSR/TopologyMapBuilder.cpp"
)

# Cycle Policy Coordinator Tests
add_core_test(CyclePolicyCoordinatorTests
    CyclePolicyCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/CyclePolicyCoordinator.cpp"
)

# Cycle Policy Executor Tests
add_core_test(CyclePolicyExecutorTests
    CyclePolicyExecutorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/CyclePolicyCoordinator.cpp"
)
target_link_libraries(CyclePolicyExecutorTests PRIVATE GTest::gmock)

# Root Selection Coordinator Tests
add_core_test(RootSelectionCoordinatorTests
    RootSelectionCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/RootSelectionCoordinator.cpp"
)
target_link_libraries(RootSelectionCoordinatorTests PRIVATE GTest::gmock)

# Gap Policy Coordinator Tests
add_core_test(GapPolicyCoordinatorTests
    GapPolicyCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/GapPolicyCoordinator.cpp"
)
target_link_libraries(GapPolicyCoordinatorTests PRIVATE GTest::gmock)

# Power/Link Policy Coordinator Tests
add_core_test(PowerLinkPolicyCoordinatorTests
    PowerLinkPolicyCoordinatorTests.cpp
    "${ASFW_DRIVER_DIR}/Bus/BusManager/PowerLinkPolicyCoordinator.cpp"
)
target_link_libraries(PowerLinkPolicyCoordinatorTests PRIVATE GTest::gmock)

# Hardware Interface Order Tests
add_core_test(HardwareInterfaceOrderTests
    HardwareInterfaceOrderTests.cpp
    "${ASFW_DRIVER_DIR}/Hardware/HardwareInterface.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)
target_link_libraries(HardwareInterfaceOrderTests PRIVATE GTest::gmock)
