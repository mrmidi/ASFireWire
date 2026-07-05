# tests/async/CMakeLists.txt

macro(add_async_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# ASFW Packet Tests (using packet serdes library)
add_executable(ASFWPacketTests
    PacketSerDesTests.cpp
    AsyncPacketSerDesLinuxCompatTests.cpp
    CompareAndSwapPacketTests.cpp
)
target_link_libraries(ASFWPacketTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(ASFWPacketTests)

# TLabel Matching Tests
add_executable(TLabelMatchingTests
    TLabelMatchingTests.cpp
)
target_link_libraries(TLabelMatchingTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(TLabelMatchingTests)

# Completion Callback Pattern Tests
add_async_test(CompletionCallbackPatternTests
    CompletionCallbackPatternTests.cpp
)

# Completion Strategy Tests
add_async_test(CompletionStrategyTests
    CompletionStrategyTests.cpp
)

# Completion Refactor Plan Tests
add_async_test(CompletionRefactorPlanTests
    CompletionRefactorPlanTests.cpp
    "${ASFW_DRIVER_DIR}/Async/Core/Transaction.cpp"
    "${ASFW_DRIVER_DIR}/Async/Core/TransactionManager.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Shared/Memory/PayloadHandle.cpp"
)

# DMA Memory Tests
add_async_test(DMAMemoryTests
    DMAMemoryTests.cpp
)

# Descriptor Ring DMA Tests
add_async_test(DescriptorRingDMATests
    DescriptorRingDMATests.cpp
    "${ASFW_DRIVER_DIR}/Shared/Rings/DescriptorRing.cpp"
)

# AT Descriptor Tests
add_async_test(ATDescriptorTests
    ATDescriptorTests.cpp
)

# Buffer Ring DMA Tests
add_async_test(BufferRingDMATests
    BufferRingDMATests.cpp
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/ARPacketParser.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

# AR Stream Processor Tests (bufferFill walker: boundary stitching, garbage policy)
add_async_test(ARStreamProcessorTests
    ARStreamProcessorTests.cpp
    "${ASFW_DRIVER_DIR}/Shared/Rings/BufferRing.cpp"
    "${ASFW_DRIVER_DIR}/Async/Rx/ARPacketParser.cpp"
    "${ASFW_DRIVER_DIR}/Common/BarrierUtils.cpp"
)

# Label Allocator Tests
add_async_test(LabelAllocatorTests
    LabelAllocatorTests.cpp
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
)

# Async Bus Contract Tests
add_async_test(AsyncBusContractTests
    AsyncBusContractTests.cpp
    "../support/AsyncSubsystemContractStub.cpp"
    "${ASFW_DRIVER_DIR}/Async/FireWireBusImpl.cpp"
    "${ASFW_DRIVER_DIR}/Bus/TopologyManager.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDStreamParser.cpp"
    "${ASFW_DRIVER_DIR}/Bus/SelfIDTopologyNormalizer.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/PayloadRegistry.cpp"
    "${ASFW_DRIVER_DIR}/Async/Core/Transaction.cpp"
    "${ASFW_DRIVER_DIR}/Async/Core/TransactionManager.cpp"
)

# Packet Field Extraction Tests
add_executable(PacketFieldExtractionTests
    PacketFieldExtractionTests.cpp
)
target_link_libraries(PacketFieldExtractionTests PRIVATE
    asfw_packet_serdes
    asfw_test_support
    GTest::gtest_main
)
gtest_discover_tests(PacketFieldExtractionTests)

# Response Sender Header Format Tests
add_async_test(ResponseSenderHeaderFormatTests
    ResponseSenderHeaderFormatTests.cpp
)

# Async Subsystem Accessor Tests
add_async_test(AsyncSubsystemAccessorTests
    AsyncSubsystemAccessorTests.cpp
    "../support/AsyncSubsystemStub.cpp"
    "${ASFW_DRIVER_DIR}/Async/Track/LabelAllocator.cpp"
    "${ASFW_DRIVER_DIR}/Bus/GenerationTracker.cpp"
)
